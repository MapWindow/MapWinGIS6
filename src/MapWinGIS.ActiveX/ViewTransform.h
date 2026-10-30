// SPDX-License-Identifier: MPL-2.0

/**
 * @file ViewTransform.h
 * @brief Declares the shared source-to-device transform for polygon renderers.
 *
 * Provides the same aspect-preserving scale and centered margin to the GDI
 * and OpenGL rendering backends.
 */
#pragma once

#include <optional>

#include "mapwingis/core/Extent.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::activex
{
    struct ViewTransform final
    {
        static constexpr double kMargin = 12.0;

        [[nodiscard]] static std::optional<ViewTransform> Create(
            const Extent& extent,
            double targetWidth,
            double targetHeight) noexcept;

        [[nodiscard]] double ToDeviceX(const rendering::Point& point) const noexcept
        {
            return offsetX + (point.x - xMin) * scale;
        }

        [[nodiscard]] double ToDeviceY(const rendering::Point& point) const noexcept
        {
            return offsetY + (yMax - point.y) * scale;
        }

        double scale = 0.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double xMin = 0.0;
        double yMax = 0.0;
    };
}
