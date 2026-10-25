// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalRuntime.h
 * @brief Declares GDAL initialization, configuration, error and progress helpers.
 *
 * These helpers provide the native runtime foundation used by MapWinGIS.Core.
 * Error scopes and progress adapters are thread-bound and must be used on the
 * thread that invokes the associated GDAL operation.
 */
#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mapwingis/core/Api.h"

namespace mwg
{
    /// Keeps GDAL driver registration initialized for the process lifetime.
    ///
    /// Construction is safe to repeat from multiple threads. Destruction does
    /// not tear down GDAL's process-wide driver manager.
    class MWG_CORE_API GdalInitializer
    {
    public:
        GdalInitializer();
        ~GdalInitializer();

        GdalInitializer(const GdalInitializer&) = delete;
        GdalInitializer& operator=(const GdalInitializer&) = delete;
    };

    /// Sets or clears a process-wide GDAL configuration option.
    class MWG_CORE_API GdalConfig
    {
    public:
        GdalConfig() = delete;

        static void SetOption(std::string_view name, std::string_view value);
        static void ClearOption(std::string_view name);
    };

    /// Restores the effective GDAL option value that existed at construction.
    ///
    /// Scopes must be destroyed in reverse construction order. The option is
    /// process-wide; concurrent changes to the same option are not isolated.
    class MWG_CORE_API ScopedGdalConfigOption
    {
    public:
        ScopedGdalConfigOption(std::string_view name, std::string_view value);
        ~ScopedGdalConfigOption();

        ScopedGdalConfigOption(const ScopedGdalConfigOption&) = delete;
        ScopedGdalConfigOption& operator=(const ScopedGdalConfigOption&) = delete;

    private:
        std::string _name;
        std::optional<std::string> _previousValue;
    };

    enum class GdalErrorClass : int
    {
        Debug = 1,
        Warning = 2,
        Failure = 3,
        Fatal = 4,
    };

    /// One diagnostic captured from a GDAL error handler.
    struct GdalError
    {
        GdalErrorClass errorClass;
        int code;
        std::string message;
    };

    /// Captures GDAL diagnostics raised on the current thread during its lifetime.
    ///
    /// The scope uses GDAL's thread-local error-handler stack and must be
    /// destroyed on the same thread on which it was constructed. Call
    /// RethrowCaptureException() after the operation to report capture failures.
    /// Diagnostics also reach GdalEnvironment's logger. Logger exceptions are
    /// preserved by the scope and rethrown by RethrowCaptureException().
    class MWG_CORE_API GdalErrorScope
    {
    public:
        GdalErrorScope();
        ~GdalErrorScope();

        GdalErrorScope(const GdalErrorScope&) = delete;
        GdalErrorScope& operator=(const GdalErrorScope&) = delete;

        [[nodiscard]] int LastErrorCode() const noexcept;
        [[nodiscard]] std::string LastErrorMessage() const;
        [[nodiscard]] std::string DetailedErrorMessage() const;
        [[nodiscard]] const std::vector<GdalError>& Errors() const noexcept;
        void RethrowCaptureException() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    /// Thread-safe cancellation state shared with a GDAL progress adapter.
    class MWG_CORE_API GdalCancellationToken
    {
    public:
        void Cancel() noexcept;
        [[nodiscard]] bool IsCancellationRequested() const noexcept;

    private:
        std::atomic_bool _cancelled{false};
    };

    /// Adapts GDAL's callback ABI to a C++ progress callback and cancellation token.
    ///
    /// Progress callbacks run synchronously on the thread executing GDAL.
    /// Keep the adapter alive and unmoved through the GDAL operation; its
    /// cancellation token must also outlive the operation. If a callback
    /// throws, the adapter cancels the operation and preserves the exception
    /// for RethrowCallbackException().
    class MWG_CORE_API GdalProgressAdapter
    {
    public:
        using Callback = std::function<bool(double, std::string_view)>;
        using NativeCallback = int(__stdcall*)(double, const char*, void*);

        explicit GdalProgressAdapter(Callback callback = {},
                                     const GdalCancellationToken* cancellationToken = nullptr);
        GdalProgressAdapter(const GdalProgressAdapter&) = delete;
        GdalProgressAdapter& operator=(const GdalProgressAdapter&) = delete;

        [[nodiscard]] NativeCallback Function() const noexcept;
        [[nodiscard]] void* UserData() noexcept;
        [[nodiscard]] bool WasCancelled() const noexcept;
        void RethrowCallbackException() const;

    private:
        static int __stdcall HandleProgress(double progress, const char* message, void* userData) noexcept;

        Callback _callback;
        const GdalCancellationToken* _cancellationToken;
        bool _cancelled = false;
        std::exception_ptr _callbackException;
    };
}
