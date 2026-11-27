# MapWinGIS6 – Vector Layer Projection Plan

## Implementation checklist

Copilot: Update this checklist as each phase is implemented. Only mark a phase
complete when its acceptance criteria have been met and its PR is ready.

- [x] **Phase 1 – Core `Projection` model + layer/map projection plumbing** (PR 1)
- [ ] **Phase 2 – On-the-fly reprojection of map layers in Core** (PR 2)
- [ ] **Phase 3 – ActiveX projection interface + test-host prompt and EPSG status bar** (PR 3)

**Target branch:** `development`

## Context

MapWinGIS6 can open polygon vector data but knows nothing about coordinate
reference systems (CRS). The map assumes every layer uses compatible
coordinates (`src/MapWinGIS.Core/include/mapwingis/map/Map.h:11-13`), so layers
in different projections do not align and nothing reports what projection a
layer or the map is in.

This plan adds a modern `projection` capability:

- A Core `mwg::Projection` value type at least as capable as MapWinGIS 5's
  `GeoProjection`, with first-class EPSG support (most geospatial data is
  identified by EPSG code, not by full WKT).
- A `projection` property on vector layers (`VectorLayer::Projection()`),
  later reusable for rasters (GDAL exposes the same `OGRSpatialReference`
  model through `GDALDataset::GetSpatialRef()`).
- A map projection: a map without a projection adopts the projection of the
  first loaded layer that has one.
- A mismatch workflow: when a layer's projection differs from the map's, the
  host is offered three choices — reproject the layer to the map projection,
  load it as is, or do not load it.
- The EPSG code of the map shown in the `ActiveXTestHost` status bar.

The work is grouped into **3 PRs** (GitHub CI checks take over an hour per
PR).

### MapWinGIS 5 reference (what to adopt, what to drop)

`src/COM classes/GeoProjection.h/.cpp` in
[MapWindow/MapWinGIS](https://github.com/MapWindow/MapWinGIS) wraps
`OGRSpatialReference` and offers: import from EPSG / WKT / PROJ4 / ESRI,
export to WKT / PROJ4 / ESRI, `IsGeographic` / `IsProjected` / `IsEmpty`,
`IsSame` / `IsSameGeogCS` / `IsSameExt`, name accessors
(`Name`, `ProjectionName`, `GeogCSName`), `.prj` file read/write, an in-place
coordinate `Transform`, and `TryAutoDetectEpsg`.

MapWindow 5's
[ProjectionMismatchService](https://github.com/MapWindow/MapWindow5/blob/master/src/MW5.Projections/Services/ProjectionMismatchService.cs)
drives the host-side flow: compare layer projection with map projection
(`IsSameExt` over the layer envelope, plus EPSG dialect handling); on
mismatch ask the user to reproject, load as is, or skip.

Adopted: the EPSG autodetect strategy (`morphFromESRI` → `AutoIdentifyEPSG`
→ `FindMatches` with confidence ≥ 80), `IsSame` with the name/
`ProjectionName` normalization fallback, and the three-way mismatch choice.
Dropped: the COM class itself, `Key`/`GlobalCallback` plumbing, the
read/write `.prj` methods (OGR already reads `.prj` with the dataset; writing
belongs to dataset editing, not to a CRS value type), and the frozen/
transform-state members — MapWinGIS6 has no backward-compatibility
obligation, and transformations become stateless functions (see Phase 2).

### Why EPSG-first matching

Two CRS objects that both resolve to an EPSG code compare equal by code. Only
when one side has no code fall back to `OGRSpatialReference::IsSame` and the
MapWinGIS 5 name-normalization comparison. This is simple, fast, and matches
how the data is actually identified.

### Test data

- Manual verification uses
  `F:\dev\GIS-data\Siebe\20200116 Clip\embankments_buffered_split.shp`.
  Its `.prj` is WKT1 **ESRI** (`Amersfoort_RD_New`) without EPSG authority
  codes — exactly the case that needs the autodetect path; the expected EPSG
  code is **28992**.
- The dataset is not committed. Committed tests use generated fixtures:
  `TestData.h` already writes a shapefile in EPSG:28992
  (`MakeSampleShapefile`, `Provinces::kEpsg`), which covers the projected-CRS
  cases. Tests that need the real file read its path from
  `MWG_PROJECTION_TEST_DATASET` (opt-in, same pattern as
  `MWG_RENDERING_BASELINE_DATASET`).
- PROJ is already available: the vcpkg GDAL build pulls in PROJ and
  `GdalEnvironment` deploys `gdal-data` and `proj` runtime folders (the test
  host already requires both). No new dependency.

---

# Phase 1 – Core `Projection` model + layer/map projection plumbing (PR 1)

The foundation: a CRS value type, a `projection` property on vector layers,
and map-projection adoption. No UI, no reprojection yet.

### 1.1 `mwg::Projection` value type

New Core area `projection/`
(`include/mapwingis/projection/Projection.h`,
`src/projection/Projection.cpp`). Immutable, cheaply copyable value type
(shared internal `OGRSpatialReference`); an empty `Projection` means "no
CRS known" and is not an error (Core convention: `std::optional`/`TryX` for
absence, `CoreError` for operational failure).

- Factories: `FromEpsg(int)`, `FromWkt(std::string_view)`,
  `FromProj4(std::string_view)`. Invalid input to the throwing factories
  raises `InvalidArgument`; `TryFromWkt`/`TryFromProj4` probes return
  `std::optional`.
- Accessors: `Epsg()` (`std::optional<int>` — autodetect per the MapWinGIS 5
  strategy: `morphFromESRI` → `AutoIdentifyEPSG` → `FindMatches` confidence
  ≥ 80; cache the result, the object is immutable), `Name()`,
  `IsGeographic()`, `IsProjected()`, `IsEmpty()`, `ExportToWkt()`,
  `ExportToProj4()`.
- `IsSame(const Projection&)`: equal EPSG codes → same; both unknown →
  `OGRSpatialReference::IsSame`; then the MapWinGIS 5
  name/`ProjectionName` normalization fallback.
- `TransformTo(const Projection& target)`: returns a stateless
  `ProjectionTransform` callable (wraps `OGRCoordinateTransformation`). Set
  `OAMS_TRADITIONAL_GIS_ORDER` on both SRS at construction (GDAL 3 axis
  order), matching MapWinGIS 5. No start/stop transform protocol.

### 1.2 Layer projection

`VectorLayer::Projection()` returns the layer's `Projection`, built from
`OGRLayer::GetSpatialRef()` (the shapefile driver resolves the sibling
`.prj`; GeoPackage/GeoJSON carry their own CRS). Replace
`VectorLayer::ProjectionWkt()` with the new property — WKT remains available
through `Projection::ExportToWkt()`; MapWinGIS6 keeps no legacy property.

### 1.3 Map projection

`Map` gains projection state (Core, per the Core/ActiveX boundary decision —
this is reusable GIS behavior):

- `Projection()` → `const std::optional<Projection>&`; `SetProjection(...)`,
  `ClearProjection()`.
- `AddLayer` adopts the layer's projection when the map projection is unset
  and the layer has one. Layers without a projection load as is and do not
  change the map projection.
- The map projection persists when layers are removed or the map is cleared;
  only explicit `SetProjection`/`ClearProjection` changes it (matches
  MapWindow 5, where clearing layers does not reset the map CRS).

### 1.4 Tests

- `ProjectionTests.cpp`: EPSG round-trips (28992, 4326, 3857); autodetect
  from the WKT1-ESRI `Amersfoort_RD_New` text (the committed
  `embankments_buffered_split.prj` content, embedded as a fixture string) →
  28992; `IsSame` matrix (same EPSG / different WKT text, 4326 vs 3857,
  empty sides); export formats parse back; PROJ4 import.
- `VectorDatasetTests`: `MakeSampleShapefile()` layer reports EPSG 28992; a
  projectionless layer (GeoJSON fixture) reports empty.
- `MapTests`: adoption on first `AddLayer`; no adoption from a
  projectionless layer; persistence after `RemoveLayer`/`Clear`; explicit
  set/clear; `IsSame` used by the adoption check.
- Opt-in test with `MWG_PROJECTION_TEST_DATASET` pointing at the real
  embankments shapefile asserting `Projection().Epsg() == 28992`.

### Acceptance criteria

- Full `ctest --preset x64-debug` green, no new Level 4 warnings.
- Opening the real embankments shapefile in Core resolves its projection to
  EPSG 28992 despite the ESRI WKT without authority codes.
- Core-only: a `Map` adopts the first layer's projection; removing all layers
  keeps it.

---

# Phase 2 – On-the-fly reprojection of map layers in Core (PR 2)

Makes the "reproject the layer" choice meaningful at the engine level.
Geometry on disk is never modified; reprojection is a display-time transform
applied while building the render scene, consistent with the immutable scene
model already in Core.

### 2.1 Display projection on map layers

`MapLayer` gains an optional display projection (default: the source layer
projection). `Map::SetLayerDisplayProjection(LayerId, Projection)` /
`Map::ClearLayerDisplayProjection(LayerId)` change it and mark the layer's
scene stale (returns whether a redraw is needed). Duplicates of the same
source layer reproject independently.

### 2.2 Transform at the scene-build choke point

`MapView` builds layer scenes from OGR geometry at a single conversion path
(`VectorLayerAccess::ForEachGeometry` / the scene assembly in `MapView.cpp`).
When the display projection differs from the source projection, transform
every coordinate through a cached `Projection::TransformTo` for that map
layer (cache keyed by layer identity + projection pair; discarded with the
scene). Rendering, viewport, and caching code stay untouched — they already
operate on scene coordinates.

### 2.3 Extents in map coordinates

`MapLayer::GetExtent()` returns the extent transformed to the display
projection (densified bbox transform, not just corner-to-corner, so curved
projections keep a valid envelope). Map-level extents, `FitToMap`, and
`ZoomToLayer` then work consistently in map coordinates. The source extent
remains available from the `VectorLayer` for callers that need it.

### 2.4 Out of scope (documented)

Editing a reprojected layer writes source coordinates back unchanged — the
current vertical slice is read-only display. State this in the `MapLayer`
doc comment so the limitation is deliberate, not accidental.

### 2.5 Tests

- Reproject the EPSG:28992 sample layer to EPSG:4326: scene coordinates
  match PROJ-computed lon/lat within tolerance; transformed extent is valid
  and contains the transformed features.
- `FitToMap`/`ZoomToLayer` after reprojection; scene invalidation when the
  display projection changes (old geometry never drawn after the change).
- Mixed maps: one reprojected layer + one native layer + one projectionless
  layer; combined extent is consistent.
- Duplicate layers reproject independently; clearing the display projection
  restores source geometry.

### Acceptance criteria

- Full CTest suites pass; no visual regressions in existing pixel tests
  (native layers draw byte-identical scenes — the transform path is skipped
  when projections match).
- A Core test shows a 28992 layer and a 4326 layer aligning in one map when
  one of them is reprojected.

---

# Phase 3 – ActiveX projection interface + test-host prompt and EPSG status bar (PR 3)

Exposes projection to hosts and implements the mismatch prompt and the
status-bar EPSG display.

### 3.1 Projection automation interface

New `IMapWinGISControlProjection : IDispatch` (new IID, added to the coclass)
following the IDL guidance: an independent capability is a separate
dispatch interface, not a version bump of the core contract.

- `MapProjection` (propget/put, BSTR WKT, `""` when unknown) and `MapEpsg`
  (propget, LONG, -1 when unknown).
- `LayerProjection(LONGLONG layerId)` → BSTR WKT; `LayerEpsg(LONGLONG)` →
  LONG.
- `ProbeVectorProjection(BSTR path, LONG* epsg, BSTR* wkt)`: opens the
  dataset read-only, reports its projection, closes it — lets a host decide
  before committing a load.
- `ReprojectLayer(LONGLONG layerId)` / `ClearLayerReprojection(LONGLONG)`:
  drive `Map::SetLayerDisplayProjection` / `ClearLayerDisplayProjection`
  (Phase 2).

The OCX itself shows no UI; it exposes facts and operations and translates
errors with the existing `SetControlComErrorInfo` pattern.

### 3.2 Test-host prompt flow (`ActiveXTestHost`)

- Status bar gains an `EPSG:` label; `RefreshStatus()` and post-load updates
  show the map EPSG (e.g. `EPSG: 28992`, or `EPSG: -`/unknown).
- Interactive loads (Open dialog, drag-drop): probe the projection first.
  Same as the map (or map projection unset) → load directly. Mismatch → a
  three-choice dialog — **Reproject to map projection** (load, then
  `ReprojectLayer`), **Load as is**, **Cancel** — naming both projections and
  their EPSG codes. This mirrors MapWindow 5's
  `ProjectionMismatchService` flow.
- The automated run stays non-interactive: it loads its own generated
  sample (no prompt) and adds a check that `MapEpsg` reports the expected
  code for a generated EPSG:28992 shapefile.
- Layer list optionally shows each layer's EPSG; keep it minimal — a tooltip
  per layer is enough.

### 3.3 Docs and memory

- Update `docs/ARCHITECTURE.md` and `docs/PORTING.md` with the projection
  model, the adoption rule, and the deliberate differences from MapWinGIS 5
  (`projection`/`geoProjection` replaced by one `projection` property; no
  legacy string property).
- Record the decision and verified lessons in `.ai/memory`
  (EPSG-first matching, autodetect strategy, on-the-fly reprojection at the
  scene-build choke point).

### Acceptance criteria

- Release `ActiveXTestHost.exe`: loading
  `embankments_buffered_split.shp` shows `EPSG: 28992` in the status bar
  (map had no projection → adopted from the layer).
- Loading a second layer with a different projection prompts with the three
  choices; each choice behaves as specified (reprojected layer aligns with
  the map).
- Automated suite green; full CTest suites pass.
- Raster note: `mwg::Projection` placement under `projection/` is documented
  as the shared model for the future raster `projection` property.

---

# Key files

- `src/MapWinGIS.Core/include/mapwingis/vector/VectorDataset.h`
  (`ProjectionWkt` :88 — replaced by `Projection()`)
- `src/MapWinGIS.Core/src/vector/VectorDataset.cpp` (`ProjectionWkt` :719)
- `src/MapWinGIS.Core/include/mapwingis/map/Map.h`, `src/MapWinGIS.Core/src/map/Map.cpp`
  (projection state, adoption in `AddLayer`, display projection)
- `src/MapWinGIS.Core/src/map/MapView.cpp` (scene-build choke point,
  layer scene cache invalidation)
- `src/MapWinGIS.Core/src/gdal/GdalEnvironment.cpp` (PROJ data deployment —
  already in place)
- `src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.idl` (new
  `IMapWinGISControlProjection` interface)
- `src/MapWinGIS.ActiveX/MapWinGISControl.cpp/.h` (`LoadVectorDatasetImpl` :1039,
  `CompleteVectorLoad` :854 — commit points that gain projection handling)
- `samples/CSharp/ActiveXTestHost/MainForm.cs` (status bar :145-169,
  `AddVectorDataset` :482, `RefreshStatus` :945 — EPSG label and prompt)
- `tests/MapWinGIS.Core.Tests/` (`ProjectionTests.cpp` new; `TestData.h`
  already provides EPSG:28992 fixtures)

# Verification

- `cmake --build --preset x64-debug` clean at warning level 4;
  `ctest --preset x64-debug` green (plus the Release build), including the
  new projection, reprojection, and automation-interface tests.
- Opt-in: `MWG_PROJECTION_TEST_DATASET` pointing at
  `embankments_buffered_split.shp` resolves to EPSG 28992.
- Manual, Release `ActiveXTestHost.exe`: status bar EPSG; three-way mismatch
  prompt with a 28992 layer and a WGS84 layer; reproject/as-is/cancel
  outcomes; pan/zoom/fit unchanged for native layers.

# Assumptions

- EPSG-code matching is the primary identity; WKT/name comparison is only a
  fallback for CRS without a resolvable code (per the user's steer and
  MapWinGIS 5 practice).
- The prompt belongs to the host application, not the OCX — the control
  exposes probe/reproject operations, the test host owns the dialog (same
  split as MapWindow 5 / MapWinGIS 5).
- Reprojection is display-time only (scene + extent transform); data on disk
  is not rewritten. Layer editing of reprojected layers is out of scope and
  documented.
- The map projection persists once set (survives layer removal and clear);
  the host can change or clear it explicitly.
- PROJ support is confirmed by the existing vcpkg GDAL feature set and the
  deployed `proj/` data folder; Phase 1 verifies this with the real
  autodetect case before anything builds on it.
- Three PRs is the right granularity given 1+ hour CI per PR; each phase
  leaves the tree green and independently reviewable.
