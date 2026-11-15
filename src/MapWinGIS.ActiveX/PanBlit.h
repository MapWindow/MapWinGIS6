// SPDX-License-Identifier: MPL-2.0

/**
 * @file PanBlit.h
 * @brief Validates map-center offsets before reusing a cached frame.
 */
#pragma once

#include <Windows.h>

#include <cmath>

namespace mwg::activex
{
    /// Converts a screen offset to a whole-pixel shift suitable for BitBlt.
    /// Fractional offsets and shifts at least as large as the target are
    /// rejected so callers can render the frame again instead.
    [[nodiscard]] inline bool TryGetWholePixelPanShift(
        const double offset,
        const LONG dimension,
        LONG& shift) noexcept
    {
        if (dimension <= 0 || !std::isfinite(offset) ||
            std::abs(offset) >= static_cast<double>(dimension))
        {
            return false;
        }

        const double rounded = std::round(offset);
        constexpr double kWholePixelEpsilon = 1e-6;
        if (std::abs(offset - rounded) > kWholePixelEpsilon ||
            std::abs(rounded) >= static_cast<double>(dimension))
        {
            return false;
        }

        shift = static_cast<LONG>(rounded);
        return true;
    }
}
