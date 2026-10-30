// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalProgress.h
 * @brief Declares cancellation tokens and the C++ GDAL progress adapter.
 *
 * Adapts GDAL progress callbacks to C++ callbacks while retaining cancellation
 * and callback-exception state without exposing GDAL's native callback ABI.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include "mapwingis/core/Api.h"

namespace mwg
{
    namespace detail
    {
        class GdalProgressAdapterAccess;
    }

    /// Thread-safe cancellation state shared with a GDAL progress adapter.
    class MWG_CORE_API GdalCancellationToken
    {
    public:
        void Cancel() noexcept;
        [[nodiscard]] bool IsCancellationRequested() const noexcept;

    private:
        std::atomic_bool _cancelled{false};
    };

    /// Adapts GDAL progress to a C++ callback and cancellation token.
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

        explicit GdalProgressAdapter(Callback callback = {},
                                     const GdalCancellationToken* cancellationToken = nullptr);
        ~GdalProgressAdapter();

        GdalProgressAdapter(const GdalProgressAdapter&) = delete;
        GdalProgressAdapter& operator=(const GdalProgressAdapter&) = delete;

        [[nodiscard]] bool WasCancelled() const noexcept;
        void RethrowCallbackException() const;

    private:
        friend class detail::GdalProgressAdapterAccess;

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
