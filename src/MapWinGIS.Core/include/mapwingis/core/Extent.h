// SPDX-License-Identifier: MPL-2.0

/**
 * @file Extent.h
 * @brief Defines an axis-aligned 2D bounding extent.
 *
 * Extent represents an empty or bounded coordinate region and provides
 * containment, intersection, and union operations for core geometry APIs.
 */
#pragma once

#include <algorithm>
#include <limits>

#include "mapwingis/core/Api.h"

namespace mwg
{
    /// Axis-aligned bounding box. Default-constructed instances are empty.
    struct MWG_CORE_API Extent
    {
        double xMin = std::numeric_limits<double>::quiet_NaN();
        double yMin = std::numeric_limits<double>::quiet_NaN();
        double xMax = std::numeric_limits<double>::quiet_NaN();
        double yMax = std::numeric_limits<double>::quiet_NaN();

        constexpr Extent() = default;

        constexpr Extent(const double xMinimum, const double yMinimum,
                         const double xMaximum, const double yMaximum) noexcept
            : xMin(xMinimum), yMin(yMinimum), xMax(xMaximum), yMax(yMaximum)
        {
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !(xMin <= xMax && yMin <= yMax);
        }

        [[nodiscard]] double Width() const noexcept { return IsEmpty() ? 0.0 : xMax - xMin; }
        [[nodiscard]] double Height() const noexcept { return IsEmpty() ? 0.0 : yMax - yMin; }
        [[nodiscard]] double CenterX() const noexcept { return (xMin + xMax) / 2.0; }
        [[nodiscard]] double CenterY() const noexcept { return (yMin + yMax) / 2.0; }

        [[nodiscard]] bool Intersects(const Extent& other) const noexcept
        {
            if (IsEmpty() || other.IsEmpty()) return false;
            return xMin <= other.xMax && xMax >= other.xMin &&
                   yMin <= other.yMax && yMax >= other.yMin;
        }

        [[nodiscard]] bool Contains(const double x, const double y) const noexcept
        {
            return !IsEmpty() && x >= xMin && x <= xMax && y >= yMin && y <= yMax;
        }

        [[nodiscard]] Extent Union(const Extent& other) const noexcept
        {
            if (IsEmpty()) return other;
            if (other.IsEmpty()) return *this;
            return {std::min(xMin, other.xMin), std::min(yMin, other.yMin),
                    std::max(xMax, other.xMax), std::max(yMax, other.yMax)};
        }
    };
}
