// SPDX-License-Identifier: MPL-2.0

/**
 * @file Viewport.h
 * @brief Declares the viewport: the view of map coordinates on a pixel area.
 *
 * The viewport converts between map coordinates and screen coordinates and
 * fits a map extent into the available display area. It holds no windowing,
 * COM, or rendering-backend state, and it does not reference a Map: callers
 * fit the viewport to Map::GetExtent and keep the two side by side.
 *
 * Coordinate conventions:
 * - Map coordinates: X increases to the right, Y increases upwards, in the
 *   units of the layers' coordinate system.
 * - Screen coordinates: X increases to the right, Y increases downwards, in
 *   pixels, with the origin at the top-left corner of the viewport. Screen
 *   coordinates are continuous: pixel (i, j) covers [i, i + 1) x [j, j + 1),
 *   so its center is at (i + 0.5, j + 0.5) and the viewport covers
 *   [0, Width()] x [0, Height()].
 */
#pragma once

#include <optional>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Extent.h"

namespace mwg
{
    /// A position in map coordinates (X right, Y up).
    struct MapPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    /// A position in continuous screen coordinates (X right, Y down, pixels).
    struct ScreenPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    /// The integer pixel that contains a screen position.
    struct PixelPoint
    {
        int x = 0;
        int y = 0;
    };

    /// Pixel that contains a screen position: each coordinate is rounded
    /// down, so pixel (i, j) holds every position in [i, i + 1) x [j, j + 1).
    /// Rounding happens only here; keep screen coordinates in double until
    /// pixels are needed. Returns std::nullopt when a coordinate is not
    /// finite or the pixel is outside the int range.
    [[nodiscard]] MWG_CORE_API std::optional<PixelPoint> ToPixel(const ScreenPoint& point) noexcept;

    /// The view of map coordinates on a rectangular pixel area.
    ///
    /// State: a size in pixels and, once set, a view given by the map
    /// coordinate shown at the viewport center and the resolution in map
    /// units per pixel. The same resolution applies to both axes, so the view
    /// never distorts geometry. The visible extent follows from these values
    /// and always has the viewport's aspect ratio.
    ///
    /// Validity: a default-constructed viewport has size 0 x 0 and no view.
    /// A viewport is valid when both dimensions are positive and it has a
    /// view. Transformations and VisibleExtent need a valid viewport; on an
    /// invalid one they return std::nullopt or an empty Extent. A zero-sized
    /// viewport (for example, a minimized window) keeps its view, so the view
    /// returns when the viewport gets a size again.
    ///
    /// Limits: the center is clamped to +/-kMaxCoordinate on each axis and
    /// the resolution to [MinResolution(center), kMaxResolution]. The lower
    /// resolution bound keeps every pixel many representable doubles wide,
    /// so screen-to-map conversions stay stable at any supported location.
    ///
    /// Viewport is a small copyable value type, like Extent, so render
    /// scenes and navigation code can take snapshots of it.
    /// Not thread-safe.
    class MWG_CORE_API Viewport
    {
    public:
        /// Largest supported width or height, in pixels.
        static constexpr int kMaxDimension = 1 << 16;

        /// Largest supported absolute center coordinate, in map units.
        static constexpr double kMaxCoordinate = 1.0e12;

        /// Largest supported resolution, in map units per pixel.
        static constexpr double kMaxResolution = 1.0e9;

        /// Smallest supported resolution relative to the center's magnitude;
        /// see MinResolution.
        static constexpr double kMinRelativeResolution = 1.0e-10;

        /// Resolution used to fit a single point when there is no view yet.
        static constexpr double kDefaultPointResolution = 1.0;

        /// Smallest supported resolution for a center: kMinRelativeResolution
        /// times the larger of 1 and the center's largest absolute coordinate.
        [[nodiscard]] static double MinResolution(const MapPoint& center) noexcept;

        /// Size 0 x 0 and no view.
        Viewport() noexcept = default;

        /// A viewport of this size without a view.
        /// \throws InvalidArgument when a dimension is negative or larger than
        ///         kMaxDimension.
        Viewport(int width, int height);

        [[nodiscard]] int Width() const noexcept { return _width; }
        [[nodiscard]] int Height() const noexcept { return _height; }

        /// True when the view has been set by SetView or FitExtent.
        [[nodiscard]] bool HasView() const noexcept { return _hasView; }

        /// True when both dimensions are positive and the viewport has a view.
        [[nodiscard]] bool IsValid() const noexcept
        {
            return _hasView && _width > 0 && _height > 0;
        }

        /// Map coordinate at the viewport center, or std::nullopt without a view.
        [[nodiscard]] std::optional<MapPoint> Center() const noexcept;

        /// Map units per pixel, or std::nullopt without a view.
        [[nodiscard]] std::optional<double> Resolution() const noexcept;

        /// Changes the size and keeps the center and resolution, so the map
        /// stays centered and keeps its scale when the display area resizes.
        /// \throws InvalidArgument when a dimension is negative or larger than
        ///         kMaxDimension.
        void SetSize(int width, int height);

        /// Sets the view. The center and resolution are clamped to the limits
        /// described above.
        /// \throws InvalidArgument when a center coordinate is not finite or
        ///         the resolution is not a finite positive number.
        void SetView(const MapPoint& center, double resolution);

        /// Removes the view; the size is kept.
        void ClearView() noexcept;

        /// Fits extent into the viewport, keeping paddingPixels free on every
        /// side, and returns true. The extent is centered, and the resolution
        /// is the smallest that shows the whole extent on both axes, so the
        /// visible extent grows on the other axis to keep the aspect ratio.
        ///
        /// The extent edges map to the centers of the outermost pixels inside
        /// the padding, so rounding to pixels never puts boundary geometry
        /// outside the viewport. When the padding leaves less than half a
        /// pixel between those centers, the extent spans half a pixel around
        /// the viewport center.
        ///
        /// Degenerate extents: an axis with zero span does not constrain the
        /// resolution. A single point is centered and keeps the current
        /// resolution, or kDefaultPointResolution without a view. Results are
        /// clamped to the limits; an extent larger than the limits allow is
        /// then not fully visible.
        ///
        /// Returns false, and changes nothing, when the extent is not valid
        /// (see Extent::IsValid) or a dimension is zero.
        /// \throws InvalidArgument when paddingPixels is negative or not finite.
        bool FitExtent(const Extent& extent, double paddingPixels = 0.0);

        /// Moves the view by a map-coordinate delta: the visible extent
        /// shifts by (deltaX, deltaY), so positive deltaX shows content
        /// further right (east) and positive deltaY content further up
        /// (north). The center is clamped to the limits. Returns true when
        /// the view changed; false without a view or for a zero delta.
        /// The size is not needed, so a zero-sized viewport can be panned.
        /// \throws InvalidArgument when a delta is not finite.
        bool Pan(double deltaX, double deltaY);

        /// Zooms around the viewport center: the resolution is divided by
        /// factor, so a factor above 1 zooms in and below 1 zooms out. The
        /// center is kept and the resolution is clamped to the limits.
        /// Returns true when the view changed; false without a view or when
        /// the resolution is already at the limit in that direction.
        /// \throws InvalidArgument when factor is not a finite positive number.
        bool Zoom(double factor);

        /// Zooms around the viewport center by multiplying the resolution
        /// by factor. A factor above 1 zooms out and below 1 zooms in.
        /// Keeps the center and clamps the resolution to the limits.
        /// Returns true when the view changed; false without a view or at
        /// the resolution limit in that direction.
        /// \throws InvalidArgument when factor is not a finite positive number.
        bool ZoomOut(double factor);

        /// Zooms around a screen position: the resolution is divided by
        /// factor as in Zoom, and the map coordinate under anchor stays
        /// under anchor. The anchor may lie outside the viewport. When the
        /// limits clamp the result, the resolution or center is clamped and
        /// the anchored coordinate may move. Moving the center can increase
        /// MinResolution, which clamps the resolution again and can also
        /// move the anchored coordinate. Returns true when the view changed;
        /// false when the viewport is not valid or the view is already at
        /// the limit.
        /// \throws InvalidArgument when factor is not a finite positive number
        ///         or an anchor coordinate is not finite.
        bool ZoomAt(const ScreenPoint& anchor, double factor);

        /// Map extent covered by the whole viewport, or an empty Extent when
        /// the viewport is not valid.
        [[nodiscard]] Extent VisibleExtent() const noexcept;

        /// Screen position of a map point. Returns std::nullopt when the
        /// viewport is not valid, a coordinate is not finite, or the result
        /// is not finite. Points outside the visible extent give positions
        /// outside the viewport.
        [[nodiscard]] std::optional<ScreenPoint> MapToScreen(const MapPoint& point) const noexcept;

        /// Map position of a screen point; the inverse of MapToScreen.
        /// Returns std::nullopt under the same conditions.
        [[nodiscard]] std::optional<MapPoint> ScreenToMap(const ScreenPoint& point) const noexcept;

    private:
        /// Applies a clamped view and returns true when it differs from the
        /// current one.
        bool ApplyView(const MapPoint& center, double resolution) noexcept;

        int _width = 0;
        int _height = 0;
        bool _hasView = false;
        MapPoint _center{};
        double _resolution = 0.0;
    };
}
