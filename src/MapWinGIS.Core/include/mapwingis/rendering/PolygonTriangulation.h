// SPDX-License-Identifier: MPL-2.0

/**
 * @file PolygonTriangulation.h
 * @brief Declares reusable polygon triangulation for rendering.
 *
 * Converts a backend-neutral polygon with optional interior rings into an
 * indexed triangle mesh using the constrained Delaunay triangulation provided
 * by GDAL/OGR and GEOS. The mesh stays in the source coordinate system and
 * contains no graphics-API or platform types.
 */
#pragma once

#include "mapwingis/core/Api.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::rendering
{
    /// Triangulates a polygon with zero or more interior rings into a 2D
    /// fill mesh.
    ///
    /// The first ring of \p polygon is the exterior boundary; any further
    /// rings are interior holes. Rings do not need to repeat their first
    /// point at the end. Interior rings with fewer than three points are
    /// ignored. Duplicate consecutive vertices are tolerated.
    ///
    /// The triangulation is the constrained Delaunay triangulation from
    /// OGRGeometry::ConstrainedDelaunayTriangulation(), which preserves the
    /// polygon boundaries. Vertices are shared through the index buffer and
    /// all triangles have counter-clockwise winding. The result is the 2D
    /// rendering mesh; no Z or M values are computed.
    ///
    /// Invalid input is never repaired: self-intersecting or otherwise
    /// invalid polygons are rejected instead of being reinterpreted.
    /// \throws InvalidArgument when the polygon has no exterior ring of at
    ///         least three points or contains non-finite coordinates.
    /// \throws CoreError when the polygon is invalid or the triangulation
    ///         fails or produces no triangles.
    [[nodiscard]] MWG_CORE_API TriangleMesh TriangulatePolygon(const Polygon& polygon);
}
