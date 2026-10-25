// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalExecution.h
 * @brief Serializes native GDAL operations and guards synchronous callbacks.
 */
#pragma once

#include <mutex>

#include "mapwingis/core/Error.h"

namespace mwg::detail
{
    std::recursive_mutex& GdalExecutionMutex();
    bool& GdalCallbackActive();

    class GdalExecutionLock
    {
    public:
        GdalExecutionLock()
        {
            if (GdalCallbackActive())
                throw CoreError("GDAL operations cannot be called from a GDAL callback.");
            _lock = std::unique_lock(GdalExecutionMutex());
        }

    private:
        std::unique_lock<std::recursive_mutex> _lock;
    };

    class GdalCallbackScope
    {
    public:
        GdalCallbackScope() : _previous(GdalCallbackActive())
        {
            GdalCallbackActive() = true;
        }

        ~GdalCallbackScope()
        {
            GdalCallbackActive() = _previous;
        }

        GdalCallbackScope(const GdalCallbackScope&) = delete;
        GdalCallbackScope& operator=(const GdalCallbackScope&) = delete;

    private:
        bool _previous;
    };
}
