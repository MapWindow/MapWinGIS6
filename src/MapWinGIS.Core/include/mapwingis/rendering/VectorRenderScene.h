// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.h
 * @brief Declares backend-neutral polygon scene data for vector rendering.
 *
 * Converts polygon features from a vector layer into owned coordinate data
 * that renderers can consume without retaining dataset-owned objects.
 */
#pragma once

#include <cstdint>
#include <optional>
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

        /// A backend-neutral triangulated 2D fill mesh for one polygon.
        ///
        /// Vertices stay in the source layer's coordinate system. Indices
        /// occur in groups of three that each describe one triangle; every
        /// index references a valid vertex. Triangles produced by
        /// TriangulatePolygon share vertices through the index buffer and
        /// have consistent counter-clockwise winding. The representation
        /// contains no graphics-API, Windows, COM, ATL, or MFC types.
        struct MWG_CORE_API TriangleMesh
        {
            std::vector<Point> vertices;
            std::vector<std::uint32_t> indices;
        };

        /// Owned polygon geometry and its source-coordinate bounds.
        struct MWG_CORE_API VectorRenderScene
        {
            Extent extent;
            /// Feature count of the layer used to create this scene.
            std::int64_t sourceFeatureCount = 0;
            std::vector<Polygon> polygons;
            /// Triangulated fill meshes prepared once when the scene is
            /// created, aligned with polygons: fillMeshes[i] belongs to
            /// polygons[i]. An entry is std::nullopt when that polygon could
            /// not be triangulated; its rings in polygons remain available
            /// for outline drawing and for backends that draw rings directly,
            /// such as GDI.
            std::vector<std::optional<TriangleMesh>> fillMeshes;
        };

        /// Copies polygon geometry from a layer into backend-neutral scene data.
        ///
        /// Supports Polygon and MultiPolygon layers. Empty geometries are
        /// skipped. The returned coordinates remain in the layer's coordinate
        /// system; this function does not transform projections. Not
        /// thread-safe, consistent with the input layer's contract.
        /// \throws CoreError when the layer is not polygonal or contains no
        ///         polygon geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorLayer& layer);

        /// Copies polygon geometry from the first layer into owned scene data.
        ///
        /// A dataset with no layers is rejected. Only the first layer is
        /// considered. The scene's sourceFeatureCount matches that layer.
        /// \throws CoreError when the dataset has no layers or its first layer
        ///         is not polygonal or contains no polygon geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorDataset& dataset);
    }
}
