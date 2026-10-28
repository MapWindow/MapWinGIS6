// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdiVectorLayerRenderer.cpp
 * @brief Draws polygon render scenes into a Windows GDI device context.
 *
 * Fits source coordinates into the control bounds and paints polygon fills
 * and outlines without retaining scene or host resources.
 */
#include "GdiVectorLayerRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mwg::activex
{
    namespace
    {
        constexpr int kMargin = 12;
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

        [[nodiscard]] POINT ToPoint(
            const rendering::Point& point,
            const rendering::VectorRenderScene& scene,
            const double scale,
            const double left,
            const double top) noexcept
        {
            return {
                static_cast<LONG>(std::lround(left + (point.x - scene.extent.xMin) * scale)),
                static_cast<LONG>(std::lround(top + (scene.extent.yMax - point.y) * scale))};
        }
    }

    HRESULT GdiVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept
    {
        if (deviceContext == nullptr || scene.extent.IsEmpty() ||
            scene.extent.Width() <= 0.0 || scene.extent.Height() <= 0.0)
        {
            return E_INVALIDARG;
        }

        const double availableWidth = static_cast<double>(bounds.right - bounds.left - 2 * kMargin);
        const double availableHeight = static_cast<double>(bounds.bottom - bounds.top - 2 * kMargin);
        if (availableWidth <= 0.0 || availableHeight <= 0.0)
        {
            return S_OK;
        }

        const double scale = std::min(
            availableWidth / scene.extent.Width(),
            availableHeight / scene.extent.Height());
        if (!std::isfinite(scale) || scale <= 0.0)
        {
            return E_INVALIDARG;
        }

        const double drawingWidth = scene.extent.Width() * scale;
        const double drawingHeight = scene.extent.Height() * scale;
        const double left = static_cast<double>(bounds.left) +
                            (static_cast<double>(bounds.right - bounds.left) - drawingWidth) / 2.0;
        const double top = static_cast<double>(bounds.top) +
                           (static_cast<double>(bounds.bottom - bounds.top) - drawingHeight) / 2.0;

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

        try
        {
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
                        points.push_back(ToPoint(point, scene, scale, left, top));
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
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
            return E_OUTOFMEMORY;
        }

        return S_OK;
    }
}
