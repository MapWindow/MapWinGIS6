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

namespace mwg::detail
{
    /// GDAL expects UTF-8 paths, so this is the only conversion the core uses.
    inline std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }
}
