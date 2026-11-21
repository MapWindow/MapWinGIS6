# MapWinGIS6 – Vector Rendering Performance Improvement Plan

## Implementation checklist

Copilot: Update this checklist as each phase is implemented. Only mark a phase complete when its acceptance criteria have been met and its PR is ready.

- [x] **Phase 1 – Establish a performance baseline** (PR 1)
  - Added opt-in, thread-safe timings and counters across Core and ActiveX, plus A-D benchmark configurations and Release results in `docs/TESTING.md`.
  - Measured the supplied 10.3 MB, 2,459-feature shapefile with all A-D configurations; results are recorded in the Phase 1 benchmark section below.
  - Limitation: the measured shapefile is not stored in the repository; `MWG_RENDERING_BASELINE_DATASET` can rerun it when available.
  - Limitation: configuration C records the current OpenGL rejection because outline-only OpenGL rendering is a Phase 2 change.
- [x] **Phase 2 – Separate geometry loading from rendering preparation** (PR 2)
  - Normal scene construction now preserves source rings and bounds without triangulating polygons; fill meshes remain available through explicit preparation.
  - GDI fills directly from source rings, and OpenGL can draw mesh-free polygon outlines while fill requests retain the existing mesh requirement and GDI fallback.
  - Limitation: OpenGL fill meshes are prepared explicitly rather than generated lazily; lazy triangulation and cache reuse remain Phase 3 and Phase 4 work.
- [x] **Phase 3 – Implement lazy polygon triangulation** (PR 3)
  - OpenGL fill draws now generate missing polygon meshes on demand and reuse them for later draws of the same immutable scene, including temporary scaled draws.
  - Explicitly prepared meshes remain preferred; outline and GDI paths continue to use source rings without requiring fill meshes.
  - Triangulation failures, including exceptions from GDAL logging callbacks, return to the coordinator for GDI fallback without preventing outline rendering.
  - Limitation: triangulation remains synchronous, and broader cache ownership, memory limits, and invalidation rules remain Phase 4 work.
- [x] **Phase 4 – Add intelligent caching and invalidation** (PR 4)
  - OpenGL lazy fill meshes are cached per stable layer ID and immutable scene identity, surviving viewport changes, layer reordering, temporary draws, visibility changes, and backend selection.
  - Scene replacement, explicit per-layer invalidation, removed-layer pruning, and clear-all invalidation release only the affected generated mesh storage; retained mesh bytes are tracked for tests.
  - Limitation: no cache-size limit or eviction policy was added because measurements did not establish that the additional complexity is justified.
- [x] **Phase 5 – Introduce viewport-aware geometry processing** (PR 5)
- [ ] **Phase 6 – Optimize OpenGL rendering and data transfer** (PR 6)
- [ ] **Phase 7 – Testing and regression prevention** (PR 7)

**Target branch:** `development`

## Objective

Improve the performance of opening and initially rendering large polygon shapefiles in MapWinGIS6.

A performance comparison with MapWinGIS5 showed that MapWinGIS6 currently takes considerably longer to load and display a polygon shapefile.

Test dataset:

- File size: approximately 10.3 MB
- Features: 2,459 polygons
- MapWinGIS5: nearly instantaneous initial display
- MapWinGIS6: more than 7 seconds before the initial display

Initial experiments established:

- GDI with GEOS triangulation disabled is the fastest configuration.
- Zooming and panning are already reasonably fast once the layer has been rendered.

This suggests that the initial scene-building and triangulation pipeline deserves particular attention. However, do not assume triangulation is the only bottleneck. Instrument the complete pipeline before deciding which optimizations are necessary.

## Architectural principles

The implementation must follow these principles:

1. Opening a dataset must not require polygon triangulation.
2. Geometry loading and rendering preparation must be separate operations.
3. Polygon outlines must be renderable without triangulation.
4. Polygon filling should use triangulation only when the selected rendering backend requires it.
5. Expensive geometry processing should be performed lazily.
6. Generated geometry and rendering resources should be cached and reused.
7. Only geometry needed for the current viewport should be processed where practical.
8. Existing rendering functionality and public ActiveX behavior must remain intact.
9. GEOS must remain an optional implementation detail of the rendering pipeline, not a prerequisite for opening a dataset.
10. Performance improvements must be measured rather than assumed.

Prioritize a clean architecture and maintainability over preserving the existing implementation structure.

---

# Phase 1 – Establish a performance baseline

Before changing the rendering architecture, add instrumentation to identify exactly where time is spent.

### 1.1 Instrument the complete pipeline

Measure the following operations independently:

- Opening the shapefile and its associated files.
- Reading feature metadata.
- Reading feature geometries from GDAL/OGR.
- Converting geometries into MapWinGIS internal representations.
- Building the vector rendering scene.
- GEOS geometry validation.
- GEOS triangulation.
- Uploading geometry buffers to OpenGL.
- Initial rendering.
- Any framebuffer readback operations, including `glReadPixels`.

Use `std::chrono::steady_clock` for C++ timing.

Instrumentation should be easy to enable or disable. Prefer a consistent diagnostic mechanism instead of scattered console output.

### 1.2 Add useful counters

In addition to elapsed time, record:

- Number of features processed.
- Number of geometries loaded.
- Number of polygons triangulated.
- Number of triangles generated.
- Number of invalid geometries.
- Number of triangulation failures.
- Number of features skipped because they are outside the viewport.
- Number of cache hits and misses.

### 1.3 Benchmark configurations

Use the same dataset and machine for all tests.

Compare:

| Test | Backend | Triangulation |
|---|---|---|
| A | GDI | Disabled |
| B | GDI | Enabled |
| C | OpenGL | Disabled |
| D | OpenGL | Enabled |

Record both initial loading time and first-render time separately.

Also measure subsequent zoom and pan operations.

### Deliverable

A performance report identifying the dominant costs and providing a baseline for future comparisons.

### Baseline results for the supplied 10.3 MB shapefile

Release x64, 1,280 x 720 target, 2,459 source features, and 2,470 polygons:

| Configuration | Open | Scene preparation | First frame | Steady frame | Pan | Zoom |
|---|---:|---:|---:|---:|---:|---:|
| A. GDI, triangulation disabled | 2.9 ms | 101.4 ms | 19.2 ms | 18.3 ms | 18.5 ms | 16.6 ms |
| B. GDI, triangulation enabled | 0.4 ms | 1,948.8 ms | 19.4 ms | 17.7 ms | 18.0 ms | 15.5 ms |
| C. OpenGL, triangulation disabled | 2.9 ms | 101.4 ms | Rejected | — | — | — |
| D. OpenGL, triangulation enabled | 0.4 ms | 1,948.8 ms | 162.5 ms | 15.2 ms | 14.7 ms | 15.3 ms |

Configuration C is rejected because the current OpenGL backend requires a
triangle mesh for every polygon. This is expected to be addressed in Phase 2.

Pipeline detail for triangulation-enabled scene construction:

| Operation | Time |
|---|---:|
| OGR feature and geometry reads | 84.3 ms |
| Internal geometry conversion | 7.3 ms |
| GEOS geometry validation | 56.3 ms |
| Polygon triangulation | 1,789.6 ms |

The run generated 586,414 triangles. Geometry submission took 102.1 ms
across eight OpenGL frames, and framebuffer readback took 3.8 ms across the
same frames. Triangulation accounted for about 92% of scene preparation.
The estimated open-to-first-display times were:

- GDI without triangulation: 123.5 ms.
- GDI with triangulation: 1,968.6 ms.
- OpenGL with triangulation: 2,111.7 ms.

Do not proceed by simply assuming that GEOS triangulation accounts for the entire delay.

---

# Phase 2 – Separate geometry loading from rendering preparation

Inspect the current implementation, particularly:

- `VectorRenderScene.cpp`
- `PolygonTriangulation.cpp`
- `VectorDataset.cpp`
- `MapView.cpp`
- `OpenGlVectorLayerRenderer.cpp`
- `VectorLayerRenderer.cpp`

### 2.1 Review the current scene-building process

Currently, the scene-building pipeline iterates through geometries and builds rendering data. Polygon triangulation is performed as part of this process.

Change this architecture so that building a vector scene does not automatically triangulate every polygon.

### 2.2 Introduce separate geometry representations

Keep the original geometry data available independently of any triangulated representation.

Conceptually, distinguish between:

- Source geometry: original points, rings, and geometry structure.
- Rendering metadata: bounding boxes, feature identifiers, and other information needed for rendering.
- Polygon fill meshes: triangulated geometry used by fill rendering.

Do not duplicate large amounts of geometry unnecessarily.

### 2.3 Preserve outline rendering

Polygon outlines must be drawable directly from the original geometry representation.

GDI rendering must not depend on triangulation.

OpenGL should also be able to render polygon outlines without requiring fill meshes.

### 2.4 Remove mandatory triangulation

Remove the unconditional triangulation step from the normal scene-building path.

Building a scene should prepare only the data required for the initial rendering operation.

### Acceptance criteria

- Opening a polygon shapefile does not invoke GEOS triangulation.
- Polygon outlines can be rendered without triangulation.
- GDI rendering works with triangulation disabled.
- Existing polygon rendering behavior is preserved when fill rendering is requested.

---

# Phase 3 – Implement lazy polygon triangulation

Introduce triangulation as an on-demand operation.

### 3.1 Define when triangulation is necessary

Triangulation should only be requested when:

- Polygon fill rendering is enabled.
- The selected rendering backend needs triangle meshes.
- A particular polygon does not already have a valid cached mesh.

Do not triangulate polygons merely because they are part of a newly opened dataset.

### 3.2 Implement lazy generation

When an OpenGL fill operation needs a polygon mesh:

1. Check whether a valid mesh already exists.
2. If it exists, reuse it.
3. Otherwise, generate the mesh using the existing GEOS triangulation implementation.
4. Store the generated mesh in the appropriate cache.
5. Continue rendering.

Avoid repeated triangulation during zooming, panning, resizing, or repainting.

### 3.3 Handle failures gracefully

Triangulation must not prevent the dataset from being opened or its outlines from being displayed.

Handle:

- Invalid geometries.
- Empty geometries.
- GEOS failures.
- Polygons containing holes.
- MultiPolygon geometries.
- Very large polygons.
- Degenerate rings.

A failed triangulation should not crash the application or invalidate unrelated features.

Do not silently change the source geometry to make triangulation succeed.

### 3.4 Consider background processing

After the basic lazy implementation works, investigate whether triangulation can be performed asynchronously.

Requirements:

- Never block the UI thread for expensive triangulation.
- Avoid concurrent access to unsafe GDAL/OGR or GEOS objects.
- Ensure cancellation when a layer is removed or its data changes.
- Ensure generated results are associated with the correct dataset and feature.
- Ensure rendering can safely handle meshes that are not yet available.

Do not introduce asynchronous processing until the synchronous lazy implementation is correct and measured.

### Acceptance criteria

- Opening a polygon shapefile does not trigger triangulation.
- GDI can render polygons without fill meshes.
- OpenGL can render outlines without fill meshes.
- Polygon fills trigger triangulation only when needed.
- Repeated rendering reuses existing meshes.
- Triangulation failures do not prevent outline rendering.

---

# Phase 4 – Add intelligent caching and invalidation

The existing scene cache in `MapView` is a useful starting point. Extend the architecture without introducing unnecessary duplicate caches.

### 4.1 Cache triangulation results

Cache generated fill meshes per feature or another suitable stable geometry identifier.

The cache should support:

- Reuse across repaint operations.
- Reuse across zoom and pan operations.
- Efficient lookup.
- Memory-aware storage.
- Explicit invalidation.

### 4.2 Define cache invalidation rules

Invalidate affected cached data when:

- The underlying dataset changes.
- A layer is removed.
- A layer is reloaded.
- Geometry is edited.
- The coordinate system or transformation changes in a way that invalidates cached coordinates.
- Rendering configuration changes in a way that makes cached data unusable.

Changing only the viewport should not invalidate triangulation meshes.

Changing between GDI and OpenGL should not discard valid geometry or triangulation data unnecessarily.

### 4.3 Manage memory

Large polygon datasets can generate substantial mesh data.

Consider:

- Tracking the memory used by cached meshes.
- Avoiding duplicate copies of vertices.
- Releasing resources when layers are removed.
- Releasing GPU resources when appropriate.
- An optional cache-size limit or eviction policy if measurements justify it.

Do not implement a complicated cache eviction system unless actual memory measurements show that it is needed.

### Acceptance criteria

- Repeated zooming and panning do not trigger repeated triangulation.
- Switching rendering backends does not cause unnecessary geometry reconstruction.
- Removing a layer releases its associated resources.
- Cache invalidation is correct and testable.

---

# Phase 5 – Introduce viewport-aware geometry processing

Currently, the scene-building process may process every feature, even when most features are outside the visible map area.

Investigate how to reduce this work.

### 5.1 Use feature bounding boxes

Use feature extents to determine whether a feature intersects the current viewport.

Avoid loading, converting, triangulating, or uploading geometry that cannot contribute to the current rendering operation.

Use a small configurable or calculated margin around the viewport to avoid excessive work during small pans.

### 5.2 Investigate spatial indexing

Check whether GDAL/OGR spatial filtering can be used effectively.

Also investigate whether MapWinGIS6 already has a suitable spatial index or whether an existing one can be reused.

Do not introduce a new spatial indexing implementation without first checking the existing architecture.

### 5.3 Avoid unnecessary full-dataset geometry copying

MapWinGIS5 uses a more demand-driven approach to reading shapefile geometries. Investigate whether a similar strategy is appropriate for MapWinGIS6.

Potential improvements:

- Load feature metadata and extents first.
- Retrieve full geometry only when required.
- Retain already loaded geometry in a cache.
- Avoid copying every feature into multiple internal representations.

Do not compromise the existing GDAL abstraction or create unnecessary coupling to the shapefile file format.

### 5.4 Handle viewport changes efficiently

When the user pans or zooms:

- Determine which features have become visible.
- Reuse already loaded geometries.
- Load missing geometries only when needed.
- Reuse cached triangulation meshes.
- Avoid rebuilding the entire layer scene if only a small part of the viewport changes.

### Acceptance criteria

- Large datasets do not require processing every feature before the first visible rendering.
- Features outside the viewport do not undergo unnecessary triangulation.
- Small pans do not cause complete layer reconstruction.
- No visible features are accidentally omitted at viewport boundaries.

### Phase 5 implementation and limitations

- `MapView` requests features through an OGR bounding-box spatial filter
  expanded by a 64-pixel margin around the visible extent.
- Filtered scenes retain source geometry and backend cache identity while the
  visible extent remains inside the processed extent.
- Pans or zoom-outs beyond the margin rebuild the affected layer selection.
  This phase does not yet provide incremental feature-level merging or tiling.
- Caller-prepared scenes remain complete snapshots and are not viewport
  filtered.
- The skipped-feature counter is available only when OGR can return a
  non-forced layer feature count; it never forces a complete scan for the
  diagnostic value.

---

# Phase 6 – Optimize OpenGL rendering and data transfer

After the preceding phases, investigate remaining OpenGL overhead.

### 6.1 Inspect buffer management

Ensure that vertex and index buffers are reused rather than recreated on every frame.

Separate geometry preparation from GPU buffer creation.

### 6.2 Investigate framebuffer readback

Inspect `OpenGlVectorLayerRenderer.cpp`, especially `glReadPixels`.

Framebuffer readback can force synchronization between the GPU and CPU.

Determine:

- Why the framebuffer is read back.
- Whether the readback is required for the current ActiveX architecture.
- Whether it can be avoided or reduced.
- Whether the current implementation copies more data than necessary.

Do not remove functionality that depends on the readback without providing an alternative.

### 6.3 Avoid unnecessary rendering work

Investigate:

- Drawing only visible features.
- Avoiding unnecessary state changes.
- Reducing redundant buffer uploads.
- Reusing GPU resources.
- Avoiding repeated coordinate transformations.

### Acceptance criteria

- OpenGL initial rendering does not perform unnecessary CPU/GPU synchronization.
- GPU buffers are reused appropriately.
- Rendering remains correct across repeated zoom and pan operations.

---

# Phase 7 – Testing and regression prevention

Performance improvements must not introduce rendering regressions.

### 7.1 Test datasets

Use a representative set of datasets:

- Small polygon shapefile.
- Medium polygon shapefile.
- Large polygon shapefile.
- Polygon shapefile with holes.
- Multipolygon shapefile.
- Invalid or degenerate polygon geometries.
- Large polygon features.
- Point and line datasets.

Include the original 10.3 MB dataset with 2,459 features as a repeatable benchmark.

### 7.2 Test rendering backends

Test:

- GDI with polygon outlines.
- GDI with polygon fills.
- OpenGL with polygon outlines.
- OpenGL with polygon fills.
- Backend switching.
- Repeated zooming and panning.
- Layer removal and reload.

### 7.3 Test correctness

Compare rendered output before and after the changes.

Pay particular attention to:

- Polygon holes.
- Multipolygons.
- Polygon boundaries.
- Feature visibility.
- Clipping at viewport boundaries.
- Invalid geometries.
- Coordinate transformations.

### 7.4 Add automated tests

Add unit tests for:

- Lazy triangulation.
- Cache hits and misses.
- Cache invalidation.
- Failed triangulation.
- Empty geometries.
- Layer removal and resource cleanup.

Add integration tests where practical.

### 7.5 Performance targets

Use the original dataset as the primary benchmark.

Targets:

- Opening a dataset must not trigger GEOS triangulation.
- Initial GDI outline rendering should be close to the cost of loading and drawing the visible geometry.
- Initial OpenGL outline rendering should not require polygon fill meshes.
- Repeated rendering must not repeat expensive geometry preparation.
- Zoom and pan performance must remain at least as good as the current implementation.

Do not introduce arbitrary timing thresholds into CI unless the test environment is sufficiently stable. Record benchmark results and compare them against the established baseline.

---

# Implementation strategy

Implement this work in separate, reviewable pull requests.

Suggested sequence:

| PR | Scope |
|---|---|
| PR 1 | Performance instrumentation and reproducible benchmark |
| PR 2 | Separate geometry preparation from triangulation |
| PR 3 | Lazy triangulation for OpenGL polygon fills |
| PR 4 | Triangulation cache and invalidation |
| PR 5 | Viewport-aware geometry processing |
| PR 6 | OpenGL buffer and framebuffer readback optimization |
| PR 7 | Additional automated tests and performance regression coverage |

Each PR must:

1. Build successfully using the existing CMake build configuration.
2. Include appropriate tests.
3. Document any architectural decisions.
4. Include before/after performance measurements where applicable.
5. Avoid unrelated refactoring.
6. Be independently reviewable.
7. Include a clear description of limitations and remaining work.
8. The PR title should reflect the title of the plan combined with the title of the phase.

Do not wait for all phases to be completed before creating PRs. Each phase should result in a separate PR against the `development` branch, following the repository's existing contribution workflow.

## Instructions to Copilot

Before implementing anything:

1. Inspect the current `development` branch.
2. Read `AGENTS.md` and all applicable repository instructions.
3. Review the existing rendering architecture and recent changes.
4. Verify that the files and classes mentioned in this plan still match the current implementation.
5. Identify existing abstractions that can be reused.
6. Present a short implementation proposal for the first PR.

Do not assume the architecture described in this plan is still exactly accurate. Verify it against the current source code.

Do not implement all phases in one large change.

Preserve the existing ActiveX public API unless a change is demonstrably necessary. Prefer internal architectural improvements.

The primary goal is to make MapWinGIS6 fast when opening and initially displaying large vector datasets, without sacrificing the advantages of the new GDAL, GEOS, and OpenGL architecture.
