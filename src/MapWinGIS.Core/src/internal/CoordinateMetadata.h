// SPDX-License-Identifier: MPL-2.0

/**
 * @file CoordinateMetadata.h
 * @brief Serializes coordinate epochs in WKT2 for CRS export and layer import.
 */
#pragma once

#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace mwg::detail
{
    inline std::string AddCoordinateMetadata(std::string wkt, const double epoch)
    {
        if (!std::isfinite(epoch) || epoch == 0.0 || wkt.empty())
        {
            return wkt;
        }

        char buffer[32];
        const auto formatted = std::to_chars(
            buffer, buffer + sizeof(buffer), epoch, std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (formatted.ec != std::errc{})
        {
            return wkt;
        }
        return "COORDINATEMETADATA[" + wkt + ",EPOCH[" +
               std::string(buffer, formatted.ptr) + "]]";
    }
}
