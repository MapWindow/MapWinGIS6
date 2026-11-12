// SPDX-License-Identifier: MPL-2.0

/**
 * @file ViewTransform.h
 * @brief Declares the map-to-device transform shared by the polygon renderers.
 *
 * Captures the Core viewport once per frame so the GDI and OpenGL backends
 * transform every vertex of every layer with the same values and the same
 * arithmetic as Viewport::MapToScreen, without per-vertex optional results.
 */
#pragma once

#include <optional>

#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::activex
{
    struct ViewTransform final
    {
        /// Returns std::nullopt when the viewport is not valid.
        [[nodiscard]] static std::optional<ViewTransform> Create(
            const Viewport& viewport) noexcept;

        /// Screen X of a map point, relative to the viewport's left edge.
        [[nodiscard]] double ToDeviceX(const rendering::Point& point) const noexcept
        {
            return halfWidth + (point.x - centerX) / resolution;
        }

        /// Screen Y of a map point, relative to the viewport's top edge.
        [[nodiscard]] double ToDeviceY(const rendering::Point& point) const noexcept
        {
            return halfHeight - (point.y - centerY) / resolution;
        }

        double halfWidth = 0.0;
        double halfHeight = 0.0;
        double centerX = 0.0;
        double centerY = 0.0;
        double resolution = 1.0;
    };
}
