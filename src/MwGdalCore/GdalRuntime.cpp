// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalRuntime.cpp
 * @brief Implements the GDAL runtime foundation.
 *
 * Provides process-safe driver registration, configuration scopes, structured
 * error capture, and cancellation-aware progress callbacks.
 */
#include "mapwingis/gdal/GdalRuntime.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "internal/GdalExecution.h"

#include <mutex>
#include <stdexcept>
#include <utility>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>

namespace mwg
{
    namespace detail
    {
        std::recursive_mutex& GdalExecutionMutex()
        {
            static std::recursive_mutex mutex;
            return mutex;
        }

        bool& GdalCallbackActive()
        {
            static thread_local bool active = false;
            return active;
        }
    }

    namespace
    {
        std::once_flag g_registrationFlag;

        void ValidateOptionName(const std::string_view name)
        {
            if (name.empty() || name.find('\0') != std::string_view::npos)
                throw std::invalid_argument("GDAL configuration option name must be non-empty and contain no null bytes.");
        }

        std::optional<std::string> GetGlobalConfigOption(const std::string& name)
        {
            using ConfigOptionsPtr = std::unique_ptr<char*, void(*)(char**)>;
            ConfigOptionsPtr configOptions(CPLGetConfigOptions(), &CSLDestroy);
            if (const char* value = CSLFetchNameValue(configOptions.get(), name.c_str()))
                return value;
            return std::nullopt;
        }
    }

    struct GdalErrorScope::Impl
    {
        std::vector<GdalError> errors;
        std::exception_ptr captureException;

        static void CPL_STDCALL Handle(const CPLErr errorClass, const CPLErrorNum errorCode, const char* message)
        {
            void* userData = CPLGetErrorHandlerUserData();
            if (message == nullptr || userData == nullptr)
                return;

            auto& scope = *static_cast<Impl*>(userData);
            if (scope.captureException)
                return;
            try
            {
                scope.errors.push_back(
                    {static_cast<GdalErrorClass>(errorClass), static_cast<int>(errorCode), message});
                const LogLevel level = errorClass >= CE_Failure ? LogLevel::Error
                    : errorClass == CE_Warning ? LogLevel::Warning : LogLevel::Debug;
                GdalEnvironment::ReportDiagnostic(level, message);
            }
            catch (...)
            {
                scope.captureException = std::current_exception();
            }
        }
    };

    GdalInitializer::GdalInitializer()
    {
        std::call_once(g_registrationFlag, [] { GDALAllRegister(); });
    }

    GdalInitializer::~GdalInitializer() = default;

    void GdalConfig::SetOption(const std::string_view name, const std::string_view value)
    {
        ValidateOptionName(name);
        if (value.find('\0') != std::string_view::npos)
            throw std::invalid_argument("GDAL configuration option value cannot contain null bytes.");
        const std::string ownedName(name);
        const std::string ownedValue(value);
        CPLSetConfigOption(ownedName.c_str(), ownedValue.c_str());
    }

    void GdalConfig::ClearOption(const std::string_view name)
    {
        ValidateOptionName(name);
        const std::string ownedName(name);
        CPLSetConfigOption(ownedName.c_str(), nullptr);
    }

    ScopedGdalConfigOption::ScopedGdalConfigOption(const std::string_view name, const std::string_view value)
        : _name(name)
    {
        ValidateOptionName(name);
        _previousValue = GetGlobalConfigOption(_name);
        GdalConfig::SetOption(_name, value);
    }

    ScopedGdalConfigOption::~ScopedGdalConfigOption()
    {
        CPLSetConfigOption(_name.c_str(), _previousValue ? _previousValue->c_str() : nullptr);
    }

    GdalErrorScope::GdalErrorScope()
        : _impl(std::make_unique<Impl>())
    {
        CPLPushErrorHandlerEx(&Impl::Handle, _impl.get());
    }

    GdalErrorScope::~GdalErrorScope()
    {
        CPLPopErrorHandler();
    }

    int GdalErrorScope::LastErrorCode() const noexcept
    {
        return _impl->errors.empty() ? 0 : _impl->errors.back().code;
    }

    std::string GdalErrorScope::LastErrorMessage() const
    {
        return _impl->errors.empty() ? std::string{} : _impl->errors.back().message;
    }

    std::string GdalErrorScope::DetailedErrorMessage() const
    {
        std::string details;
        for (const GdalError& error : _impl->errors)
        {
            if (!details.empty())
                details += '\n';
            details += "GDAL error (class ";
            details += std::to_string(static_cast<int>(error.errorClass));
            details += ", code ";
            details += std::to_string(error.code);
            details += "): ";
            details += error.message;
        }
        return details;
    }

    const std::vector<GdalError>& GdalErrorScope::Errors() const noexcept
    {
        return _impl->errors;
    }

    void GdalErrorScope::RethrowCaptureException() const
    {
        if (_impl->captureException)
            std::rethrow_exception(_impl->captureException);
    }

    void GdalCancellationToken::Cancel() noexcept
    {
        _cancelled.store(true, std::memory_order_relaxed);
    }

    bool GdalCancellationToken::IsCancellationRequested() const noexcept
    {
        return _cancelled.load(std::memory_order_relaxed);
    }

    GdalProgressAdapter::GdalProgressAdapter(Callback callback, const GdalCancellationToken* cancellationToken)
        : _callback(std::move(callback)),
          _cancellationToken(cancellationToken)
    {
    }

    GdalProgressAdapter::NativeCallback GdalProgressAdapter::Function() const noexcept
    {
        return &HandleProgress;
    }

    void* GdalProgressAdapter::UserData() noexcept
    {
        return this;
    }

    bool GdalProgressAdapter::WasCancelled() const noexcept
    {
        return _cancelled;
    }

    void GdalProgressAdapter::RethrowCallbackException() const
    {
        if (_callbackException)
            std::rethrow_exception(_callbackException);
    }

    int __stdcall GdalProgressAdapter::HandleProgress(
        const double progress, const char* message, void* userData) noexcept
    {
        if (userData == nullptr)
            return 0;

        auto& adapter = *static_cast<GdalProgressAdapter*>(userData);
        if (adapter._cancelled)
            return 0;
        if (adapter._cancellationToken != nullptr && adapter._cancellationToken->IsCancellationRequested())
        {
            adapter._cancelled = true;
            return 0;
        }

        try
        {
            const detail::GdalCallbackScope callbackScope;
            if (adapter._callback && !adapter._callback(progress, message != nullptr ? message : ""))
            {
                adapter._cancelled = true;
                return 0;
            }
            if (adapter._cancellationToken != nullptr &&
                adapter._cancellationToken->IsCancellationRequested())
            {
                adapter._cancelled = true;
                return 0;
            }
        }
        catch (...)
        {
            adapter._callbackException = std::current_exception();
            adapter._cancelled = true;
            return 0;
        }

        return 1;
    }
}
