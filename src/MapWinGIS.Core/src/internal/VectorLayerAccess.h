// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerAccess.h
 * @brief Declares private sequential access to vector layer geometries.
 *
 * Gives Core algorithms direct OGR geometry access without exposing GDAL
 * types through the public VectorLayer API.
 */
#pragma once

#include <functional>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Extent.h"
#include "mapwingis/vector/VectorDataset.h"

namespace mwg::detail
{
    struct VectorLayerAccess final
    {
        /// Returns the exact number of features, including absent geometries.
        /// When extent is supplied, computes and caches exact bounds in the
        /// same traversal. Neither cached result changes after a failed scan.
        static std::int64_t ForEachGeometry(
            const VectorLayer& layer,
            const std::function<void(const OGRGeometry&)>& callback,
            Extent* extent = nullptr,
            const Extent* spatialFilter = nullptr);

        /// Whether the source driver can return an extent without scanning
        /// every feature.
        [[nodiscard]] static bool HasFastExtent(const VectorLayer& layer);

        /// Whether MapView can safely reduce feature reads with the layer's
        /// single active geometry field.
        [[nodiscard]] static bool HasFastSpatialFilter(const VectorLayer& layer);
    };
}
