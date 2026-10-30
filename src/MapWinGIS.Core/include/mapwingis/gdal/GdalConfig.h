// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalConfig.h
 * @brief Declares GDAL initialization and configuration helpers.
 *
 * Provides process-wide driver registration and configuration options,
 * including a scope that restores a previous option value.
 */
#pragma once

#include <memory>
#include <string_view>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Error.h"

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

        /// @throws InvalidArgument if name is empty or contains a null byte,
        /// or if value contains a null byte.
        static void SetOption(std::string_view name, std::string_view value);

        /// @throws InvalidArgument if name is empty or contains a null byte.
        static void ClearOption(std::string_view name);
    };

    /// Restores the effective GDAL option value that existed at construction.
    ///
    /// Scopes must be destroyed in reverse construction order. The option is
    /// process-wide; concurrent changes to the same option are not isolated.
    class MWG_CORE_API ScopedGdalConfigOption
    {
    public:
        /// @throws InvalidArgument if name is empty or contains a null byte,
        /// or if value contains a null byte.
        ScopedGdalConfigOption(std::string_view name, std::string_view value);
        ~ScopedGdalConfigOption();

        ScopedGdalConfigOption(const ScopedGdalConfigOption&) = delete;
        ScopedGdalConfigOption& operator=(const ScopedGdalConfigOption&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
