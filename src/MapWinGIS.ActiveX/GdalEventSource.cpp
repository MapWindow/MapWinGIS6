// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEventSource.cpp
 * @brief Implements GDAL owner-thread dispatch and COM event delivery.
 *
 * Creates a message window for native worker wakeups and delivers queued
 * operation notifications to the control's connection-point sinks.
 */
#include "GdalEventSource.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>

#include "ActiveXModule.h"
#include "ComConversion.h"

namespace mwg::activex
{
    namespace
    {
        constexpr DISPID kGdalProgressDispId = 1;
        constexpr DISPID kGdalOperationCompletedDispId = 2;
        constexpr std::size_t kMaxEventArguments = 3;

        [[nodiscard]] std::string FormatHresult(const HRESULT errorCode)
        {
            std::ostringstream message;
            message << "COM event callback failed with HRESULT 0x"
                    << std::uppercase << std::hex << std::setw(8)
                    << std::setfill('0')
                    << static_cast<std::uint32_t>(errorCode) << ".";
            return message.str();
        }
    }

    GdalEventSource::GdalEventSource(
        ATL::CComDynamicUnkArray& sinks,
        const GdalDispatchWindow::Callback callback,
        void* const context)
        : _sinks(sinks),
          _dispatchCallback(callback),
          _dispatchContext(context)
    {
    }

    GdalEventSource::~GdalEventSource()
    {
        Shutdown();
        DestroyDispatchWindow();
    }

    HRESULT GdalEventSource::EnsureDispatchWindow() noexcept
    {
        if (_closing)
        {
            return E_UNEXPECTED;
        }
        try
        {
            if (_dispatchWindow == nullptr)
            {
                _dispatchWindow = std::make_shared<GdalDispatchWindow>();
            }
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        if (!_dispatchWindow->IsWindow() &&
            _dispatchWindow->Create(
                _dispatchCallback,
                _dispatchContext) == nullptr)
        {
            const DWORD error = ::GetLastError();
            return error != ERROR_SUCCESS
                ? HRESULT_FROM_WIN32(error)
                : E_FAIL;
        }
        return S_OK;
    }

    HRESULT GdalEventSource::SetAnimationTimer(const bool enabled) noexcept
    {
        return _dispatchWindow == nullptr
            ? E_UNEXPECTED
            : _dispatchWindow->SetAnimationTimer(enabled);
    }

    std::uint64_t GdalEventSource::Begin(
        std::string algorithmPath,
        std::vector<std::string> arguments)
    {
        if (_closing || _dispatchWindow == nullptr ||
            !_dispatchWindow->IsWindow())
        {
            throw std::runtime_error(
                "The GDAL event source is not available for new operations.");
        }

        _dispatcher.SetWakeCallback(
            &GdalEventSource::PostDispatchMessage,
            _dispatchWindow->m_hWnd);
        _dispatcher.SetWorkerFinishedCallback(
            &GdalEventSource::UnlockActiveXModule,
            nullptr);
        _AtlModule.Lock();
        try
        {
            return _dispatcher.Begin(
                std::move(algorithmPath),
                std::move(arguments));
        }
        catch (...)
        {
            _AtlModule.Unlock();
            throw;
        }
    }

    GdalCancelResult GdalEventSource::Cancel(
        const std::uint64_t operationId) noexcept
    {
        return _dispatcher.Cancel(operationId);
    }

    GdalOperationLookup GdalEventSource::GetResult(
        const std::uint64_t operationId) const
    {
        return _dispatcher.GetResult(operationId);
    }

    GdalOperationLookup GdalEventSource::End(const std::uint64_t operationId)
    {
        return _dispatcher.End(operationId);
    }

    void GdalEventSource::RecordCallbackError(
        const std::uint64_t operationId,
        const HRESULT result) noexcept
    {
        try
        {
            _dispatcher.RecordCallbackError(
                operationId,
                result,
                FormatHresult(result));
        }
        catch (const std::bad_alloc&)
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to retain a COM event callback failure.\n");
        }
    }

    void GdalEventSource::OnSize() noexcept
    {
        if (!_closing && _dispatchWindow != nullptr &&
            _dispatchWindow->IsWindow())
        {
            _dispatcher.SetWakeCallback(
                &GdalEventSource::PostDispatchMessage,
                _dispatchWindow->m_hWnd);
        }
    }

    void GdalEventSource::OnDestroy() noexcept
    {
        if (_closing)
        {
            _dispatcher.Shutdown();
            DestroyDispatchWindow();
        }
        else
        {
            _dispatcher.Suspend();
        }
    }

    LRESULT GdalEventSource::OnDispatch(BOOL& handled)
    {
        handled = TRUE;
        if (_dispatching || !_dispatcher.IsDispatchActive())
        {
            return 0;
        }

        _dispatching = true;
        struct DispatchGuard final
        {
            GdalEventSource& source;

            ~DispatchGuard()
            {
                source._dispatching = false;
                if (source._dispatchWindowDestroyPending)
                {
                    source.DestroyDispatchWindow();
                }
            }
        } dispatchGuard{*this};

        while (_dispatcher.IsDispatchActive())
        {
            const auto notifications = _dispatcher.DrainNotifications();
            if (notifications.empty())
            {
                break;
            }

            for (const GdalOperationNotification& notification : notifications)
            {
                if (!_dispatcher.IsDispatchActive())
                {
                    break;
                }
                if (!_dispatcher.BeginNotificationDelivery(notification.operationId))
                {
                    continue;
                }

                struct NotificationGuard final
                {
                    GdalOperationDispatcher& dispatcher;
                    std::uint64_t operationId;

                    ~NotificationGuard()
                    {
                        dispatcher.EndNotificationDelivery(operationId);
                    }
                } notificationGuard{_dispatcher, notification.operationId};

                const HRESULT result =
                    notification.kind == GdalNotificationKind::Progress
                    ? FireProgress(notification)
                    : FireCompleted(notification.operationId);
                if (FAILED(result))
                {
                    RecordCallbackError(notification.operationId, result);
                }
            }
        }
        return 0;
    }

    void GdalEventSource::Close() noexcept
    {
        _closing = true;
        _dispatcher.Shutdown();
        DestroyDispatchWindow();
    }

    void GdalEventSource::Shutdown() noexcept
    {
        _dispatcher.Shutdown();
    }

    bool GdalEventSource::PostDispatchMessage(void* const window) noexcept
    {
        return ::PostMessageW(
            static_cast<HWND>(window),
            kGdalDispatchMessage,
            0,
            0) != FALSE;
    }

    void GdalEventSource::UnlockActiveXModule(void*) noexcept
    {
        _AtlModule.Unlock();
    }

    void GdalEventSource::DestroyDispatchWindow() noexcept
    {
        if (_dispatching)
        {
            _dispatchWindowDestroyPending = true;
            return;
        }

        _dispatchWindowDestroyPending = false;
        if (_dispatchWindow != nullptr)
        {
            _dispatchWindow->Destroy();
        }
    }

    HRESULT GdalEventSource::FireProgress(
        const GdalOperationNotification& notification)
    {
        BSTR message = nullptr;
        HRESULT result = ComConversion::Utf8ToBstr(notification.message, &message);
        if (FAILED(result))
        {
            return result;
        }

        VARIANTARG arguments[3]{};
        arguments[0].vt = VT_BSTR;
        arguments[0].bstrVal = message;
        arguments[1].vt = VT_R8;
        arguments[1].dblVal = notification.progress;
        arguments[2].vt = VT_I8;
        arguments[2].llVal = static_cast<LONGLONG>(notification.operationId);
        DISPPARAMS parameters{arguments, nullptr, 3, 0};
        result = FireEvent(
            kGdalProgressDispId,
            parameters,
            notification.operationId);
        ::SysFreeString(message);
        return result;
    }

    HRESULT GdalEventSource::FireCompleted(const std::uint64_t operationId)
    {
        VARIANTARG argument{};
        argument.vt = VT_I8;
        argument.llVal = static_cast<LONGLONG>(operationId);
        DISPPARAMS parameters{&argument, nullptr, 1, 0};
        return FireEvent(
            kGdalOperationCompletedDispId,
            parameters,
            operationId);
    }

    HRESULT GdalEventSource::FireEvent(
        const DISPID eventId,
        DISPPARAMS& parameters,
        const std::uint64_t operationId)
    {
        if (parameters.cArgs > kMaxEventArguments)
        {
            return E_INVALIDARG;
        }

        std::vector<ATL::CComPtr<IUnknown>> sinks;
        try
        {
            sinks.reserve(static_cast<std::size_t>(_sinks.GetSize()));
            for (int index = 0; index < _sinks.GetSize(); ++index)
            {
                IUnknown* sink = _sinks.GetAt(index);
                if (sink != nullptr)
                {
                    sinks.emplace_back(sink);
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        HRESULT firstError = S_OK;
        for (const ATL::CComPtr<IUnknown>& unknown : sinks)
        {
            if (!_dispatcher.IsDispatchActive())
            {
                break;
            }
            ATL::CComQIPtr<IDispatch> sink(unknown);
            if (sink == nullptr)
            {
                if (SUCCEEDED(firstError))
                {
                    firstError = E_NOINTERFACE;
                }
                RecordCallbackError(operationId, E_NOINTERFACE);
                continue;
            }

            std::array<VARIANTARG, kMaxEventArguments> arguments{};
            UINT copiedArguments = 0;
            HRESULT result = S_OK;
            for (; copiedArguments < parameters.cArgs; ++copiedArguments)
            {
                result = ::VariantCopy(
                    &arguments[copiedArguments],
                    &parameters.rgvarg[copiedArguments]);
                if (FAILED(result))
                {
                    break;
                }
            }
            if (FAILED(result))
            {
                for (UINT index = 0; index < copiedArguments; ++index)
                {
                    ::VariantClear(&arguments[index]);
                }
                if (SUCCEEDED(firstError))
                {
                    firstError = result;
                }
                RecordCallbackError(operationId, result);
                continue;
            }

            DISPPARAMS sinkParameters{
                arguments.data(),
                parameters.rgdispidNamedArgs,
                parameters.cArgs,
                parameters.cNamedArgs};
            const HRESULT invokeResult = sink->Invoke(
                eventId,
                IID_NULL,
                LOCALE_USER_DEFAULT,
                DISPATCH_METHOD,
                &sinkParameters,
                nullptr,
                nullptr,
                nullptr);
            for (UINT index = 0; index < copiedArguments; ++index)
            {
                ::VariantClear(&arguments[index]);
            }
            if (FAILED(invokeResult))
            {
                if (SUCCEEDED(firstError))
                {
                    firstError = invokeResult;
                }
                RecordCallbackError(operationId, invokeResult);
            }
        }
        return firstError;
    }
}
