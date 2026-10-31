// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.h
 * @brief Declares backend-neutral render data for all layers of a map.
 *
 * A map render scene is an owned snapshot of the drawable geometry of a Map,
 * in drawing order. It holds no references to the map, its layers, or their
 * datasets, and it holds no view state: renderers draw it through a separate
 * Viewport, so a view change does not need a new scene.
 */
#pragma once

#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::rendering
{
    /// Owned geometry of one map layer.
    struct LayerRenderScene
    {
        /// Identity of the map layer the geometry was copied from.
        LayerId layerId;
        /// Polygon geometry in map coordinates; never empty in a MapRenderScene.
        VectorRenderScene scene;
    };

    /// Why CreateMapRenderScene left a map layer out of the scene.
    enum class LayerSkipReason : int
    {
        /// The layer is hidden.
        Hidden,
        /// The layer has no polygon geometry to draw, for example an empty layer.
        NoGeometry,
        /// The layer cannot be rendered, for example because it is not a
        /// polygon layer or its geometry cannot be read. See the message.
        Failed,
    };

    /// A map layer that is not part of a MapRenderScene.
    struct SkippedLayer
    {
        LayerId layerId;
        LayerSkipReason reason = LayerSkipReason::Failed;
        /// Error message for LayerSkipReason::Failed; empty otherwise.
        std::string message;
    };

    /// Owned render data for the layers of a map.
    struct MWG_CORE_API MapRenderScene
    {
        /// Drawable layers in drawing order: the bottom map layer first.
        std::vector<LayerRenderScene> layers;
        /// Layers left out of the scene, in map order.
        std::vector<SkippedLayer> skippedLayers;
    };

    /// Copies the drawable geometry of every visible map layer into owned
    /// scene data, in map order (see Map).
    ///
    /// Each layer is converted on its own, so a layer that cannot be
    /// rendered is listed in skippedLayers and does not prevent the other
    /// layers from being drawn. Hidden layers and layers without polygon
    /// geometry are listed in skippedLayers too. Coordinates stay in the
    /// layers' source coordinate system; the scene does not reproject.
    ///
    /// The scene is a snapshot: later changes to the map or its sources are
    /// not reflected, so create a new scene after the map changes. Reads
    /// every visible layer completely. Not thread-safe; see Map.
    /// \throws std::bad_alloc when memory runs out. Other std::exception
    ///         errors from individual layers are reported in skippedLayers.
    ///         Exceptions not derived from std::exception propagate.
    [[nodiscard]] MWG_CORE_API MapRenderScene CreateMapRenderScene(const Map& map);
}
