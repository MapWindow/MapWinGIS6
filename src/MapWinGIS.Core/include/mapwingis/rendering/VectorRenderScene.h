// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.h
 * @brief Declares backend-neutral vector geometry for rendering.
 *
 * Converts polygon features from a vector layer into owned coordinate data
 * that renderers can consume without retaining dataset-owned objects.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Extent.h"

namespace mwg
{
    class VectorDataset;
    class VectorLayer;

    namespace rendering
    {
        /// A two-dimensional coordinate in the source layer's coordinate system.
        struct Point
        {
            double x;
            double y;
        };

        /// One closed polygon boundary, including its exterior or an interior hole.
        using Ring = std::vector<Point>;

        /// A polygon and its rings. The first ring is the exterior boundary.
        using Polygon = std::vector<Ring>;

        /// Owned point, line, and polygon geometry in map coordinates.
        struct MWG_CORE_API VectorRenderScene
        {
            Extent extent;
            /// Feature count of the layer used to create this scene.
            std::int64_t sourceFeatureCount = 0;
            std::vector<Polygon> polygons;
            std::vector<Point> points;
            std::vector<Ring> lines;
        };

        /// Copies polygon geometry from a layer into backend-neutral scene data.
        ///
        /// Supports Point, MultiPoint, LineString, MultiLineString, Polygon,
        /// MultiPolygon, and their curved OGR equivalents. Empty geometries
        /// are skipped. Coordinates remain in the layer's coordinate system.
        /// Not thread-safe, consistent with the input layer's contract.
        /// \throws CoreError when the layer uses an unsupported geometry type
        ///         or contains no geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorLayer& layer);

        /// Copies vector geometry from the first layer into owned scene data.
        ///
        /// A dataset with no layers is rejected. Only the first layer is
        /// considered. The scene's sourceFeatureCount matches that layer.
        /// \throws CoreError when the dataset has no layers or its first layer
        /// has an unsupported geometry type or contains no geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorDataset& dataset);
    }
}
