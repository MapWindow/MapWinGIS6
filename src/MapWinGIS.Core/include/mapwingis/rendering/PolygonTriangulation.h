// SPDX-License-Identifier: MPL-2.0

/**
 * @file PolygonTriangulation.h
 * @brief Declares reusable polygon triangulation for rendering.
 *
 * Converts a backend-neutral polygon with optional interior rings into an
 * indexed triangle mesh. The implementation uses earcut first and a GEOS
 * constrained Delaunay triangulation fallback. The mesh stays in the source
 * coordinate system and contains no graphics-API or platform types.
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
    /// Earcut triangulates the original ring coordinates first. If it cannot
    /// produce a mesh, the implementation falls back to
    /// OGRGeometry::ConstrainedDelaunayTriangulation(). Vertices are shared
    /// through the index buffer and all triangles have counter-clockwise
    /// winding. The result is the 2D rendering mesh; no Z or M values are
    /// computed. Earcut works directly from the supplied rings and does not
    /// run an OGC validity precheck. A mesh is rejected when its area does
    /// not agree with the signed ring area, which keeps ambiguous
    /// self-intersections on the caller's existing failure fallback.
    ///
    /// \throws InvalidArgument when the polygon has no exterior ring of at
    ///         least three points or contains non-finite coordinates.
    /// \throws CoreError when both triangulation paths fail or no triangles
    ///         can be produced.
    [[nodiscard]] MWG_CORE_API TriangleMesh TriangulatePolygon(const Polygon& polygon);

    /// Triangulates borrowed flat-buffer rings with the same contract as the
    /// owning-polygon overload, without rebuilding nested coordinate vectors.
    [[nodiscard]] MWG_CORE_API TriangleMesh TriangulatePolygon(PolygonView polygon);
}
