// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneBuilder.h
 * @brief Declares private scene building shared by layer and map scenes.
 *
 * Shares polygon preparation and per-layer failure classification between
 * standalone map snapshots and the map view's scene cache.
 */
#pragma once

#include <variant>

#include "mapwingis/rendering/MapRenderScene.h"

namespace mwg::rendering::detail
{
    using LayerScene = std::variant<LayerRenderScene, SkippedLayer>;

    /// Builds drawable geometry or a skip diagnostic for one map layer.
    /// Allocation failures and non-standard exceptions propagate.
    [[nodiscard]] LayerScene BuildLayerRenderScene(
        const MapLayer& layer,
        const VectorRenderSceneOptions& options = {});

    /// Copies polygon geometry from a layer into scene data. Unlike
    /// CreateVectorRenderScene, a layer without polygon geometry gives a
    /// scene with no polygons instead of an error.
    /// \throws CoreError when the layer is not polygonal or its geometry
    ///         cannot be converted.
    [[nodiscard]] VectorRenderScene BuildVectorRenderScene(
        const VectorLayer& layer,
        const VectorRenderSceneOptions& options = {});

    /// Rejects programmer errors in scene options before per-layer failures
    /// can convert them into skipped-layer diagnostics.
    void ValidateVectorRenderSceneOptions(const VectorRenderSceneOptions& options);
}
