// SPDX-License-Identifier: MPL-2.0

/**
 * @file ViewTransform.cpp
 * @brief Builds the shared source-to-device transform for polygon renderers.
 */
#include "ViewTransform.h"

#include <algorithm>
#include <cmath>

namespace mwg::activex
{
    std::optional<ViewTransform> ViewTransform::Create(
        const Extent& extent,
        const double targetWidth,
        const double targetHeight) noexcept
    {
        const double extentWidth = extent.Width();
        const double extentHeight = extent.Height();
        const double availableWidth = targetWidth - 2.0 * kMargin;
        const double availableHeight = targetHeight - 2.0 * kMargin;
        if (extent.IsEmpty() || !std::isfinite(extentWidth) ||
            !std::isfinite(extentHeight) || extentWidth <= 0.0 ||
            extentHeight <= 0.0 || !std::isfinite(availableWidth) ||
            !std::isfinite(availableHeight) || availableWidth <= 0.0 ||
            availableHeight <= 0.0)
        {
            return std::nullopt;
        }

        const double scale = std::min(
            availableWidth / extentWidth,
            availableHeight / extentHeight);
        if (!std::isfinite(scale) || scale <= 0.0)
        {
            return std::nullopt;
        }

        const double drawingWidth = extentWidth * scale;
        const double drawingHeight = extentHeight * scale;
        const double offsetX = (targetWidth - drawingWidth) / 2.0;
        const double offsetY = (targetHeight - drawingHeight) / 2.0;
        if (!std::isfinite(drawingWidth) || !std::isfinite(drawingHeight) ||
            !std::isfinite(offsetX) || !std::isfinite(offsetY))
        {
            return std::nullopt;
        }

        return ViewTransform{
            scale,
            offsetX,
            offsetY,
            extent.xMin,
            extent.yMax};
    }
}
