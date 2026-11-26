# MapWinGIS6 – Vector Rendering First-Frame Performance Plan

## Implementation checklist

Copilot: Update this checklist as each phase is implemented. Only mark a phase
complete when its acceptance criteria have been met and its PR is ready.

- [x] **Phase 1 – End-to-end benchmark + keep triangulation off the paint path** (PR 1)
- [x] **Phase 2 – Fast triangulation + background mesh preparation** (PR 2)
- [x] **Phase 3 – Remaining costs + documentation** (PR 3)

**Target branch:** `development`

**Predecessor:** [Vector-Rendering-Performance-Improvement.md](Vector-Rendering-Performance-Improvement.md)

## Context

The previous plan removed triangulation from dataset opening and scene
construction. It did not remove triangulation from the user-visible load path.
Opening
`F:\dev\GIS-data\Siebe\20200116 Clip\afwateringseenheden_2019_copy_join_pgb_20190704.shp`
(10.3 MB, 2,459 features / 2,470 polygons, 586,414 triangles) in
`ActiveXTestHost.exe` still takes 7+ seconds before the polygons appear.
MapWinGIS5 shows the same file almost instantly.

This plan consolidates the follow-up analysis into **3 PRs** (GitHub CI checks
take over an hour per PR, so the work is grouped accordingly).

The test dataset is not committed. Use `MWG_RENDERING_BASELINE_DATASET` to
point the benchmarks at it.

## Benchmark results (2026-11-25, real shapefile, 1280x720, one workstation)

`RenderingBaseline.*` against the real shapefile, plus the Debug comparison.
Results are from single runs, so use them as an indication.

| Measurement | Release | Debug |
|---|---:|---:|
| Source-only scene (OGR read + conversion) | 89–186 ms | 543 ms |
| GEOS `IsValid` precheck (2,470 polygons) | 51–56 ms | 400 ms |
| GEOS constrained Delaunay triangulation | 1,730–1,799 ms | ~11,700 ms |
| GDI fill first frame (direct from source rings, no meshes) | 18–31 ms | 62–96 ms |
| OpenGL outline first frame (incl. context creation) | 268–276 ms | 314 ms |
| **OpenGL lazy-fill first frame (= time to first pixel)** | **~1,900 ms** | **~12,400 ms** |
| OpenGL steady / pan / zoom | 10–15 ms | 44–45 ms |
| `glReadPixels` (8 frames total) | 36–66 ms | 64 ms |

## Root causes (verified against the code)

1. **First paint blocks on full-layer triangulation.** The renderer defaults
   to `Automatic` backend + `Fill` mode
   (`src/MapWinGIS.ActiveX/VectorLayerRenderer.h:87`), so the first `OnDraw`
   uses OpenGL, which synchronously triangulates all 2,470 polygons on the UI
   thread (`OpenGlVectorLayerRenderer.cpp` `PrepareFillMeshes`,
   all-or-nothing per layer) before any pixel is presented. Triangulation is
   ~94% of time-to-first-pixel. The lazy-triangulation phases moved the cost
   from load to first paint; they did not remove it.
2. **The 7+ s was measured on Debug binaries** (debug GDAL/GEOS from vcpkg
   make triangulation ~6.5x slower: ~12.4 s first frame). Release is ~2.0 s
   end-to-end — still far from MapWinGIS5.
3. **GDI fill needs no triangulation** — it fills source rings in ~20 ms
   (Release), which is exactly why MapWinGIS5 feels instant. The default
   backend order never uses it for the first frame.
4. **Secondary costs:** OGR reads 81 ms; OpenGL context/FBO creation
   ~50–250 ms on first draw; per-frame O(total-vertices) validity scans
   (`OpenGlVectorLayerRenderer.cpp:287`, `GdiVectorLayerRenderer.cpp:128-144`);
   GPU buffer build/upload uninstrumented
   (`OpenGlVectorLayerRenderer.cpp:819-951`); the `FrameRender` scope
   double-counts triangulation.
5. **Measurement gap:** no benchmark drives the real
   `LoadVectorDatasetEx` → first presented `OnDraw` path through
   `CMapWinGISControl`; the existing baselines test renderers in isolation.
   Viewport filtering (previous plan, Phase 5) does not help here: the control
   seeds a complete scene (`MapWinGISControl.cpp:104-137` →
   `MapView.cpp:213-216`) and at full extent every feature is visible anyway.

## Target (Release, test dataset, default settings `Automatic` + `Fill`)

- `LoadVectorDatasetEx` → first presented filled frame: **≤ 250 ms**.
- No frame blocks the UI thread more than **100 ms** for mesh generation.
- Debug stays usable: **≤ 1.5 s** to first frame.
- No regression in steady pan/zoom, visual output, or the existing test
  suites.

---

# Phase 1 – End-to-end benchmark + keep triangulation off the paint path (PR 1)

Measurement and the primary fix land together; the new benchmark proves the
fix.

### 1.1 End-to-end benchmark

Add an opt-in `RenderingBaseline` test that hosts `CMapWinGISControl`, calls
`LoadVectorDatasetEx` (and the `BeginLoadVectorDataset`/`EndLoadVectorDataset`
path), forces the first `OnDraw` into a memory DC with default settings, and
reports load time, first-presented-frame time, and the diagnostic breakdown.
Fix the `FrameRender`/`PolygonTriangulation` scope overlap and instrument the
GPU buffer build/upload so the breakdown is accurate.

### 1.2 GDI-first first paint

In `Automatic` mode, when a layer has no cached fill meshes, draw the frame
with the existing GDI direct-ring fill path (~20 ms) instead of triangulating
during the draw; switch that layer to OpenGL on a later frame once meshes
exist. Do not mix backends within one frame. An explicit `OpenGL` preference
keeps the current lazy behavior (documented) until Phase 2 provides background
preparation.

### 1.3 Report the active backend

`RenderingBackend` and the rendering status must state that GDI is in use
while the meshes are pending.

### 1.4 Test-host timings

Show the load time and the first-paint time in the `ActiveXTestHost` status
bar for debugger-free comparison with MapWinGIS5.

### 1.5 Structural regression assertion

No timing thresholds: with default settings the first frame performs **0
triangulations**. Visual parity between GDI ALTERNATE fill and OpenGL meshes
for holed polygons and multipolygons stays within the existing pixel-test
tolerances.

### 1.6 Record the baseline

Record the Release and Debug results for the test dataset in
`docs/TESTING.md`. State clearly that comparisons with MapWinGIS5 must use
Release builds.

### Acceptance criteria

- One command reproduces the user-visible delay and shows how it divides into
  load, scene, triangulation, and draw.
- With the default settings, the first frame does not call
  `TriangulatePolygon`.
- Release first frame ≤ 250 ms on the test dataset.
- The full CTest suites pass.

### Status after Phase 1

Measured on the test dataset (2026-11-25, one workstation, recorded in
`docs/TESTING.md`): `LoadVectorDatasetEx` → first presented frame is
213 ms Release / 1,164 ms Debug; the async path is 198 ms Release /
1,162 ms Debug. Every first frame reported GDI and performed zero
triangulations. Debug and Release CTest suites (484 tests) pass.

Remaining limitations:

- With `Automatic`, fill rendering stays on GDI until fill meshes exist;
  nothing prepares them in the background yet. Phase 2 adds the worker and
  the view invalidation that switches the layer to OpenGL on a later frame.
- The OGR scene read (~165 ms Release for 2,459 features) now dominates the
  load path; Phase 3 evaluates batch reading and the remaining costs.

---

# Phase 2 – Fast triangulation + background mesh preparation (PR 2)

Even with GDI-first paint, OpenGL fill should arrive quickly and never block
the UI thread.

### 2.1 Replace GEOS CDT for fill meshes with earcut

Evaluate and adopt [mapbox/earcut.hpp](https://github.com/mapbox/earcut.hpp)
(ISC license, vcpkg port `earcut-hpp`, header-only). It handles holes, works
directly on the scene's ring buffers (no OGR/GEOS object creation), and is
typically one to two orders of magnitude faster than constrained Delaunay
triangulation. Target ≤ 100 ms for all 2,470 polygons in Release.

Before adopting, confirm:

- The license is compatible with MPL-2.0 and the vcpkg port builds with the
  overlay triplets.
- Output parity with GDI fill for holes, multipolygons, touching rings, and
  self-intersecting (invalid) polygons.

`TriangulatePolygon` stays the single entry point; earcut — like GEOS —
remains an internal implementation detail and never appears in public headers.
Keep GEOS as the second-level fallback for polygons earcut cannot mesh (mark
failed → existing GDI fallback otherwise). Do not modify source geometry to
make triangulation succeed.

### 2.2 Remove the `IsValid` precheck from the fill path

Earcut does not need valid OGC geometry. Removing the precheck saves 55 ms
(Release) / 400 ms (Debug) per layer.

### 2.3 Background preparation

After a layer is added, generate meshes on a worker with bounded parallelism
(earcut has no shared state; the GEOS fallback uses per-thread contexts). The
worker reads only the immutable, owned `VectorRenderScene` — never OGR/GDAL
objects. Key results by stable layer ID and scene identity (the existing cache
key). Cancel or discard the work when the layer is removed, replaced, or
invalidated. Post a message to the control window to invalidate the view when
the meshes are ready, so the next frame switches from GDI to OpenGL.

### 2.4 Optional investigation (only if earcut falls short)

Stencil-then-cover OpenGL fill: draw ring fans into the stencil buffer with
`GL_INVERT` (even-odd rule, matches GDI ALTERNATE), then cover with the
bounding box. Needs no triangulation and reuses the outline buffers. Adopt
only if clearly better than earcut plus background preparation.

### Acceptance criteria

- Lazy fill triangulation of the test dataset takes ≤ 100 ms in Release.
- No UI-thread stall over 100 ms for mesh generation.
- Removing or reloading a layer during preparation does not crash, leak
  memory, or publish stale meshes.
- Tests cover the pending, ready, cancelled, and failed states.
- Existing triangulation, holed, concave, and degenerate tests pass; pixel
  comparisons with GDI fill stay within the current tolerances.

### Status after Phase 2

Earcut is the internal fast path behind `TriangulatePolygon`; GEOS constrained
Delaunay triangulation remains the fallback. The ActiveX control keeps Automatic
Fill on GDI while immutable scenes are prepared on a bounded background path,
then publishes completed meshes on the owner thread and invalidates the frame.
Stable layer IDs, scene identities, and generation tokens discard stale work
after replacement, clearing, or invalidation.

Measured on the test dataset (2026-11-25, Release, one workstation):
2,470 polygons completed in a 93.8 ms lazy OpenGL fill frame
(178.9 ms process-wide triangulation CPU time across four workers). The first
GDI frame was 20.3 ms after a 102.5 ms synchronous load; the asynchronous path
was 21.2 ms after a 118.9 ms load. The full Debug non-interactive suite and
the targeted Release benchmarks pass.

Remaining limitations:

- The control's normal Automatic path is background-prepared. A direct
  explicit-OpenGL draw of a mesh-free scene still retains the synchronous
  fallback contract when no background preparation was queued.
- GEOS remains necessary for polygons earcut rejects. The failed path still
  stays on GDI, including ambiguous self-intersecting geometry.
- Stencil-then-cover was not needed to meet the measured target and was not
  adopted.

---

# Phase 3 – Remaining costs + documentation (PR 3)

Do this work only where the Phase 1 measurements justify it.

- **OpenGL context warm-up:** create the context and FBO when the control is
  created or idle, not during the first draw (~50–250 ms).
- **Async load in the test host:** use `BeginLoadVectorDataset` (scene build
  is 89 ms Release / 543 ms Debug on the UI thread), or document why not.
- **Display-resolution vertex reduction:** at the full extent, ~590 k vertices
  go into 1280 x 720 pixels. Reduce consecutive vertices that fall in the same
  pixel for GDI and outline drawing (MapWinGIS 5 uses a similar method). Never
  modify cached source geometry.
- **OGR read throughput:** measure whether `OGRLayer::GetArrowStream` or batch
  reading beats per-feature `GetNextFeature` (81 ms). Keep the GDAL
  abstraction.
- **Docs and memory:** update `docs/TESTING.md`, `docs/ARCHITECTURE.md`, and
  the test host README. Record verified lessons in
  `.ai/memory/lessons/lessons.md` (for example: "lazy is not enough when the
  paint path is the first consumer" and "measure Debug and Release
  separately"). Keep benchmarks opt-in and threshold-free; regressions are
  caught by structural assertions.

### Status after Phase 3

Completed the context/FBO warm-up when background meshes are published, before
the Automatic backend-switch repaint. Publication and warm-up wait until the
first frame of a new layer set has been presented, because the posted
completion can otherwise run before the initial `WM_PAINT`. Draw does not
publish owner-dispatched meshes. The interactive C# host Open and
drag-and-drop paths now use `BeginLoadVectorDataset` and completion events, so
scene construction does not block the host UI.

On 2026-11-26, the real 2,459-feature dataset completed the Release
`LoadVectorDatasetEx` path in 93.4 ms plus a 21.6 ms first GDI frame. The
asynchronous path completed in 194.4 ms plus a 20.4 ms first frame. Both
stay below the 250 ms target and the first frames reported GDI. These are
diagnostic single runs, not CI thresholds. The complete Debug CTest preset
passed 507 tests, the automated ActiveX host checks passed, and the Debug and
Release CMake builds reported no new code-level Level 4 warnings. The existing
MSVC `D9025` command-line override notices remain in the build configuration.

Remaining limitations:

- Display-resolution vertex reduction was not adopted. The measured Release
  first frame and steady pan/zoom already meet the target, and viewport-specific
  reduction would complicate or invalidate the reusable immutable-scene and
  GPU-buffer caches.
- OGR Arrow/batch reads were not adopted. The sequential Release read remains
  within the overall target and retains existing error-detection and driver
  abstractions; a future change needs an equivalent benchmark that proves a
  material improvement.
- OpenGL target warm-up moves context/FBO allocation before the backend-switch
  repaint, but the owner-thread warm-up still depends on the driver's cost and
  is not a timing threshold. Systems without usable OpenGL continue to use GDI.

---

# Key files

- `src/MapWinGIS.ActiveX/MapWinGISControl.cpp` (`PrepareVectorDataset` :104,
  `OnDraw` :1894, `DrawLayersAndUpdateCache` :1831)
- `src/MapWinGIS.ActiveX/VectorLayerRenderer.h` (backend/mode defaults :87)
- `src/MapWinGIS.ActiveX/OpenGlVectorLayerRenderer.cpp` (`PrepareFillMeshes`
  :1242, `DrawLayerWithGpuBuffers` :781, readback :1673)
- `src/MapWinGIS.ActiveX/GdiVectorLayerRenderer.cpp` (direct-ring fill :128)
- `src/MapWinGIS.Core/src/rendering/PolygonTriangulation.cpp` (GEOS CDT :261)
- `src/MapWinGIS.Core/src/map/MapView.cpp` (complete-scene seeding :213)
- `tests/MapWinGIS.ActiveX.Tests/RenderingBaselineTests.cpp` (benchmark
  harness)
- `samples/CSharp/ActiveXTestHost/` (status-bar timings, async load)

# Verification

- The new end-to-end benchmark on the real shapefile
  (`MWG_RENDERING_BASELINE_DATASET`), Release and Debug: first presented
  filled frame ≤ 250 ms Release / ≤ 1.5 s Debug; the first default frame
  performs 0 triangulations.
- `ctest --preset x64-debug` (plus the Release build) green, including
  lazy-fill lifecycle, GDI/OpenGL pixel parity, backend switching, and
  cancellation tests.
- Manual: Release `ActiveXTestHost.exe` shows the shapefile essentially
  immediately; pan, zoom, resize, layer removal/reload, and backend switching
  behave as before; the status bar shows the active backend and timings.
- `docs/TESTING.md` records the new baseline numbers.

# Assumptions

- Performance targets apply to Release; Debug only needs to stay usable.
  Phase 1 verifies the Debug/Release split end-to-end before optimization
  lands.
- GDI ALTERNATE fill and OpenGL mesh fill are visually equivalent for this
  dataset (covered by existing pixel tests; confirmed again in Phase 1).
- No public ActiveX API changes; earcut is an internal dependency only.
- Three PRs is the right granularity given over an hour of CI per PR; each PR
  is still independently reviewable and leaves the tree green.
