// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.h
 * @brief Declares source polygon geometry and optional fill data for rendering.
 *
 * Stores scene coordinates and polygon/ring ranges in flat buffers. Borrowed
 * polygon views let renderers read them without per-polygon allocations.
 * Fill meshes are prepared only when explicitly requested.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
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

        /// A contiguous range of coordinates or rings in a PolygonBuffer.
        struct GeometryRange
        {
            std::size_t offset;
            std::size_t count;
        };

        /// Borrowed read-only rings of one polygon. The first is its exterior.
        /// Invalidated by mutation, assignment, or destruction of its buffer.
        /// Ring access throws InvalidArgument for an out-of-range index,
        /// including front() on an empty view.
        class MWG_CORE_API PolygonView
        {
        public:
            [[nodiscard]] std::size_t size() const noexcept { return _rings.size(); }
            [[nodiscard]] bool empty() const noexcept { return _rings.empty(); }
            [[nodiscard]] std::span<const Point> operator[](std::size_t index) const;
            [[nodiscard]] std::span<const Point> front() const { return (*this)[0]; }

        private:
            friend class PolygonBuffer;
            PolygonView(std::span<const Point> coordinates,
                        std::span<const GeometryRange> rings) noexcept
                : _coordinates(coordinates), _rings(rings)
            {
            }

            std::span<const Point> _coordinates;
            std::span<const GeometryRange> _rings;
        };

        /// Owned flat coordinates, ring ranges, and polygon ranges.
        ///
        /// Copies own their storage; no spans are retained in the buffer.
        /// Topology is read-only except through AppendPolygon, which preserves
        /// it on failure. Not thread-safe. Polygon access throws InvalidArgument
        /// for an out-of-range index, including front() on an empty buffer.
        class MWG_CORE_API PolygonBuffer
        {
        public:
            PolygonBuffer() = default;
            PolygonBuffer(std::initializer_list<Polygon> polygons);
            PolygonBuffer(const PolygonBuffer&) = default;
            PolygonBuffer(PolygonBuffer&&) noexcept = default;
            PolygonBuffer& operator=(PolygonBuffer other) noexcept;

            [[nodiscard]] std::size_t size() const noexcept { return _polygons.size(); }
            [[nodiscard]] bool empty() const noexcept { return _polygons.empty(); }
            [[nodiscard]] PolygonView operator[](std::size_t index) const;
            [[nodiscard]] PolygonView front() const { return (*this)[0]; }
            [[nodiscard]] std::span<const Point> Coordinates() const noexcept { return _coordinates; }
            [[nodiscard]] std::span<Point> Coordinates() noexcept { return _coordinates; }
            [[nodiscard]] std::span<const GeometryRange> Rings() const noexcept { return _rings; }
            [[nodiscard]] std::span<const GeometryRange> Polygons() const noexcept { return _polygons; }

            /// Appends rings without retaining the input. ringEnds contains
            /// nondecreasing exclusive coordinate ends, including the final
            /// coordinates.size(). Empty rings and polygons are allowed.
            /// Input spans must not refer to this buffer's storage.
            /// \throws InvalidArgument when ringEnds does not partition coordinates.
            /// \throws std::bad_alloc when memory runs out.
            void AppendPolygon(std::span<const Point> coordinates,
                               std::span<const std::size_t> ringEnds);

            /// Convenience input for standalone polygons; scenes still store
            /// only flat buffers. Does not retain polygon or its rings.
            void AppendPolygon(const Polygon& polygon);

        private:
            std::vector<Point> _coordinates;
            std::vector<GeometryRange> _rings;
            std::vector<GeometryRange> _polygons;
        };

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

        /// Controls optional fill preparation while constructing a scene.
        struct VectorRenderSceneOptions
        {
            /// When false, preserves polygon rings and leaves fillMeshes empty.
            /// Enable this only for a rendering operation that needs triangle
            /// meshes. Source geometry and bounds are prepared either way.
            bool prepareFillMeshes = false;
            /// When present, only features whose bounding boxes intersect this
            /// extent are read and converted. This uses the source layer's
            /// spatial-filter capability and leaves the source filter restored
            /// after scene construction.
            /// \throws InvalidArgument when the extent is not valid.
            std::optional<Extent> spatialFilter;
        };

        /// Owned polygon geometry and its source-coordinate bounds.
        struct MWG_CORE_API VectorRenderScene
        {
            Extent extent;
            /// Feature count of the layer used to create this scene.
            std::int64_t sourceFeatureCount = 0;
            /// Non-empty geometry parts that were not rendered: non-polygonal
            /// geometries or collection members, and polygons without an
            /// exterior ring of at least three points.
            std::int64_t skippedGeometryCount = 0;
            PolygonBuffer polygons;
            /// Optional fill meshes prepared when the scene is created. This
            /// is empty when scene options omit fill preparation; otherwise
            /// fillMeshes[i] belongs to polygons[i]. An entry is std::nullopt
            /// when that polygon could not be triangulated; its rings remain
            /// available for outline drawing and for backends such as GDI.
            std::vector<std::optional<TriangleMesh>> fillMeshes;
        };

        /// Copies polygon geometry from a layer into backend-neutral scene data.
        ///
        /// Supports Polygon, MultiPolygon, and curved polygon layers. Empty
        /// geometries are skipped. Polygonal members of geometry collections
        /// are rendered; other geometries, and polygons whose exterior ring
        /// has fewer than three points, are skipped and counted in
        /// skippedGeometryCount. Interior rings with fewer than three points
        /// are dropped. The returned coordinates remain in the layer's coordinate
        /// system; this function does not transform projections. When options
        /// specify a spatial filter, sourceFeatureCount is the number of
        /// matching source features and extent covers only their geometry.
        /// Not thread-safe, consistent with the input layer's contract.
        /// \throws CoreError when the layer is not polygonal or contains no
        ///         polygon geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorLayer& layer);

        /// Creates a scene with explicit fill-preparation behavior.
        ///
        /// The default is source geometry only. Set prepareFillMeshes to true
        /// when the requested fill rendering operation needs triangle meshes.
        /// Set spatialFilter to avoid loading geometry outside a viewport.
        /// Features returned by an OGR spatial filter include bounding-box
        /// boundary intersections. The default overload never invokes polygon
        /// triangulation.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorLayer& layer,
            const VectorRenderSceneOptions& options);

        /// Copies polygon geometry from the first layer into owned scene data.
        ///
        /// A dataset with no layers is rejected. Only the first layer is
        /// considered. The scene's sourceFeatureCount matches that layer.
        /// \throws CoreError when the dataset has no layers or its first layer
        ///         is not polygonal or contains no polygon geometry to render.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorDataset& dataset);

        /// Creates the first layer's scene with explicit fill preparation.
        /// Layer selection and errors match the default overload.
        [[nodiscard]] MWG_CORE_API VectorRenderScene CreateVectorRenderScene(
            const VectorDataset& dataset,
            const VectorRenderSceneOptions& options);
    }
}
