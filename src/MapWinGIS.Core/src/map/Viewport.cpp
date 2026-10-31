// SPDX-License-Identifier: MPL-2.0

/**
 * @file Viewport.cpp
 * @brief Implements viewport transformations and extent fitting.
 */
#include "mapwingis/map/Viewport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "mapwingis/core/Error.h"

namespace mwg
{
    namespace
    {
        void ValidateSize(const int width, const int height)
        {
            if (width < 0 || height < 0 || width > Viewport::kMaxDimension ||
                height > Viewport::kMaxDimension)
            {
                throw InvalidArgument(
                    "Viewport size out of range: " + std::to_string(width) + " x " +
                    std::to_string(height));
            }
        }

        [[nodiscard]] MapPoint ClampCenter(const MapPoint& center) noexcept
        {
            return {
                std::clamp(center.x, -Viewport::kMaxCoordinate, Viewport::kMaxCoordinate),
                std::clamp(center.y, -Viewport::kMaxCoordinate, Viewport::kMaxCoordinate)};
        }

        [[nodiscard]] double ClampResolution(const double resolution, const MapPoint& center) noexcept
        {
            return std::clamp(resolution, Viewport::MinResolution(center), Viewport::kMaxResolution);
        }

        // Pixels available for the extent on one axis. The extent edges map
        // to the centers of the outermost pixels inside the padding, which
        // removes one pixel from the span. At least half a pixel remains, so
        // even a one-pixel axis keeps both edges inside the pixel.
        [[nodiscard]] double AvailablePixels(const int dimension, const double padding) noexcept
        {
            return std::max(static_cast<double>(dimension) - 1.0 - 2.0 * padding, 0.5);
        }

        [[nodiscard]] bool IsFinite(const double x, const double y) noexcept
        {
            return std::isfinite(x) && std::isfinite(y);
        }

        [[nodiscard]] bool IsInIntRange(const double value) noexcept
        {
            return value >= static_cast<double>(std::numeric_limits<int>::min()) &&
                   value <= static_cast<double>(std::numeric_limits<int>::max());
        }
    }

    std::optional<PixelPoint> ToPixel(const ScreenPoint& point) noexcept
    {
        if (!IsFinite(point.x, point.y))
        {
            return std::nullopt;
        }

        const double x = std::floor(point.x);
        const double y = std::floor(point.y);
        if (!IsInIntRange(x) || !IsInIntRange(y))
        {
            return std::nullopt;
        }
        return PixelPoint{static_cast<int>(x), static_cast<int>(y)};
    }

    double Viewport::MinResolution(const MapPoint& center) noexcept
    {
        const double magnitude = std::max({1.0, std::abs(center.x), std::abs(center.y)});
        return kMinRelativeResolution * magnitude;
    }

    Viewport::Viewport(const int width, const int height)
    {
        SetSize(width, height);
    }

    std::optional<MapPoint> Viewport::Center() const noexcept
    {
        if (!_hasView)
        {
            return std::nullopt;
        }
        return _center;
    }

    std::optional<double> Viewport::Resolution() const noexcept
    {
        if (!_hasView)
        {
            return std::nullopt;
        }
        return _resolution;
    }

    void Viewport::SetSize(const int width, const int height)
    {
        ValidateSize(width, height);
        _width = width;
        _height = height;
    }

    void Viewport::SetView(const MapPoint& center, const double resolution)
    {
        if (!IsFinite(center.x, center.y))
        {
            throw InvalidArgument("Viewport center must be finite");
        }
        if (!std::isfinite(resolution) || resolution <= 0.0)
        {
            throw InvalidArgument("Viewport resolution must be a finite positive number");
        }

        _center = ClampCenter(center);
        _resolution = ClampResolution(resolution, _center);
        _hasView = true;
    }

    void Viewport::ClearView() noexcept
    {
        _hasView = false;
        _center = {};
        _resolution = 0.0;
    }

    bool Viewport::FitExtent(const Extent& extent, const double paddingPixels)
    {
        if (!std::isfinite(paddingPixels) || paddingPixels < 0.0)
        {
            throw InvalidArgument("Viewport padding must be a finite non-negative number");
        }
        if (!extent.IsValid() || _width <= 0 || _height <= 0)
        {
            return false;
        }

        // Halving before adding keeps the center finite for extents near the
        // double range.
        const MapPoint center = ClampCenter(
            {extent.xMin / 2.0 + extent.xMax / 2.0, extent.yMin / 2.0 + extent.yMax / 2.0});

        double resolution = std::max(
            extent.Width() / AvailablePixels(_width, paddingPixels),
            extent.Height() / AvailablePixels(_height, paddingPixels));
        if (!std::isfinite(resolution))
        {
            resolution = kMaxResolution;
        }
        else if (resolution <= 0.0)
        {
            resolution = _hasView ? _resolution : kDefaultPointResolution;
        }

        _center = center;
        _resolution = ClampResolution(resolution, center);
        _hasView = true;
        return true;
    }

    Extent Viewport::VisibleExtent() const noexcept
    {
        if (!IsValid())
        {
            return {};
        }

        const double halfWidth = static_cast<double>(_width) * _resolution / 2.0;
        const double halfHeight = static_cast<double>(_height) * _resolution / 2.0;
        return {
            _center.x - halfWidth,
            _center.y - halfHeight,
            _center.x + halfWidth,
            _center.y + halfHeight};
    }

    std::optional<ScreenPoint> Viewport::MapToScreen(const MapPoint& point) const noexcept
    {
        if (!IsValid() || !IsFinite(point.x, point.y))
        {
            return std::nullopt;
        }

        const double x = static_cast<double>(_width) / 2.0 + (point.x - _center.x) / _resolution;
        const double y = static_cast<double>(_height) / 2.0 - (point.y - _center.y) / _resolution;
        if (!IsFinite(x, y))
        {
            return std::nullopt;
        }
        return ScreenPoint{x, y};
    }

    std::optional<MapPoint> Viewport::ScreenToMap(const ScreenPoint& point) const noexcept
    {
        if (!IsValid() || !IsFinite(point.x, point.y))
        {
            return std::nullopt;
        }

        const double x = _center.x + (point.x - static_cast<double>(_width) / 2.0) * _resolution;
        const double y = _center.y - (point.y - static_cast<double>(_height) / 2.0) * _resolution;
        if (!IsFinite(x, y))
        {
            return std::nullopt;
        }
        return MapPoint{x, y};
    }
}
