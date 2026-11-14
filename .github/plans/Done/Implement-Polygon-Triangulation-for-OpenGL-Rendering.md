# Implement Polygon Triangulation for OpenGL Rendering

## Overview

Make the OpenGL rendering backend support concave polygons, polygons with interior rings (holes), and multipart polygon geometries.

Currently, the OpenGL backend only accepts convex polygons containing a single ring. Unsupported geometry causes the rendering coordinator to fall back to GDI.

Replace this limitation with proper polygon triangulation using the existing GDAL/GEOS dependencies.

**Do not implement a triangulation algorithm from scratch.** Use GDAL/OGR's `OGRGeometry::ConstrainedDelaunayTriangulation()`, which delegates to GEOS.

The triangulation functionality must live in `MapWinGIS.Core` as reusable, backend-neutral geometry functionality. The OpenGL backend in `MapWinGIS.ActiveX` consumes the resulting meshes and renders them using OpenGL vertex arrays.

GDI remains the fallback for systems without framebuffer-object support and for situations where OpenGL cannot render a scene.

## Phase checklist

- [x] **Phase 1 — Core polygon triangulation and render-scene integration** (PR 1)
  - Implement a reusable polygon triangulation utility using GDAL/OGR and GEOS.
  - Introduce backend-neutral triangle mesh data.
  - Integrate reusable meshes into the Core rendering scene.
  - Add comprehensive GoogleTest coverage.
  - Remaining limitations: the OpenGL backend does not consume the meshes yet
    (Phase 2), so rendering behavior is unchanged; GDI continues to draw the
    original rings. Documentation updates are deferred to Phase 3.

- [x] **Phase 2 — OpenGL mesh rendering and removal of convexity restrictions** (PR 2)
  - Replace `GL_POLYGON` with indexed triangle rendering.
  - Support concave polygons, interior rings and multipart geometries.
  - Remove the convexity and single-ring restrictions.
  - Preserve polygon outlines and the existing rendering coordinator and GDI fallback.
  - Add OpenGL renderer tests.
  - Remaining limitations: performance characteristics and mesh memory usage
    have not been measured yet (Phase 3); the architecture documentation
    still describes the old convex-polygon restriction and is updated in
    Phase 3.

- [x] **Phase 3 — Integration verification, performance and documentation** (PR 3)
  - Verify GDI/OpenGL rendering consistency.
  - Test complex geometries and fallback scenarios.
  - Check resource management and performance.
  - Update architecture and rendering documentation.
  - Remaining limitations: performance numbers are from one developer
    workstation and the opt-in Release baseline; the C# interactive host
    checks still require a manual desktop run and are not part of CI.

---

# PR 1 — Core polygon triangulation and render-scene integration

## 1.1. Investigate the existing geometry and dependency infrastructure

Before implementation, inspect:

- `src/MapWinGIS.Core/include/mapwingis/rendering/VectorRenderScene.h`
- `src/MapWinGIS.Core/src/rendering/VectorRenderScene.cpp`
- `src/MapWinGIS.Core/src/internal/VectorRenderSceneBuilder.h`
- `src/MapWinGIS.Core/src/vector/VectorDataset.cpp`
- `src/MapWinGIS.Core/MapWinGIS.Core.vcxproj`
- `tests/MapWinGIS.Core.Tests/VectorRenderSceneTests.cpp`
- `tests/MapWinGIS.Core.Tests/MapRenderSceneTests.cpp`
- `docs/ARCHITECTURE.md`
- `docs/TESTING.md`

Review how existing OGR geometries are constructed, owned and converted into the current backend-neutral rendering representation.

The current rendering model uses:

- `rendering::Point`
- `rendering::Ring`
- `rendering::Polygon`
- `rendering::VectorRenderScene`

A polygon consists of one exterior ring followed by zero or more interior rings.

A multipolygon is represented by multiple independent polygons in the scene.

Preserve these semantics.

## 1.2. Use the existing constrained Delaunay triangulation implementation

Use:

```cpp
OGRGeometry::ConstrainedDelaunayTriangulation()
```

This method:

- Is available since GDAL/OGR 3.12.
- Requires GEOS 3.10 or newer.
- Produces triangular polygon geometries.
- Preserves polygon constraints rather than merely triangulating an arbitrary point cloud.

Do not use ordinary `DelaunayTriangulation()` as a substitute. It triangulates the vertices without enforcing polygon boundaries.

Relevant documentation:

https://gdal.org/en/stable/api/ogrgeometry_cpp.html

The repository already declares GDAL 3.12.4 and GEOS in `vcpkg.json`. Do not add another dependency or modify the dependency configuration unless an actual compatibility problem is discovered.

Use the existing OGR C++ API rather than introducing another direct GEOS integration or maintaining separate GEOS context management.

## 1.3. Introduce a backend-neutral triangle mesh

Create an appropriate reusable geometry representation in the Core rendering/geometry namespace.

A possible design:

```cpp
struct TriangleMesh
{
    std::vector<Point> vertices;
    std::vector<uint32_t> indices;
};
```

Requirements:

- Indices must always occur in groups of three.
- Each group describes one triangle.
- Vertex references must be valid.
- Avoid unnecessary duplicate vertices.
- Triangle winding must be consistent.
- Mesh coordinates must remain in the original map coordinate system.
- The representation must not contain OpenGL, Windows, COM, ATL or MFC types.

Use indexed geometry so the OpenGL backend can efficiently use `glDrawElements()`.

The public Core API must not expose OGR or GEOS types.

Keep the interface consistent with existing Core conventions, including exported API declarations and appropriate ownership.

## 1.4. Implement the polygon triangulation utility

Introduce a reusable utility, for example:

```cpp
TriangleMesh TriangulatePolygon(const Polygon& polygon);
```

The exact API and file placement can be adjusted to fit the existing architecture.

The implementation should:

1. Accept a polygon with an exterior ring and zero or more interior rings.
2. Convert the polygon into an OGR polygon.
3. Preserve the existing shell/hole semantics.
4. Validate the input geometry where appropriate.
5. Call `ConstrainedDelaunayTriangulation()`.
6. Traverse the resulting geometry collection.
7. Extract the three vertices of each resulting triangle.
8. Build an indexed `TriangleMesh`.
9. Return the mesh without retaining OGR or GEOS objects.

Use RAII for all temporary OGR geometries.

Handle OGR geometry ownership correctly, including the destruction of returned triangulation geometries.

Do not silently repair invalid geometries using `MakeValid()` or another geometry operation. Invalid input should have an explicit and documented failure behavior.

Handle empty geometries, degenerate rings, duplicate coordinates and triangulation failures safely.

### Geometry semantics

The current Core representation already distinguishes exterior and interior rings. Use that information to construct the OGR polygon.

There is no need to implement a separate even-odd/winding-rule tessellator for this task. The existing OGR polygon topology is authoritative.

Self-intersecting or otherwise invalid polygons must not be silently reinterpreted.

### Coordinate dimensions

The current rendering `Point` representation contains X and Y only.

Keep this implementation consistent with the existing 2D rendering model. Do not introduce an unrelated Z/M rendering redesign.

Document that triangulation produces the 2D rendering mesh.

## 1.5. Integrate triangulation into the Core render scene

The OpenGL renderer must not triangulate polygons on every frame.

The mesh should be prepared once and retained with the backend-neutral render data.

Investigate the cleanest way to integrate this with `VectorRenderScene`.

A preferred approach is to associate a fill mesh with each polygon while preserving the original polygon rings.

The original geometry must remain available because:

- GDI needs the original rings.
- Polygon outlines must follow the original boundaries.
- Hole boundaries must not be replaced by internal triangle edges.

Consider representing triangulation as optional data. If triangulation fails for a particular polygon, retain the original geometry so that GDI can still render it.

The scene must not become unusable simply because GEOS cannot triangulate one polygon.

Ensure that mesh and polygon ordering remain consistent.

For multipart geometries, each component must have its own corresponding mesh.

Do not introduce a cache that depends on raw pointers to temporary scene objects or requires invalidation on every viewport change.

The existing `MapView` scene lifecycle should remain the authority for geometry lifetime and reuse.

## 1.6. Add Core unit tests

Add tests to the existing Core GoogleTest project.

At minimum, test:

**Basic polygons**
- Triangle.
- Convex rectangle.
- Convex polygon with several vertices.

**Concave polygons**
- Simple concave polygon.
- L-shaped polygon.
- More complex concave polygon.

**Interior rings**
- Polygon with one hole.
- Polygon with multiple holes.
- Concave polygon with a hole.

**Multipart geometries**
- Multiple independent polygons.
- Multiple polygons with different vertex counts.
- Multipart geometry where one component has a hole.

**Input validation**
- Empty polygon.
- Degenerate polygon.
- Duplicate consecutive vertices.
- Invalid/self-intersecting polygon.
- Missing or malformed rings.

**Mesh correctness**
- Index count is divisible by three.
- All indices reference valid vertices.
- No degenerate triangles.
- Consistent triangle winding.
- Triangles do not extend outside the original polygon.
- Triangles do not cover interior holes.
- Triangles do not overlap each other in their interiors.
- Sum of triangle areas matches the original polygon area within a suitable numerical tolerance.

Use area and topology checks rather than relying only on the expected number of triangles.

Also verify that multiple calls produce valid meshes and that temporary OGR geometry ownership does not result in leaks.

Add the new files to `MapWinGIS.Core.vcxproj` as required by the existing project structure.

### PR 1 acceptance criteria

- Core provides a reusable polygon triangulation utility.
- The implementation uses OGR constrained Delaunay triangulation backed by GEOS.
- No custom triangulation algorithm is introduced.
- Concave polygons, holes and multipart polygons are supported.
- Original polygon boundaries remain available.
- Meshes are prepared once and reused.
- Triangulation failures do not destroy the original geometry needed by GDI.
- Core public APIs remain independent of graphics and COM.
- All new GoogleTests pass.

---

# PR 2 — OpenGL mesh rendering and removal of convexity restrictions

## 2.1. Investigate the existing OpenGL implementation

Inspect:

- `src/MapWinGIS.ActiveX/OpenGlVectorLayerRenderer.cpp`
- `src/MapWinGIS.ActiveX/OpenGlVectorLayerRenderer.h`
- `src/MapWinGIS.ActiveX/VectorLayerRenderer.cpp`
- `src/MapWinGIS.ActiveX/VectorRenderBackend.h`
- `src/MapWinGIS.ActiveX/GdiVectorLayerRenderer.cpp`
- `tests/MapWinGIS.ActiveX.Tests/VectorLayerRendererTests.cpp`

Understand the existing rendering lifecycle, WGL context management, framebuffer allocation, CPU staging buffer and GDI fallback.

Preserve the existing responsibilities and resource ownership.

## 2.2. Replace GL_POLYGON with triangle mesh rendering

Currently, `DrawLayer()` uses:

```cpp
glBegin(GL_POLYGON);
...
glEnd();
```

Replace polygon filling with indexed triangle rendering.

Use the Core-generated mesh.

Prefer the OpenGL 1.1 client-side vertex-array API already available through the existing Windows OpenGL implementation:

- `glVertexPointer()`
- `glDrawElements()`
- `glEnableClientState()`
- `glDisableClientState()`

Do not introduce a new OpenGL context requirement, shader infrastructure, VBO dependency or modern OpenGL profile as part of this change.

Transform mesh coordinates consistently with the existing `ViewTransform`.

Use the existing shared viewport transformation. Both backends must continue to render the same map coordinates at the same device positions.

Ensure that:

- Vertex and index buffers remain alive for the entire draw operation.
- Index types match the actual mesh representation.
- Buffer sizes are checked before conversion to OpenGL integer types.
- OpenGL client state is restored or disabled appropriately.
- Triangle winding is consistent with the existing rendering configuration.

Keep fill rendering separate from outline rendering.

## 2.3. Preserve original polygon boundaries

Triangle edges are not polygon boundaries.

Do not draw the edges of every triangle as polygon outlines.

Instead, continue to draw outlines using the original rings from `VectorRenderScene`.

This must include:

- Exterior boundaries.
- Interior ring boundaries.
- Every component of multipart geometries.

Use suitable line rendering for each ring.

Preserve the existing fill and outline colors.

## 2.4. Remove the convexity and single-ring restrictions

The current `IsSupported()` implementation rejects polygons when:

```cpp
polygon.size() != 1 || !IsConvex(polygon.front())
```

Remove the convexity restriction and the requirement for exactly one ring.

The OpenGL backend should accept scenes containing:

- Convex polygons.
- Concave polygons.
- Polygons with one or more interior rings.
- Multiple polygons in a feature.
- Multiple polygon components across multiple features.
- Multiple vector layers.

Remove the now-obsolete `IsConvex()` implementation and associated helper functions if no longer needed.

Remove or update rejection messages that refer to convexity or hole-free polygons.

Do not remove unrelated validation.

In particular, retain checks for:

- Non-finite coordinates.
- Invalid viewport dimensions.
- Unsupported target dimensions.
- Framebuffer limitations.
- Staging-buffer memory limits.
- OpenGL initialization failures.
- Drawing and readback failures.

A polygon without a valid triangulation mesh must not be drawn using an incorrect fallback such as `GL_POLYGON`.

Instead, report the scene as unsupported by OpenGL and let the coordinator use GDI.

## 2.5. Preserve the rendering coordinator

Do not redesign the rendering coordinator.

The existing behavior should remain:

1. Try OpenGL when the user preference permits it.
2. If the scene is supported and OpenGL can render it, use OpenGL.
3. If OpenGL cannot render the scene, use GDI.
4. If framebuffer objects are unavailable, use GDI.
5. If OpenGL initialization or drawing fails, use GDI.
6. Report the selected backend and diagnostic through the existing status properties.

The `Automatic`, `OpenGL` and `GDI` preferences must retain their existing semantics.

An explicit OpenGL preference must not disable GDI fallback.

GDI remains the Windows CPU rendering fallback and must continue working independently of GPU availability.

Do not change COM interfaces, DISPIDs or interface inheritance for this feature.

## 2.6. Add ActiveX renderer tests

Extend `VectorLayerRendererTests.cpp`.

Test the following:

**OpenGL rendering**
- Concave polygon.
- L-shaped polygon.
- Polygon with one interior ring.
- Polygon with multiple interior rings.
- Multipart polygon.
- Multiple polygons in one layer.
- Multiple layers containing different polygon types.

**Visual correctness**
- Interior holes remain unfilled.
- Concave indentations remain unfilled.
- Polygon outlines follow original boundaries.
- Internal triangle edges are not visible.
- Layer ordering remains unchanged.
- GDI and OpenGL produce equivalent fill and outline results.

**Fallback**
- Missing triangulation mesh.
- Invalid polygon.
- Missing framebuffer-object support.
- OpenGL initialization failure.
- Framebuffer allocation failure.
- Drawing/readback failure.

Where a real OpenGL context is required, follow the existing testing convention: skip only when the environment genuinely lacks usable OpenGL framebuffer support.

Keep geometry support and fallback decision tests independent of COM activation and, where possible, independent of a physical GPU.

### PR 2 acceptance criteria

- OpenGL fills polygons using triangle meshes.
- No polygon fill uses `GL_POLYGON`.
- Concave polygons render correctly.
- Holes remain transparent.
- Multipart polygons render correctly.
- Original boundaries are used for outlines.
- Convexity and single-ring restrictions are removed.
- The rendering coordinator remains unchanged in its overall design.
- GDI fallback continues to work.
- Existing framebuffer and memory safeguards remain intact.
- No COM interface changes are introduced.

---

# PR 3 — Integration verification, performance and documentation

## 3.1. Verify the complete rendering pipeline

Test the complete flow:

```text
VectorDataset
     |
     v
VectorRenderScene
     |
     v
Polygon triangulation (GDAL/OGR + GEOS)
     |
     v
Backend-neutral triangle meshes
     |
     v
MapRenderScene
     |
     v
Rendering coordinator
     |
     +-------------------+
     |                   |
     v                   v
   OpenGL                GDI
     |                   |
     v                   v
  Triangle meshes     Original rings
     |                   |
     +---------+---------+
               |
               v
          Final output
```

Verify that the existing `MapView` scene lifecycle correctly retains meshes when:

- The viewport is resized.
- The user zooms in or out.
- The user pans.
- The map is redrawn.
- A new layer is added.
- A layer is removed.
- The scene is invalidated and rebuilt.

Changing the viewport must not trigger polygon triangulation again.

## 3.2. Verify GDI compatibility

GDI must continue using the original polygon rings.

Test that:

- Polygons with holes render correctly.
- Concave polygons render correctly.
- Multipart geometries render correctly.
- A polygon whose triangulation fails can still be drawn by GDI if its original geometry is supported.
- OpenGL fallback does not leave partially rendered output or corrupt the target surface.
- Backend switching does not discard map state.

Do not introduce triangulation requirements into the GDI backend.

## 3.3. Performance and memory

Measure the impact of triangulation and mesh storage.

Use representative datasets containing:

- Many small polygons.
- Large polygons with many vertices.
- Concave polygons.
- Polygons with holes.
- Multipart geometries.
- Multiple vector layers.

Check:

- Time required to prepare the rendering scene.
- Time required to triangulate polygons.
- Memory consumption of the original rings and generated meshes.
- OpenGL rendering time.
- CPU readback and GDI presentation time.

Ensure that triangulation is not repeated during ordinary redraws.

Avoid unnecessary copies of mesh buffers.

Do not introduce premature caching, paging or a new rendering architecture. Keep the implementation aligned with the current scene ownership model.

The existing 64 MiB OpenGL CPU staging-buffer limit must remain unchanged unless a separately justified change is necessary.

## 3.4. Update documentation

Update `docs/ARCHITECTURE.md`.

The rendering architecture section should no longer state that OpenGL supports only convex single-ring polygons.

Document the new behavior:

- Core generates backend-neutral triangulated meshes using GDAL/OGR and GEOS.
- The OpenGL backend consumes the generated meshes.
- Original polygon rings remain available for outlines and GDI rendering.
- Concave polygons, holes and multipart geometries are supported.
- GDI remains the fallback for unsupported scenes and unavailable OpenGL capabilities.

Update the current limits and next-work section accordingly.

Check whether `README.md` or other rendering documentation contains outdated descriptions and update them where appropriate.

Do not document implementation details that are not actually verified.

## 3.5. Final verification

Run the required builds and tests from `AGENTS.md`:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m

bin\Debug\x64\MapWinGIS.Core.Tests.exe

bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe
```

Also run the existing rendering baseline where practical.

Check:

- No new Level 4 compiler warnings.
- No memory leaks from OGR geometry ownership.
- No resource leaks from OpenGL client state or rendering buffers.
- No regressions in existing GDI rendering.
- No regressions in the existing ActiveX test application.
- All existing tests continue to pass.

### PR 3 acceptance criteria

- Complete rendering pipeline works with complex polygon geometries.
- Meshes are reused across redraws.
- GDI fallback remains fully functional.
- Performance and memory impact have been evaluated.
- Architecture documentation reflects the implementation.
- Debug/x64 build succeeds without new Level 4 warnings.
- Core and ActiveX tests pass.

---

# General implementation instructions

These instructions apply to all three PRs.

## Architecture and code quality

Follow `AGENTS.md` and the existing project conventions.

In particular:

- Modern C++20.
- Core stays independent of COM, ATL, MFC, Windows graphics APIs and OpenGL.
- Keep the public Core API free of third-party geometry types.
- Use RAII and explicit ownership.
- Keep implementation details private where possible.
- Add tests with every feature.
- Prefer existing infrastructure over new dependencies.
- Do not transliterate MapWinGIS5 code.
- Modernize the architecture first; backward compatibility is secondary.

## Scope restrictions

Do not:

- Implement a custom ear-clipping or sweep-line triangulation algorithm.
- Introduce GLU.
- Add another geometry library.
- Replace GDAL/GEOS.
- Redesign the rendering coordinator.
- Change COM interfaces or DISPIDs.
- Remove GDI fallback.
- Introduce OpenCL.
- Introduce shader-based rendering or a modern OpenGL context requirement.
- Change the existing rendering colors or styling unnecessarily.
- Silently repair invalid geometries.

## Stacked PR organization

Create three stacked PRs:

| PR | Suggested title | Base branch |
|---|---|---|
| 1 | Add GEOS-backed polygon triangulation to Core | `development` |
| 2 | Render triangulated polygons in OpenGL | PR 1 branch |
| 3 | Verify and document complex polygon rendering | PR 2 branch |

Each PR should contain only the changes belonging to its phase and should be independently buildable and testable.

Keep the dependencies between PRs explicit.

Merge them in order, starting with PR 1.

## AI memory

At the beginning of implementation, read:

- `AGENTS.md`
- `.ai/memory/MEMORY.md`
- Relevant architecture, testing and build memory files.

At the end of each PR:

- Review whether reusable project knowledge was discovered.
- Update the appropriate existing memory files if necessary.
- Update `MEMORY.md` and category indexes if new memory files are added.
- Include relevant memory updates in the PR.

Do not record temporary implementation status as permanent project knowledge.

---

# Definition of done

The feature is complete when OpenGL can correctly render ordinary and complex polygon geometries, including concave polygons, holes and multipart geometries, without relying on `GL_POLYGON`.

Triangulation must be provided by the existing GDAL/GEOS infrastructure, implemented in Core, independently testable, and reusable across rendering frames.

The original geometry must remain available for outlines and GDI fallback.

The existing rendering coordinator, framebuffer handling, memory safeguards and GDI fallback architecture must remain intact.