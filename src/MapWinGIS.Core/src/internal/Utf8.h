// SPDX-License-Identifier: MPL-2.0

/**
 * @file Utf8.h
 * @brief Converts filesystem paths to UTF-8 strings for GDAL operations.
 *
 * Provides the shared internal path conversion used by the core library.
 */
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "mapwingis/core/Error.h"

namespace mwg::detail
{
    /// GDAL expects UTF-8 paths, so this is the only conversion the core uses.
    /// Invalid native path sequences are reported through the normal Core API
    /// exception contract rather than leaking a filesystem conversion exception.
    inline std::string ToUtf8(const std::filesystem::path& path)
    {
        try
        {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }
        catch (const std::system_error& error)
        {
            throw InvalidArgument("Filesystem path cannot be converted to UTF-8: " +
                                  std::string(error.what()));
        }
    }
}
