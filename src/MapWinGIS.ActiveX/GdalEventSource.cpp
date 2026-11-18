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
            _dispatchWindow.get());
        _dispatcher.SetWorkerFinishedCallback(
            &GdalEventSource::UnlockActiveXModule,
            nullptr);
        ArmRetryTimer();
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
            UpdateRetryTimer();
            throw;
        }
    }

    std::uint64_t GdalEventSource::BeginWork(
        GdalOperationDispatcher::WorkCallback work)
    {
        if (_closing || _dispatchWindow == nullptr ||
            !_dispatchWindow->IsWindow())
        {
            throw std::runtime_error(
                "The GDAL event source is not available for new operations.");
        }

        _dispatcher.SetWakeCallback(
            &GdalEventSource::PostDispatchMessage,
            _dispatchWindow.get());
        _dispatcher.SetWorkerFinishedCallback(
            &GdalEventSource::UnlockActiveXModule,
            nullptr);
        ArmRetryTimer();
        _AtlModule.Lock();
        try
        {
            return _dispatcher.BeginWork(std::move(work), true);
        }
        catch (...)
        {
            _AtlModule.Unlock();
            UpdateRetryTimer();
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

    GdalOperationLookup GdalEventSource::GetResultForDelivery(
        const std::uint64_t operationId) const
    {
        return _dispatcher.GetResultForDelivery(operationId);
    }

    void GdalEventSource::UpdateCompletedResult(
        const std::uint64_t operationId,
        GdalOperationSnapshot result)
    {
        _dispatcher.UpdateCompletedResult(operationId, std::move(result));
    }

    GdalOperationLookup GdalEventSource::End(const std::uint64_t operationId)
    {
        GdalOperationLookup result = _dispatcher.End(operationId);
        UpdateRetryTimer();
        return result;
    }

    void GdalEventSource::SetCompletionCallback(
        const CompletionCallback callback,
        void* const context) noexcept
    {
        _completionCallback = callback;
        _completionContext = context;
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
                _dispatchWindow.get());
            UpdateRetryTimer();
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
        if (_dispatching)
        {
            // A sink pumped messages and consumed a wake meant for work queued
            // after the current batch was drained. Re-post it once that batch ends.
            _redispatchRequested = true;
            return 0;
        }
        if (!_dispatcher.IsDispatchActive())
        {
            return 0;
        }

        _dispatching = true;
        _redispatchRequested = false;
        const std::uint64_t generation = _dispatcher.GetGeneration();
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

        // Deliver one batch per message. Notifications queued while the sinks
        // run post their own wake message, so the owner thread returns to its
        // message loop between batches instead of looping here for as long as
        // a fast-reporting operation keeps producing progress.
        const auto notifications = _dispatcher.DrainNotifications();
        for (const GdalOperationNotification& notification : notifications)
        {
            if (_dispatcher.GetGeneration() != generation ||
                !_dispatcher.IsDispatchActive())
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
                std::uint64_t generation;

                ~NotificationGuard()
                {
                    if (dispatcher.GetGeneration() == generation)
                    {
                        dispatcher.EndNotificationDelivery(operationId);
                    }
                }
            } notificationGuard{_dispatcher, notification.operationId, generation};

            if (notification.kind == GdalNotificationKind::Completed &&
                _completionCallback != nullptr)
            {
                _completionCallback(_completionContext, notification.operationId);
            }
            const HRESULT result =
                notification.kind == GdalNotificationKind::Progress
                ? FireProgress(notification, generation)
                : FireCompleted(notification.operationId, generation);
            if (FAILED(result) && _dispatcher.GetGeneration() == generation)
            {
                RecordCallbackError(notification.operationId, result);
            }
            if (_dispatcher.GetGeneration() != generation)
            {
                break;
            }
        }
        if (_dispatcher.GetGeneration() == generation)
        {
            if (_redispatchRequested && _dispatchWindow != nullptr &&
                !_dispatchWindowDestroyPending && _dispatcher.IsDispatchActive() &&
                !_dispatchWindow->PostWake())
            {
                ArmRetryTimer();
            }
            _redispatchRequested = false;
            UpdateRetryTimer();
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

    HRESULT GdalEventSource::Reactivate() noexcept
    {
        if (!_closing) return S_OK;
        try
        {
            _dispatcher.Restart();
            _closing = false;
            _dispatchWindowDestroyPending = false;
            return S_OK;
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_UNEXPECTED; }
    }

    bool GdalEventSource::PostDispatchMessage(void* const window) noexcept
    {
        return static_cast<GdalDispatchWindow*>(window)->PostWake();
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
            _dispatchWindow.reset();
        }
    }

    void GdalEventSource::ArmRetryTimer() noexcept
    {
        if (_dispatchWindow != nullptr && !_dispatchWindow->StartRetryTimer())
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to arm the GDAL operation retry timer.\n");
        }
    }

    void GdalEventSource::UpdateRetryTimer() noexcept
    {
        if (_dispatchWindow == nullptr)
        {
            return;
        }
        if (_dispatcher.HasOutstandingOperations())
        {
            ArmRetryTimer();
        }
        else
        {
            _dispatchWindow->StopRetryTimer();
        }
    }

    HRESULT GdalEventSource::FireProgress(
        const GdalOperationNotification& notification,
        const std::uint64_t generation)
    {
        BSTR message = nullptr;
        HRESULT result = ComConversion::DiagnosticToBstr(notification.message, &message);
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
            notification.operationId,
            generation);
        ::SysFreeString(message);
        return result;
    }

    HRESULT GdalEventSource::FireCompleted(
        const std::uint64_t operationId,
        const std::uint64_t generation)
    {
        VARIANTARG argument{};
        argument.vt = VT_I8;
        argument.llVal = static_cast<LONGLONG>(operationId);
        DISPPARAMS parameters{&argument, nullptr, 1, 0};
        return FireEvent(
            kGdalOperationCompletedDispId,
            parameters,
            operationId,
            generation);
    }

    HRESULT GdalEventSource::FireEvent(
        const DISPID eventId,
        DISPPARAMS& parameters,
        const std::uint64_t operationId,
        const std::uint64_t generation)
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
            if (_dispatcher.GetGeneration() != generation ||
                !_dispatcher.IsDispatchActive())
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
                if (_dispatcher.GetGeneration() == generation)
                {
                    RecordCallbackError(operationId, E_NOINTERFACE);
                }
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
                if (_dispatcher.GetGeneration() == generation)
                {
                    RecordCallbackError(operationId, result);
                }
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
            if (_dispatcher.GetGeneration() != generation)
            {
                break;
            }
            if (FAILED(invokeResult))
            {
                if (SUCCEEDED(firstError))
                {
                    firstError = invokeResult;
                }
                if (_dispatcher.GetGeneration() == generation)
                {
                    RecordCallbackError(operationId, invokeResult);
                }
            }
        }
        return firstError;
    }
}
