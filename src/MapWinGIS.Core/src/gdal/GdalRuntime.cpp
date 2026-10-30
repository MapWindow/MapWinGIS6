// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalRuntime.cpp
 * @brief Implements the GDAL runtime foundation.
 *
 * Provides process-safe driver registration, configuration scopes, structured
 * error capture, and cancellation-aware progress callbacks.
 */
#include "mapwingis/gdal/GdalConfig.h"
#include "mapwingis/gdal/GdalDiagnostics.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalProgress.h"
#include "internal/GdalExecution.h"
#include "internal/GdalProgressAdapterAccess.h"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
                throw InvalidArgument("GDAL configuration option name must be non-empty and contain no null bytes.");
        }

        std::optional<std::string> GetGlobalConfigOption(const std::string& name)
        {
            // CSLDestroy is __stdcall on x86, so wrap it instead of naming its function-pointer type.
            const auto destroyOptions = [](char** options) { CSLDestroy(options); };
            std::unique_ptr<char*, decltype(destroyOptions)> configOptions(CPLGetConfigOptions(), destroyOptions);
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
            try
            {
                scope.errors.push_back(
                    {static_cast<GdalErrorClass>(errorClass), static_cast<int>(errorCode), message});
            }
            catch (...)
            {
                if (!scope.captureException)
                    scope.captureException = std::current_exception();
                return;
            }

            try
            {
                const LogLevel level = errorClass >= CE_Failure ? LogLevel::Error
                    : errorClass == CE_Warning ? LogLevel::Warning : LogLevel::Debug;
                GdalEnvironment::ReportDiagnostic(level, message);
            }
            catch (...)
            {
                if (!scope.captureException)
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
            throw InvalidArgument("GDAL configuration option value cannot contain null bytes.");
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

    struct ScopedGdalConfigOption::Impl
    {
        explicit Impl(const std::string_view name) : name(name) {}

        std::string name;
        std::optional<std::string> previousValue;
    };

    ScopedGdalConfigOption::ScopedGdalConfigOption(
        const std::string_view name, const std::string_view value)
        : _impl(std::make_unique<Impl>(name))
    {
        ValidateOptionName(name);
        _impl->previousValue = GetGlobalConfigOption(_impl->name);
        GdalConfig::SetOption(_impl->name, value);
    }

    ScopedGdalConfigOption::~ScopedGdalConfigOption()
    {
        CPLSetConfigOption(
            _impl->name.c_str(),
            _impl->previousValue ? _impl->previousValue->c_str() : nullptr);
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

    struct GdalProgressAdapter::Impl
    {
        Impl(Callback callback, const GdalCancellationToken* cancellationToken)
            : callback(std::move(callback)), cancellationToken(cancellationToken)
        {
        }

        Callback callback;
        const GdalCancellationToken* cancellationToken;
        bool cancelled = false;
        std::exception_ptr callbackException;
    };

    GdalProgressAdapter::GdalProgressAdapter(
        Callback callback, const GdalCancellationToken* cancellationToken)
        : _impl(std::make_unique<Impl>(std::move(callback), cancellationToken))
    {
    }

    GdalProgressAdapter::~GdalProgressAdapter() = default;

    bool GdalProgressAdapter::WasCancelled() const noexcept
    {
        return _impl->cancelled;
    }

    void GdalProgressAdapter::RethrowCallbackException() const
    {
        if (_impl->callbackException)
            std::rethrow_exception(_impl->callbackException);
    }

    detail::GdalProgressAdapterAccess::NativeCallback detail::GdalProgressAdapterAccess::Function(
        const GdalProgressAdapter&) noexcept
    {
        return &HandleProgress;
    }

    void* detail::GdalProgressAdapterAccess::UserData(GdalProgressAdapter& adapter) noexcept
    {
        return adapter._impl.get();
    }

    int __stdcall detail::GdalProgressAdapterAccess::HandleProgress(
        const double progress, const char* message, void* userData) noexcept
    {
        if (userData == nullptr)
            return 0;

        auto& impl = *static_cast<GdalProgressAdapter::Impl*>(userData);
        if (impl.cancelled)
            return 0;
        if (impl.cancellationToken != nullptr && impl.cancellationToken->IsCancellationRequested())
        {
            impl.cancelled = true;
            return 0;
        }

        try
        {
            const detail::GdalCallbackScope callbackScope;
            if (impl.callback && !impl.callback(progress, message != nullptr ? message : ""))
            {
                impl.cancelled = true;
                return 0;
            }
            if (impl.cancellationToken != nullptr &&
                impl.cancellationToken->IsCancellationRequested())
            {
                impl.cancelled = true;
                return 0;
            }
        }
        catch (...)
        {
            impl.callbackException = std::current_exception();
            impl.cancelled = true;
            return 0;
        }

        return 1;
    }
}
