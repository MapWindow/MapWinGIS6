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

        /// Checks every vertex of a layer before drawing so invalid geometry
        /// skips the whole layer instead of being partially rasterized.
        [[nodiscard]] bool HasDrawableCoordinates(
            const rendering::VectorRenderScene& scene,
            const ViewTransform& transform,
            const double originX,
            const double originY) noexcept
        {
            for (const rendering::Point& point : scene.points)
            {
                if (!IsDrawableCoordinate(ToDeviceX(point, transform, originX)) ||
                    !IsDrawableCoordinate(ToDeviceY(point, transform, originY)))
                {
                    return false;
                }
            }
            for (const rendering::Ring& line : scene.lines)
            {
                for (const rendering::Point& point : line)
                {
                    if (!IsDrawableCoordinate(ToDeviceX(point, transform, originX)) ||
                        !IsDrawableCoordinate(ToDeviceY(point, transform, originY)))
                    {
                        return false;
                    }
                }
            }
            for (const rendering::Polygon& polygon : scene.polygons)
            {
                for (const rendering::Ring& ring : polygon)
                {
                    for (const rendering::Point& point : ring)
                    {
                        if (!IsDrawableCoordinate(ToDeviceX(point, transform, originX)) ||
                            !IsDrawableCoordinate(ToDeviceY(point, transform, originY)))
                        {
                            return false;
                        }
                    }
                }
            }

            return true;
        }

        /// GDI addresses pixels by integer coordinates, so a continuous
        /// screen position is drawn in the pixel that contains it, as ToPixel
        /// defines.
        [[nodiscard]] POINT ToPoint(
            const rendering::Point& point,
            const ViewTransform& transform,
            const double originX,
            const double originY) noexcept
        {
            return {
                static_cast<LONG>(std::floor(ToDeviceX(point, transform, originX))),
                static_cast<LONG>(std::floor(ToDeviceY(point, transform, originY)))};
        }

        [[nodiscard]] HRESULT DrawLayer(
            const HDC deviceContext,
            const rendering::VectorRenderScene& scene,
            const ViewTransform& transform,
            const double originX,
            const double originY)
        {
            for (const rendering::Ring& line : scene.lines)
            {
                if (line.size() < 2)
                {
                    continue;
                }
                if (line.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return E_INVALIDARG;
                }
                std::vector<POINT> points;
                points.reserve(line.size());
                for (const rendering::Point& point : line)
                {
                    points.push_back(ToPoint(point, transform, originX, originY));
                }
                if (Polyline(deviceContext, points.data(), static_cast<int>(points.size())) == FALSE)
                {
                    return E_FAIL;
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

                    const std::size_t maximumPoints =
                        static_cast<std::size_t>(std::numeric_limits<int>::max());
                    if (ring.size() > maximumPoints ||
                        points.size() > maximumPoints - ring.size())
                    {
                        return E_INVALIDARG;
                    }

                    ringCounts.push_back(static_cast<int>(ring.size()));
                    for (const rendering::Point& point : ring)
                    {
                        points.push_back(ToPoint(point, transform, originX, originY));
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
                const POINT pixel = ToPoint(point, transform, originX, originY);
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

                if (!HasDrawableCoordinates(layer.scene, *transform, originX, originY))
                {
                    _failedLayers.push_back(layer.layerId);
                    continue;
                }

                const HRESULT layerResult =
                    DrawLayer(deviceContext, layer.scene, *transform, originX, originY);
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
