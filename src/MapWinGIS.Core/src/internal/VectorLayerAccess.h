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

#include "mapwingis/vector/VectorDataset.h"

namespace mwg::detail
{
    struct VectorLayerAccess final
    {
        static void ForEachGeometry(
            const VectorLayer& layer,
            const std::function<void(const OGRGeometry&)>& callback);
    };
}
