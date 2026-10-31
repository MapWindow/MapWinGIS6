// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdiVectorLayerRenderer.cpp
 * @brief Draws vector geometry into a Windows GDI device context.
 *
 * Transforms every layer through the same Core viewport and paints points,
 * lines, and polygons without retaining scene or host resources.
 */
#include "GdiVectorLayerRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "ViewTransform.h"

#pragma comment(lib, "Msimg32.lib")

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

        class ScopedMemoryDc
        {
        public:
            explicit ScopedMemoryDc(HDC deviceContext) noexcept
                : _deviceContext(CreateCompatibleDC(deviceContext))
            {
            }

            ~ScopedMemoryDc()
            {
                if (_deviceContext != nullptr)
                {
                    DeleteDC(_deviceContext);
                }
            }

            [[nodiscard]] HDC Get() const noexcept { return _deviceContext; }

        private:
            HDC _deviceContext;
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

        [[nodiscard]] bool IsDrawableCoordinate(const double value) noexcept
        {
            return std::isfinite(value) &&
                value >= -kMaximumGdiCoordinate &&
                value <= kMaximumGdiCoordinate;
        }

        [[nodiscard]] bool HasFiniteCoordinates(
            const rendering::VectorRenderScene& scene) noexcept
        {
            const auto finite = [](const rendering::Point& point)
            {
                return std::isfinite(point.x) && std::isfinite(point.y);
            };
            if (!std::all_of(scene.points.begin(), scene.points.end(), finite))
            {
                return false;
            }
            for (const rendering::Ring& line : scene.lines)
            {
                if (!std::all_of(line.begin(), line.end(), finite))
                {
                    return false;
                }
            }
            for (const rendering::Polygon& polygon : scene.polygons)
            {
                for (const rendering::Ring& ring : polygon)
                {
                    if (!std::all_of(ring.begin(), ring.end(), finite))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        struct DevicePoint
        {
            double x = 0.0;
            double y = 0.0;
        };

        [[nodiscard]] DevicePoint ToDevicePoint(
            const rendering::Point& point,
            const ViewTransform& transform,
            const double originX,
            const double originY) noexcept
        {
            return {
                ToDeviceX(point, transform, originX),
                ToDeviceY(point, transform, originY)};
        }

        [[nodiscard]] POINT ToPoint(const DevicePoint& point) noexcept
        {
            return {
                static_cast<LONG>(std::floor(point.x)),
                static_cast<LONG>(std::floor(point.y))};
        }

        [[nodiscard]] bool ClipSegment(
            DevicePoint& start,
            DevicePoint& end,
            const RECT& clip) noexcept
        {
            if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
                !std::isfinite(end.x) || !std::isfinite(end.y))
            {
                return false;
            }

            const double deltaX = end.x - start.x;
            const double deltaY = end.y - start.y;
            if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
            {
                return false;
            }

            double minimum = 0.0;
            double maximum = 1.0;
            const double p[] = {-deltaX, deltaX, -deltaY, deltaY};
            const double q[] = {
                start.x - clip.left,
                clip.right - start.x,
                start.y - clip.top,
                clip.bottom - start.y};
            for (int edge = 0; edge < 4; ++edge)
            {
                if (p[edge] == 0.0)
                {
                    if (q[edge] < 0.0)
                    {
                        return false;
                    }
                    continue;
                }

                const double ratio = q[edge] / p[edge];
                if (p[edge] < 0.0)
                {
                    minimum = std::max(minimum, ratio);
                }
                else
                {
                    maximum = std::min(maximum, ratio);
                }
                if (minimum > maximum)
                {
                    return false;
                }
            }

            const DevicePoint originalStart = start;
            start = {
                originalStart.x + minimum * deltaX,
                originalStart.y + minimum * deltaY};
            end = {
                originalStart.x + maximum * deltaX,
                originalStart.y + maximum * deltaY};
            return true;
        }

        [[nodiscard]] bool IsInside(
            const DevicePoint& point,
            const int edge,
            const RECT& clip) noexcept
        {
            switch (edge)
            {
            case 0:
                return point.x >= clip.left;
            case 1:
                return point.x <= clip.right;
            case 2:
                return point.y >= clip.top;
            default:
                return point.y <= clip.bottom;
            }
        }

        [[nodiscard]] DevicePoint IntersectEdge(
            const DevicePoint& start,
            const DevicePoint& end,
            const int edge,
            const RECT& clip) noexcept
        {
            if (edge < 2)
            {
                const double x = edge == 0 ? clip.left : clip.right;
                const double ratio = (x - start.x) / (end.x - start.x);
                return {x, start.y + ratio * (end.y - start.y)};
            }

            const double y = edge == 2 ? clip.top : clip.bottom;
            const double ratio = (y - start.y) / (end.y - start.y);
            return {start.x + ratio * (end.x - start.x), y};
        }

        [[nodiscard]] std::vector<DevicePoint> ClipRing(
            const rendering::Ring& ring,
            const ViewTransform& transform,
            const double originX,
            const double originY,
            const RECT& clip)
        {
            std::vector<DevicePoint> points;
            points.reserve(ring.size());
            for (const rendering::Point& point : ring)
            {
                const DevicePoint device =
                    ToDevicePoint(point, transform, originX, originY);
                if (!std::isfinite(device.x) || !std::isfinite(device.y))
                {
                    return {};
                }
                points.push_back(device);
            }

            for (int edge = 0; edge < 4 && !points.empty(); ++edge)
            {
                std::vector<DevicePoint> clipped;
                clipped.reserve(points.size() + 4U);
                DevicePoint previous = points.back();
                bool previousInside = IsInside(previous, edge, clip);
                for (const DevicePoint& current : points)
                {
                    const bool currentInside = IsInside(current, edge, clip);
                    if (currentInside != previousInside)
                    {
                        clipped.push_back(IntersectEdge(previous, current, edge, clip));
                    }
                    if (currentInside)
                    {
                        clipped.push_back(current);
                    }
                    previous = current;
                    previousInside = currentInside;
                }
                points = std::move(clipped);
            }
            return points;
        }

        [[nodiscard]] HRESULT DrawClippedLine(
            const HDC deviceContext,
            const rendering::Ring& line,
            const ViewTransform& transform,
            const double originX,
            const double originY,
            const RECT& clip)
        {
            if (line.size() < 2)
            {
                return S_OK;
            }
            for (std::size_t index = 1; index < line.size(); ++index)
            {
                DevicePoint start =
                    ToDevicePoint(line[index - 1], transform, originX, originY);
                DevicePoint end =
                    ToDevicePoint(line[index], transform, originX, originY);
                if (!ClipSegment(start, end, clip))
                {
                    continue;
                }
                const POINT points[] = {ToPoint(start), ToPoint(end)};
                if (Polyline(deviceContext, points, 2) == FALSE)
                {
                    return E_FAIL;
                }
            }
            return S_OK;
        }

        [[nodiscard]] HRESULT DrawLayer(
            const HDC deviceContext,
            const rendering::VectorRenderScene& scene,
            const ViewTransform& transform,
            const double originX,
            const double originY,
            const RECT& clip)
        {
            for (const rendering::Ring& line : scene.lines)
            {
                const HRESULT lineResult = DrawClippedLine(
                    deviceContext, line, transform, originX, originY, clip);
                if (FAILED(lineResult))
                {
                    return lineResult;
                }
            }

            for (const rendering::Polygon& polygon : scene.polygons)
            {
                std::vector<POINT> points;
                std::vector<int> ringCounts;
                for (const rendering::Ring& ring : polygon)
                {
                    if (ring.size() < 3)
                    {
                        continue;
                    }

                    std::vector<DevicePoint> clipped = ClipRing(
                        ring, transform, originX, originY, clip);
                    if (clipped.size() < 3)
                    {
                        continue;
                    }
                    const std::size_t maximumPoints =
                        static_cast<std::size_t>(std::numeric_limits<int>::max());
                    if (clipped.size() > maximumPoints ||
                        points.size() > maximumPoints - clipped.size())
                    {
                        return E_INVALIDARG;
                    }

                    ringCounts.push_back(static_cast<int>(clipped.size()));
                    for (const DevicePoint& point : clipped)
                    {
                        points.push_back(ToPoint(point));
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

            for (const rendering::Point& point : scene.points)
            {
                const DevicePoint device = ToDevicePoint(
                    point, transform, originX, originY);
                if (!std::isfinite(device.x) || !std::isfinite(device.y) ||
                    device.x < clip.left - 3.0 || device.x > clip.right + 3.0 ||
                    device.y < clip.top - 3.0 || device.y > clip.bottom + 3.0)
                {
                    continue;
                }
                const POINT pixel = ToPoint(device);
                if (Ellipse(
                        deviceContext,
                        pixel.x - 3,
                        pixel.y - 3,
                        pixel.x + 4,
                        pixel.y + 4) == FALSE)
                {
                    return E_FAIL;
                }
            }
            return S_OK;
        }

        [[nodiscard]] HRESULT DrawRasterLayer(
            const HDC deviceContext,
            const RasterImage& image,
            const ViewTransform& transform,
            const double originX,
            const double originY)
        {
            if (image.width <= 0 || image.height <= 0 || !image.extent.IsValid() ||
                image.pixels.size() !=
                    static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height))
            {
                return E_INVALIDARG;
            }

            const rendering::Point topLeft{image.extent.xMin, image.extent.yMax};
            const rendering::Point bottomRight{image.extent.xMax, image.extent.yMin};
            const double leftValue = ToDeviceX(topLeft, transform, originX);
            const double topValue = ToDeviceY(topLeft, transform, originY);
            const double rightValue = ToDeviceX(bottomRight, transform, originX);
            const double bottomValue = ToDeviceY(bottomRight, transform, originY);
            if (!IsDrawableCoordinate(leftValue) || !IsDrawableCoordinate(topValue) ||
                !IsDrawableCoordinate(rightValue) || !IsDrawableCoordinate(bottomValue))
            {
                return E_INVALIDARG;
            }

            const LONG left = static_cast<LONG>(std::floor(leftValue));
            const LONG top = static_cast<LONG>(std::floor(topValue));
            const LONG right = static_cast<LONG>(std::ceil(rightValue));
            const LONG bottom = static_cast<LONG>(std::ceil(bottomValue));
            if (right <= left || bottom <= top)
            {
                return S_OK;
            }

            ScopedMemoryDc memoryDc(deviceContext);
            if (memoryDc.Get() == nullptr)
            {
                return E_FAIL;
            }

            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = image.width;
            bitmapInfo.bmiHeader.biHeight = -image.height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            const HBITMAP bitmap = CreateDIBSection(
                memoryDc.Get(),
                &bitmapInfo,
                DIB_RGB_COLORS,
                &pixels,
                nullptr,
                0);
            const ScopedGdiObject ownedBitmap(bitmap);
            if (bitmap == nullptr || pixels == nullptr)
            {
                return E_OUTOFMEMORY;
            }
            std::memcpy(
                pixels,
                image.pixels.data(),
                image.pixels.size() * sizeof(std::uint32_t));

            const ScopedSelection selectedBitmap(memoryDc.Get(), bitmap);
            if (!selectedBitmap.IsValid())
            {
                return E_FAIL;
            }
            const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            return AlphaBlend(
                deviceContext,
                left,
                top,
                right - left,
                bottom - top,
                memoryDc.Get(),
                0,
                0,
                image.width,
                image.height,
                blend) != FALSE
                ? S_OK
                : E_FAIL;
        }
    }

    HRESULT GdiVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::MapRenderScene& scene,
        const Viewport& viewport) noexcept
    {
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

        const ScopedGdiObject brush(CreateSolidBrush(kFillColor));
        const ScopedGdiObject pen(CreatePen(PS_SOLID, 1, kOutlineColor));
        if (brush.Get() == nullptr || pen.Get() == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        const ScopedSelection selectedBrush(deviceContext, brush.Get());
        const ScopedSelection selectedPen(deviceContext, pen.Get());
        if (!selectedBrush.IsValid() || !selectedPen.IsValid() ||
            SetPolyFillMode(deviceContext, ALTERNATE) == 0)
        {
            return E_FAIL;
        }

        const double originX = static_cast<double>(bounds.left);
        const double originY = static_cast<double>(bounds.top);
        const RECT clip = bounds;
        std::size_t drawnLayers = 0;
        try
        {
            for (const rendering::LayerRenderScene& layer : scene.layers)
            {
                if (layer.raster.has_value())
                {
                    const HRESULT result = DrawRasterLayer(
                        deviceContext,
                        *layer.raster,
                        *transform,
                        originX,
                        originY);
                    if (result == E_INVALIDARG)
                    {
                        _failedLayers.push_back(layer.layerId);
                        continue;
                    }
                    if (FAILED(result))
                    {
                        return result;
                    }
                    ++drawnLayers;
                    continue;
                }

                if (!HasFiniteCoordinates(layer.scene))
                {
                    _failedLayers.push_back(layer.layerId);
                    continue;
                }

                const HRESULT layerResult = DrawLayer(
                    deviceContext,
                    layer.scene,
                    *transform,
                    originX,
                    originY,
                    clip);
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
