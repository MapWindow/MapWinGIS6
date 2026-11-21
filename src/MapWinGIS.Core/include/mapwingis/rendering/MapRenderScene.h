// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.h
 * @brief Declares backend-neutral render data for all layers of a map.
 *
 * A map render scene is a snapshot of the drawable geometry of a Map, in
 * drawing order. Layer geometry is immutable and shared through
 * reference-counted pointers. The scene holds no references to the map, its
 * layers, or their datasets, and it holds no view state: renderers draw it
 * through a separate Viewport, so a view change does not need a new scene.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::rendering
{
    /// Geometry of one map layer.
    struct LayerRenderScene
    {
        /// Identity of the map layer the geometry was copied from.
        LayerId layerId;
        /// Immutable polygon geometry in map coordinates; never null or empty
        /// in a MapRenderScene. Shared, not copied, between a MapView's layer
        /// cache and the scenes it assembles; replace the pointer rather than
        /// modifying the geometry.
        std::shared_ptr<const VectorRenderScene> scene;
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
        /// No polygon geometry intersects an applied spatial filter.
        OutsideViewport,
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
    /// not reflected, so create a new scene after the map changes. With no
    /// spatial filter, every visible layer is read completely. Not
    /// thread-safe; see Map.
    /// \throws std::bad_alloc when memory runs out. Other std::exception
    ///         errors from individual layers are reported in skippedLayers.
    ///         Exceptions not derived from std::exception propagate.
    [[nodiscard]] MWG_CORE_API MapRenderScene CreateMapRenderScene(const Map& map);

    /// Creates a map scene with explicit fill-preparation behavior.
    ///
    /// Hidden, empty, and failed layers retain the same handling as the
    /// default overload. Fill preparation is disabled by default, so drawable
    /// layer scenes contain source rings without triangulating polygons.
    /// \throws InvalidArgument when the spatial-filter extent is invalid.
    [[nodiscard]] MWG_CORE_API MapRenderScene CreateMapRenderScene(
        const Map& map,
        const VectorRenderSceneOptions& options);
}
