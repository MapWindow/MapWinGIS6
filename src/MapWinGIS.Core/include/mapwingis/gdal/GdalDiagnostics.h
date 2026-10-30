// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalDiagnostics.h
 * @brief Declares structured GDAL diagnostics and error capture.
 *
 * Captures errors raised by GDAL on the current thread and preserves failures
 * from diagnostic logging until the enclosing GDAL operation returns.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"

namespace mwg
{
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
}
