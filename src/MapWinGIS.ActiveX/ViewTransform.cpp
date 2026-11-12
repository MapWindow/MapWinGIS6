// SPDX-License-Identifier: MPL-2.0

/**
 * @file ViewTransform.cpp
 * @brief Captures the Core viewport as the renderers' map-to-device transform.
 */
#include "ViewTransform.h"

namespace mwg::activex
{
    std::optional<ViewTransform> ViewTransform::Create(const Viewport& viewport) noexcept
    {
        const std::optional<MapPoint> center = viewport.Center();
        const std::optional<double> resolution = viewport.Resolution();
        if (!viewport.IsValid() || !center.has_value() || !resolution.has_value())
        {
            return std::nullopt;
        }

        // Same terms as Viewport::MapToScreen, so both give identical results.
        return ViewTransform{
            static_cast<double>(viewport.Width()) / 2.0,
            static_cast<double>(viewport.Height()) / 2.0,
            center->x,
            center->y,
            *resolution};
    }
}
