// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdiVectorLayerRenderer.cpp
 * @brief Draws polygon render scenes into a Windows GDI device context.
 *
 * Transforms every layer through the same Core viewport and paints polygon
 * fills and outlines without retaining scene or host resources.
 */
#include "GdiVectorLayerRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "ViewTransform.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"

namespace mwg::activex
{
    namespace
    {
        constexpr COLORREF kFillColor = RGB(61, 122, 169);
        constexpr COLORREF kOutlineColor = RGB(24, 58, 82);

        class ScopedDcState
        {
        public:
            explicit ScopedDcState(HDC deviceContext)
                : _deviceContext(deviceContext), _state(SaveDC(deviceContext))
            {
            }

            ~ScopedDcState()
            {
                if (_state != 0)
                {
                    RestoreDC(_deviceContext, _state);
                }
            }

            [[nodiscard]] bool IsValid() const noexcept { return _state != 0; }

        private:
            HDC _deviceContext;
            int _state;
        };

        class ScopedGdiObject
        {
        public:
            explicit ScopedGdiObject(HGDIOBJ object) noexcept
                : _object(object)
            {
            }

            ~ScopedGdiObject()
            {
                if (_object != nullptr)
                {
                    DeleteObject(_object);
                }
            }

            [[nodiscard]] HGDIOBJ Get() const noexcept { return _object; }

        private:
            HGDIOBJ _object;
        };

        class ScopedSelection
        {
        public:
            ScopedSelection(HDC deviceContext, HGDIOBJ object) noexcept
                : _deviceContext(deviceContext), _previous(SelectObject(deviceContext, object))
            {
            }

            ~ScopedSelection()
            {
                if (_previous != nullptr && _previous != HGDI_ERROR)
                {
                    SelectObject(_deviceContext, _previous);
                }
            }

            [[nodiscard]] bool IsValid() const noexcept
            {
                return _previous != nullptr && _previous != HGDI_ERROR;
            }

        private:
            HDC _deviceContext;
            HGDIOBJ _previous;
        };

        // GDI on Windows NT only accepts coordinates within a signed 27-bit range.
        constexpr double kMaximumGdiCoordinate = 134217727.0;

        [[nodiscard]] double ToDeviceX(
            const rendering::Point& point,
            const ViewTransform& transform,
            const double originX) noexcept
        {
            return originX + transform.ToDeviceX(point);
        }

        [[nodiscard]] double ToDeviceY(
            const rendering::Point& point,
            const ViewTransform& transform,
            const double originY) noexcept
        {
            return originY + transform.ToDeviceY(point);
        }

        [[nodiscard]] bool IsFiniteCoordinate(const double value) noexcept
        {
            return std::isfinite(value);
        }

        /// Checks every transformed vertex before drawing so non-finite
        /// geometry skips the whole layer instead of being partly rasterized.
        [[nodiscard]] bool HasFiniteCoordinates(
            const rendering::VectorRenderScene& scene,
            const ViewTransform& transform,
            const double originX,
            const double originY) noexcept
        {
            for (const rendering::Point& point : scene.polygons.Coordinates())
            {
                if (!IsFiniteCoordinate(ToDeviceX(point, transform, originX)) ||
                    !IsFiniteCoordinate(ToDeviceY(point, transform, originY)))
                {
                    return false;
                }
            }

            return true;
        }

        struct DevicePoint
        {
            double x;
            double y;
        };

        struct DeviceClipBounds
        {
            double minimumX;
            double maximumX;
            double minimumY;
            double maximumY;
        };

        [[nodiscard]] DeviceClipBounds MakeDeviceClipBounds(const RECT& bounds) noexcept
        {
            const double left = static_cast<double>(bounds.left);
            const double right = static_cast<double>(bounds.right);
            const double top = static_cast<double>(bounds.top);
            const double bottom = static_cast<double>(bounds.bottom);
            const double margin = std::max(right - left, bottom - top) + 1.0;
            return {
                std::max(-kMaximumGdiCoordinate, left - margin),
                std::min(kMaximumGdiCoordinate, right + margin),
                std::max(-kMaximumGdiCoordinate, top - margin),
                std::min(kMaximumGdiCoordinate, bottom + margin)};
        }

        enum class ClipBoundary
        {
            MinimumX,
            MaximumX,
            MinimumY,
            MaximumY
        };

        [[nodiscard]] bool IsInside(
            const DevicePoint& point,
            const ClipBoundary boundary,
            const DeviceClipBounds& bounds) noexcept
        {
            switch (boundary)
            {
            case ClipBoundary::MinimumX:
                return point.x >= bounds.minimumX;
            case ClipBoundary::MaximumX:
                return point.x <= bounds.maximumX;
            case ClipBoundary::MinimumY:
                return point.y >= bounds.minimumY;
            case ClipBoundary::MaximumY:
                return point.y <= bounds.maximumY;
            }
            return false;
        }

        [[nodiscard]] double InterpolationParameter(
            const double start,
            const double end,
            const double boundary) noexcept
        {
            if (std::signbit(start) != std::signbit(end))
            {
                const double halfStart = start * 0.5;
                const double halfEnd = end * 0.5;
                return (boundary * 0.5 - halfStart) / (halfEnd - halfStart);
            }
            return (boundary - start) / (end - start);
        }

        [[nodiscard]] DevicePoint IntersectBoundary(
            const DevicePoint& start,
            const DevicePoint& end,
            const ClipBoundary boundary,
            const DeviceClipBounds& bounds) noexcept
        {
            if (boundary == ClipBoundary::MinimumX ||
                boundary == ClipBoundary::MaximumX)
            {
                const double x = boundary == ClipBoundary::MinimumX
                    ? bounds.minimumX
                    : bounds.maximumX;
                const double parameter = InterpolationParameter(start.x, end.x, x);
                return {x, std::lerp(start.y, end.y, parameter)};
            }

            const double y = boundary == ClipBoundary::MinimumY
                ? bounds.minimumY
                : bounds.maximumY;
            const double parameter = InterpolationParameter(start.y, end.y, y);
            return {std::lerp(start.x, end.x, parameter), y};
        }

        void ClipRingAgainstBoundary(
            const std::vector<DevicePoint>& input,
            std::vector<DevicePoint>& output,
            const ClipBoundary boundary,
            const DeviceClipBounds& bounds)
        {
            output.clear();
            if (input.empty())
            {
                return;
            }

            DevicePoint previous = input.back();
            bool previousInside = IsInside(previous, boundary, bounds);
            for (const DevicePoint& current : input)
            {
                const bool currentInside = IsInside(current, boundary, bounds);
                if (previousInside && currentInside)
                {
                    output.push_back(current);
                }
                else if (previousInside)
                {
                    output.push_back(IntersectBoundary(previous, current, boundary, bounds));
                }
                else if (currentInside)
                {
                    output.push_back(IntersectBoundary(previous, current, boundary, bounds));
                    output.push_back(current);
                }
                previous = current;
                previousInside = currentInside;
            }
        }

        void ClipRingToBounds(
            std::vector<DevicePoint>& ring,
            std::vector<DevicePoint>& scratch,
            const DeviceClipBounds& bounds)
        {
            for (const ClipBoundary boundary : {
                     ClipBoundary::MinimumX,
                     ClipBoundary::MaximumX,
                     ClipBoundary::MinimumY,
                     ClipBoundary::MaximumY})
            {
                ClipRingAgainstBoundary(ring, scratch, boundary, bounds);
                ring.swap(scratch);
                if (ring.size() < 3)
                {
                    ring.clear();
                    return;
                }
            }
        }

        [[nodiscard]] HRESULT DrawLayer(
            const HDC deviceContext,
            const rendering::VectorRenderScene& scene,
            const ViewTransform& transform,
            const double originX,
            const double originY,
            const DeviceClipBounds& clipBounds,
            std::vector<POINT>& points,
            std::vector<int>& ringCounts)
        {
            std::vector<DevicePoint> ringPoints;
            std::vector<DevicePoint> clippedPoints;
            for (std::size_t polygonIndex = 0; polygonIndex < scene.polygons.size(); ++polygonIndex)
            {
                const rendering::PolygonView polygon = scene.polygons[polygonIndex];
                points.clear();
                ringCounts.clear();
                for (std::size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex)
                {
                    const std::span<const rendering::Point> ring = polygon[ringIndex];
                    if (ring.size() < 3)
                    {
                        if (ringIndex == 0)
                        {
                            points.clear();
                            ringCounts.clear();
                            break;
                        }
                        continue;
                    }

                    const std::size_t maximumPoints =
                        static_cast<std::size_t>(std::numeric_limits<int>::max());
                    if (ring.size() > maximumPoints ||
                        points.size() > maximumPoints - ring.size())
                    {
                        return E_INVALIDARG;
                    }

                    ringPoints.clear();
                    ringPoints.reserve(ring.size());
                    for (const rendering::Point& point : ring)
                    {
                        ringPoints.push_back({
                            ToDeviceX(point, transform, originX),
                            ToDeviceY(point, transform, originY)});
                    }
                    ClipRingToBounds(ringPoints, clippedPoints, clipBounds);
                    if (ringPoints.size() < 3)
                    {
                        if (ringIndex == 0)
                        {
                            points.clear();
                            ringCounts.clear();
                            break;
                        }
                        continue;
                    }

                    if (ringCounts.size() >= maximumPoints ||
                        ringPoints.size() > maximumPoints ||
                        points.size() > maximumPoints - ringPoints.size())
                    {
                        return E_INVALIDARG;
                    }
                    ringCounts.push_back(static_cast<int>(ringPoints.size()));
                    for (const DevicePoint& point : ringPoints)
                    {
                        points.push_back({
                            static_cast<LONG>(std::floor(point.x)),
                            static_cast<LONG>(std::floor(point.y))});
                    }
                }

                if (!points.empty() &&
                    PolyPolygon(
                        deviceContext,
                        points.data(),
                        ringCounts.data(),
                        static_cast<int>(ringCounts.size())) == FALSE)
                {
                    return E_FAIL;
                }
            }
            return S_OK;
        }
    }

    HRESULT GdiVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::MapRenderScene& scene,
        const Viewport& viewport,
        const bool,
        const VectorRenderMode renderMode) noexcept
    {
        const diagnostics::PerformanceScope scope(
            diagnostics::PerformanceOperation::FrameRender);
        _failedLayers.clear();
        if (deviceContext == nullptr || !MatchesViewport(bounds, viewport))
        {
            return E_INVALIDARG;
        }

        const auto transform = ViewTransform::Create(viewport);
        if (!transform.has_value())
        {
            return E_INVALIDARG;
        }

        const ScopedDcState savedState(deviceContext);
        if (!savedState.IsValid())
        {
            return E_FAIL;
        }

        const int clipResult = IntersectClipRect(
            deviceContext, bounds.left, bounds.top, bounds.right, bounds.bottom);
        if (clipResult == ERROR)
        {
            return E_FAIL;
        }
        RECT clipBounds{};
        const int effectiveClip = GetClipBox(deviceContext, &clipBounds);
        if (effectiveClip == ERROR)
        {
            return E_FAIL;
        }
        if (effectiveClip == NULLREGION)
        {
            return S_OK;
        }

        const ScopedGdiObject fillBrush(
            renderMode == VectorRenderMode::Fill ? CreateSolidBrush(kFillColor) : nullptr);
        const HGDIOBJ brush = renderMode == VectorRenderMode::Fill
            ? fillBrush.Get()
            : GetStockObject(NULL_BRUSH);
        const ScopedGdiObject pen(CreatePen(PS_SOLID, 1, kOutlineColor));
        if (brush == nullptr || pen.Get() == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        const ScopedSelection selectedBrush(deviceContext, brush);
        const ScopedSelection selectedPen(deviceContext, pen.Get());
        if (!selectedBrush.IsValid() || !selectedPen.IsValid() ||
            SetPolyFillMode(deviceContext, ALTERNATE) == 0)
        {
            return E_FAIL;
        }

        const double originX = static_cast<double>(bounds.left);
        const double originY = static_cast<double>(bounds.top);
        const DeviceClipBounds coordinateClipBounds = MakeDeviceClipBounds(bounds);
        if (coordinateClipBounds.minimumX > bounds.left ||
            coordinateClipBounds.maximumX < bounds.right ||
            coordinateClipBounds.minimumY > bounds.top ||
            coordinateClipBounds.maximumY < bounds.bottom)
        {
            return E_INVALIDARG;
        }
        std::size_t drawnLayers = 0;
        try
        {
            // Reused for every polygon so a frame allocates only when a ring set
            // exceeds the largest one seen so far.
            std::vector<POINT> points;
            std::vector<int> ringCounts;
            for (const rendering::LayerRenderScene& layer : scene.layers)
            {
                if (layer.scene == nullptr ||
                    !HasFiniteCoordinates(*layer.scene, *transform, originX, originY))
                {
                    _failedLayers.push_back(layer.layerId);
                    continue;
                }

                const HRESULT layerResult = DrawLayer(
                    deviceContext,
                    *layer.scene,
                    *transform,
                    originX,
                    originY,
                    coordinateClipBounds,
                    points,
                    ringCounts);
                if (layerResult == E_INVALIDARG)
                {
                    _failedLayers.push_back(layer.layerId);
                    continue;
                }
                if (FAILED(layerResult))
                {
                    return layerResult;
                }
                ++drawnLayers;
            }
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
            return E_OUTOFMEMORY;
        }

        return drawnLayers == 0 && !_failedLayers.empty() ? E_INVALIDARG : S_OK;
    }
}
