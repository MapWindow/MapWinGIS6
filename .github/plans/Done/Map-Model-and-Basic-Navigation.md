# MapWinGIS6 — Map Model and Basic Navigation

## Overall implementation checklist

Complete each phase in order. Each phase should be implemented and reviewed in its own pull request.

- [x] **Phase 1 — Map model and layer collection**
- [x] **Phase 2 — Viewport and coordinate transformations**
- [x] **Phase 3 — Multi-layer rendering**
- [x] **Phase 4 — Programmatic map navigation**
- [x] **Phase 5 — Interactive navigation in ActiveX**
- [x] **Phase 6 — Drag-and-drop and end-to-end testing**
- [x] **Phase 7 — Verify complete implementation and fix remaining open issues**

## Goal

Build the first reusable MapWinGIS6 map implementation. It manages vector and raster layers, calculates the visible map extent, reprojects declared coordinate reference systems, renders supported geometry through a common viewport, and allows users to navigate through ActiveX.

XYZ tile layers, viewport-sized raster reads, GDI clipping, per-layer scene
caching, and layer reordering are implemented. WMTS/WMS services, OpenGL
clipping, and broader large-scene optimizations remain future work.

## Architectural decisions

These decisions apply throughout the plan.

1. **One authoritative map extent.** The Core map/viewport model owns the current view state. ActiveX must not maintain an independent, competing map extent.
2. **Stable layer identity.** Every layer has an identity that remains stable when other layers are added or removed. Do not use a display name or list index as the sole identity.
3. **Map coordinate system.** The first layer's declared coordinate reference system defines the map coordinate system. Reproject other declared coordinate systems into it, and reject a map that mixes referenced and unreferenced layers rather than silently aligning them.
4. **Core owns GIS state and calculations.** Layer collections, extents, viewport transformations, and navigation calculations belong in Core. ActiveX translates host and mouse events into map operations.
5. **Rendering backends remain separate.** GDI and OpenGL render the prepared scene; neither should become the authoritative owner of map state.
6. **Preserve the existing architecture.** Inspect and reuse existing types before introducing new classes. Keep the Core library independent of COM, window handles, and ActiveX-specific behavior.

---

# Phase 1 — Map model and layer collection

## Objective

Introduce a single map model in Core that owns the ordered collection of layers and provides map-level extent calculations.

## Implementation checklist

### 1.1 Inspect existing types

- [x] Review `VectorLayer`, `VectorDataset`, `Extent`, `VectorRenderScene`, and the existing renderer interfaces.
- [x] Identify existing layer ownership and lifetime rules.
- [x] Identify where the current ActiveX implementation stores the loaded dataset and extent.
- [x] Reuse existing abstractions wherever possible; avoid duplicating existing functionality.

### 1.2 Define the map model

- [x] Introduce or adapt a Core map class.
- [x] Make the map the authoritative owner of its layer collection and map-level state.
- [x] Define the collection's ordering semantics.
- [x] Define how empty maps and invalid extents are represented.
- [x] Keep windowing, COM, and rendering-backend details outside the map model.

### 1.3 Implement layer management

- [x] Add a layer to the map.
- [x] Remove a layer by stable identity.
- [x] Clear all layers.
- [x] Query layer count and retrieve layers.
- [x] Define duplicate-layer behavior.
- [x] Define layer visibility and whether hidden layers contribute to zoom-to-map.
- [x] Define ownership and lifetime behavior for layers and their underlying datasets.
- [x] Ensure removing one layer does not invalidate unrelated layers.

### 1.4 Implement extent aggregation

- [x] Retrieve each layer's extent.
- [x] Calculate the combined extent of eligible layers.
- [x] Ignore empty or invalid extents.
- [x] Handle an empty map and a map with no valid layer extents.
- [x] Define how extent changes invalidate or refresh the combined extent.
- [x] Reuse the existing `Extent` implementation where practical.

### 1.5 Add unit tests

- [x] Add and remove layers.
- [x] Clear the map.
- [x] Verify stable identities after other layers are removed.
- [x] Verify layer ordering.
- [x] Verify combined extents for overlapping and separated layers.
- [x] Verify empty and invalid extent handling.
- [x] Verify hidden-layer behavior.

## Definition of done

- [x] Core can represent a map containing multiple vector layers.
- [x] Layer identity and ownership rules are documented.
- [x] The combined extent is correct and deterministic.
- [x] Tests pass without creating a Windows window or using ActiveX.
- [x] Existing tests remain passing.

## Remaining limitations (follow-up)

- ActiveX still keeps its own single dataset and extent; switching it to `mwg::Map` is Phase 3 work (3.1).
- `Map::GetExtent` recomputes on every call. Formats without a stored extent may need a full layer scan the first time (the `VectorLayer` cache helps after that). Add caching if navigation profiling shows a need.
- CRS validation and reprojection were deferred at this phase and implemented
  during Phase 7 (see decision 3).
- At the end of Phase 1, only vector layers were supported. Raster and XYZ tile
  support were added later; WMTS and WMS sources are not supported.
- `MapLayer::Source()` gives mutable access to the source layer through a const
  `MapLayer`, so callers can edit geometry. Callers must invalidate the view's
  render scene after edits; there is no automatic change notification.
- `Map` is not thread-safe, like `VectorLayer`.

---

# Phase 2 — Viewport and coordinate transformations

## Objective

Implement a reusable viewport that converts map coordinates to screen pixels and fits a map extent into the available display area.

## Implementation checklist

### 2.1 Define viewport semantics

- [x] Introduce or adapt a Core viewport type.
- [x] Define viewport width and height in pixels.
- [x] Define the visible map extent and its relationship to the map model.
- [x] Document the coordinate conventions: map X increases right, map Y increases up; screen X increases right, screen Y increases down.
- [x] Define behavior for zero-sized or invalid viewports.
- [x] Define sensible limits for zoom and map extent.

### 2.2 Implement coordinate transformations

- [x] Convert map coordinates to screen coordinates.
- [x] Convert screen coordinates to map coordinates.
- [x] Handle the inverted screen Y axis correctly.
- [x] Avoid premature rounding; round only when converting to pixel coordinates.
- [x] Handle invalid inputs without producing undefined or non-finite results.

### 2.3 Implement extent fitting

- [x] Fit a supplied extent into the viewport.
- [x] Preserve aspect ratio to avoid distorting geometry.
- [x] Support configurable padding.
- [x] Expand the fitted extent when necessary to match the viewport aspect ratio.
- [x] Handle zero-width or zero-height extents safely.
- [x] Avoid clipping geometry due to rounding at viewport boundaries.

### 2.4 Add unit tests

- [x] Test all four map-extent corners.
- [x] Test the map center.
- [x] Test map-to-screen-to-map round trips.
- [x] Test wide and tall viewports.
- [x] Test wide and tall extents.
- [x] Test padding and aspect-ratio preservation.
- [x] Test zero-sized viewports and degenerate extents.
- [x] Test invalid and non-finite inputs.

## Definition of done

- [x] The viewport works independently of ActiveX and the rendering backend.
- [x] Coordinate transformations are predictable and tested.
- [x] Extent fitting preserves aspect ratio.
- [x] Invalid dimensions and extents are handled safely.
- [x] All relevant Core tests pass.

## Remaining limitations (follow-up)

- ActiveX still uses its own `mwg::activex::ViewTransform` (fixed 12-pixel margin, recomputed from the dataset extent on every draw). Replace it with `mwg::Viewport` in Phase 3 (3.1). Use `FitExtent(map.GetExtent(), 12.0)` to keep the current look.
- `Viewport` sits next to `Map` and is not owned by it. Phase 3 has to choose one owner for the pair, for example the control or a future map-view type, so there is still a single authoritative view state (decision 1).
- `FitExtent` returns `false` on a zero-sized viewport. Hosts that load data before the first resize must fit again once they have a size (Phase 3, 3.3).
- No rotation and no separate X/Y scales; the viewport is axis-aligned with square pixels.
- No DPI or device-pixel-ratio handling yet. Pixels are device pixels.
- The limits (`kMaxCoordinate` 1e12, `kMaxResolution` 1e9, relative minimum resolution 1e-10, `kMaxDimension` 65536) are engineering choices and are not tied to the CRS. Phase 4 zoom limits may want CRS-aware bounds.
- `Viewport` is not thread-safe.

---

# Phase 3 — Multi-layer rendering

## Objective

Connect the map model and viewport to the existing rendering pipeline so multiple layers are drawn using the same map state and transformation.

## Implementation checklist

### 3.1 Integrate map state with rendering

- [x] Make the rendering flow obtain its layers from the Core map model.
- [x] Pass the shared viewport transformation into the layer-rendering pipeline.
- [x] Remove duplicate or competing extent state from ActiveX or renderer code where appropriate.
- [x] Preserve the existing GDI and OpenGL rendering backends.
- [x] Keep the render scene separate from persistent map and layer state.

### 3.2 Render multiple layers

- [x] Render layers in deterministic order.
- [x] Respect layer visibility.
- [x] Skip layers with no renderable geometry.
- [x] Ensure all layers use the same viewport transformation.
- [x] Ensure a failure in one layer does not unnecessarily prevent other layers from rendering.
- [x] Define how map changes trigger a redraw.

### 3.3 Integrate viewport resizing

- [x] Update viewport dimensions when the ActiveX control is resized.
- [x] Recalculate the viewport transformation.
- [x] Request a redraw.
- [x] Preserve the current map center where practical.
- [x] Handle very small and unusually wide or tall control dimensions.

### 3.4 Add tests and validation

- [x] Test construction of a render scene containing multiple layers.
- [x] Test layer ordering and visibility.
- [x] Test rendering with layers having different extents.
- [x] Test that the viewport is shared across layers.
- [x] Verify existing single-layer rendering still works.
- [x] Run the existing renderer and Core regression tests.

## Definition of done

- [x] Two or more vector layers can be displayed in one map.
- [x] All layers use the same authoritative map extent and viewport.
- [x] Resizing the control does not distort the map.
- [x] GDI/OpenGL remain rendering backends, not owners of map state.
- [x] Existing rendering tests continue to pass.

## Remaining limitations (follow-up)

- The COM API exposes layer listing by stable ID, removal, clear, visibility,
  and reordering.
- Loading a layer preserves the current view after the initial layer; call `ZoomToMap` to include later layers in the visible extent.
- The C# host loads two GeoJSON layers for navigation checks. Drag-and-drop and its end-to-end user workflow remain Phase 6 work.
- Per-layer vector scenes are cached across layer membership, visibility, and
  ordering changes. Edits to source geometry are not observed automatically;
  callers must call `MapView::InvalidateScene`.
- One backend draws the whole frame: one unsupported layer (for example, a polygon with a hole) sends every layer to GDI.
- All layers use one fixed fill and outline color; there is no per-layer style, so draw order is not visible in pixels and is tested at scene level only.
- A rejected `LoadVectorDataset` still uses up a `LayerId`, and when it was the first layer the view stays fitted to the rejected layer's extent.
- `OnDraw` sets the viewport size from the drawn bounds (windowless hosts send no `WM_SIZE`). Drawing to a different-size target, such as printing, therefore changes the viewport size; the center and scale are kept.

---

# Phase 4 — Programmatic map navigation

## Objective

Add testable map navigation operations in Core before wiring them to mouse input and host interaction.

## Implementation checklist

### 4.1 Zoom to map

- [x] Fit the combined eligible layer extent into the viewport.
- [x] Support padding.
- [x] Handle empty maps and invalid extents safely.
- [x] Define whether hidden layers are excluded.
- [x] Request a redraw when the view changes.

### 4.2 Zoom to layer

- [x] Fit a selected layer's extent into the viewport.
- [x] Identify the layer using its stable identity.
- [x] Handle missing layers and invalid extents.
- [x] Support padding.
- [x] Request a redraw.

### 4.3 Zoom in and out

- [x] Implement zoom-in and zoom-out operations using a defined zoom factor.
- [x] Preserve the map center for center-based zoom.
- [x] Prevent invalid or extreme zoom values.
- [x] Ensure the operations update the authoritative viewport state.

### 4.4 Implement reusable pan and anchored-zoom calculations

- [x] Implement a viewport operation that shifts the visible extent by a map-coordinate delta.
- [x] Implement zoom around a specified screen position.
- [x] Preserve the map coordinate beneath the specified screen position when zooming.
- [x] Keep these calculations independent of Windows mouse events.

### 4.5 Add unit tests

- [x] Zoom to map with one layer.
- [x] Zoom to map with multiple layers.
- [x] Zoom to layer for large and small layers.
- [x] Zoom on an empty map or invalid layer.
- [x] Verify zoom-in and zoom-out factors.
- [x] Verify center preservation for center-based zoom.
- [x] Verify cursor-anchor preservation for anchored zoom.
- [x] Verify pan direction and distance.

## Definition of done

- [x] All navigation operations can be invoked programmatically.
- [x] Navigation calculations are independently unit-tested.
- [x] No navigation logic depends on COM, mouse messages, or a specific renderer.
- [x] Empty and invalid cases are handled safely.

Remaining limitations (follow-up)

- Deep zoom is limited by `Viewport::MinResolution`. GDI clips finite vector
  geometry to the control bounds; OpenGL still supports only convex single-ring
  polygons and uses GDI for unsupported scenes.
- At a resolution or center limit, `ZoomAt` clamps and the anchored map coordinate can move; center clamping can also make `Pan` stop short of the requested delta.
- Zoom levels are continuous (any factor); there are no snapped scales or scale denominators, and no CRS-aware limits.
- `ZoomToLayer` fits hidden layers too (the caller named the layer); `FitToMap` uses visible layers only.
- A pending fit is a single request: a new fit replaces it and any navigation step that changes the view cancels it. A pending `ZoomToLayer` whose layer is removed is dropped on the next resize.
- There is no navigation history (previous/next extent) and no animated transitions.

---
# Phase 5 — Interactive navigation in ActiveX

## Objective

Connect the tested Core navigation operations to the ActiveX control's resize, mouse-wheel, and pointer-drag events.

## Implementation checklist

### 5.1 Expose appropriate host-facing methods

- [x] Add or adapt a COM-visible operation for zoom-to-map.
- [x] Add or adapt a COM-visible operation for zoom-to-layer.
- [x] Add zoom-in and zoom-out methods if appropriate for the public API.
- [x] Use the current COM API conventions for errors and return values.
- [x] Keep internal Core implementation types out of the COM contract unless explicitly wrapped.
- [x] Test the methods from the C# test application.

### 5.2 Mouse-wheel zoom

- [x] Convert the cursor position to the viewport's coordinate space.
- [x] Invoke cursor-anchored zoom.
- [x] Handle wheel direction and multiple wheel increments.
- [x] Request a redraw.
- [x] Test zooming at the center and near viewport edges.

### 5.3 Drag-to-pan

- [x] Record the pointer position when dragging starts.
- [x] Record the initial extent or anchor state.
- [x] Convert screen movement into map-coordinate movement.
- [x] Update the view during dragging or at a controlled redraw rate.
- [x] Handle pointer release outside the control.
- [x] Ensure mouse capture and focus are released correctly.
- [x] Avoid interfering with normal host application interaction.

### 5.4 Verify resizing and redraw behavior

- [x] Recalculate viewport dimensions on resize.
- [x] Keep the current view stable where practical.
- [x] Avoid unnecessary redraws.
- [x] Verify the map does not distort or unexpectedly jump after resizing.

### 5.5 Integration tests

- [x] Test programmatic zoom-to-map from C#.
- [x] Test zoom-to-layer from C#.
- [x] Test mouse-wheel zoom.
- [x] Test drag-to-pan.
- [x] Test resizing during normal use.
- [x] Verify the control remains responsive after repeated interactions.

## Definition of done

- [x] Programmatic navigation works through the ActiveX API.
- [x] Mouse-wheel zoom and drag pan work in the C# host.
- [x] Navigation uses the Core map/viewport state.
- [x] No duplicate extent state is introduced in ActiveX.
- [x] Existing COM activation and loading behavior still work.

## Remaining limitations (follow-up)

- Snapped zoom scales or scale denominators are not available.
- Later `LoadVectorDataset` calls preserve the current view. Hosts should call `ZoomToMap` when they want newly loaded layers included in the visible extent.
- Automated ActiveX input and pixel checks require an interactive Windows desktop session and are not currently part of CI.

---

# Phase 6 — Drag-and-drop and end-to-end testing

## Objective

Complete the basic end-user workflow: drop supported vector or raster files onto the control, load them as layers, and navigate the resulting map.

## Implementation checklist

### 6.1 Implement drag-and-drop

- [x] Enable file drag-and-drop on the ActiveX control.
- [x] Accept supported vector and raster file paths from Windows Explorer.
- [x] Validate paths and supported formats.
- [x] Load datasets using the existing Core/GDAL data-loading path.
- [x] Add successfully loaded datasets as layers with stable identities.
- [x] Define and document whether dropped files append to the current map or replace it.
- [x] Define whether dropping files automatically zooms to the new layer or the combined map.
- [x] Handle unsupported files and loading errors without crashing.
- [x] Handle multiple dropped files and partial failures.

### 6.2 End-to-end validation

- [x] Open one vector dataset.
- [x] Drop a second dataset onto the control.
- [x] Verify both layers render.
- [x] Verify the combined extent includes both layers.
- [x] Zoom to map and to each layer.
- [x] Use mouse-wheel zoom and drag pan.
- [x] Resize the control repeatedly.
- [x] Drop multiple files in one operation.
- [x] Try unsupported or invalid files.
- [x] Verify the control remains usable after a loading error.

### 6.3 Regression and documentation

- [x] Run all relevant Core, GDAL, rendering, and ActiveX tests.
- [x] Confirm existing functionality is not regressed.
- [x] Document the map model, viewport semantics, and layer identity.
- [x] Document the new ActiveX API and drag-and-drop behavior.
- [x] Document map coordinate-system selection and reprojection behavior.
- [x] Verify the project builds in the required CI configurations.

## Definition of done

- [x] A C# host application can load multiple vector and raster datasets.
- [x] Users can add supported vector and raster files through drag-and-drop.
- [x] All loaded layers share the same map/viewport state.
- [x] Zoom-to-map, zoom-to-layer, mouse-wheel zoom, and pan work.
- [x] Errors are handled predictably.
- [x] Relevant tests and CI checks pass.
- [x] Documentation is updated.

## Remaining limitations (follow-up)

- Native ActiveX tests exercise the windowless OLE drop-target with `CF_HDROP`
  and verify loading, combined rendering, and partial failures. A manual
  Explorer-to-C#-host drag remains a desktop-only check and is not in CI.
- File-load failures are reported through the drop result and debugger output;
  there is no user-facing per-file error dialog or automation event.
- XYZ tile URL templates are supported through GDAL's WMS/TMS driver, with
  Web Mercator addressing and a bounded in-memory cache. WMTS/WMS services are
  not supported, and tile requests are synchronous.
- `MapView` reads only the visible raster window at viewport resolution, subject
  to a 16-million-pixel output limit. The standalone scene-builder retains its
  bounded-preview behavior.
- The map CRS is established by its first layer and remains until the map is
  cleared, even if that layer is removed.
- Raster reprojection uses nearest-neighbor sampling, and raster display
  supports grayscale, RGB, palette, nodata, and alpha-band data only.
- The visible scene remains in memory. Per-layer vector scenes are cached
  across map membership, visibility, and ordering changes; source edits require
  explicit invalidation. GDI clips vectors to the control bounds. OpenGL still
  supports only convex single-ring polygons before falling back to GDI.
- Navigation history is bounded to 100 views. ActiveX animated fits use its
  owner-thread timer; hosts without message dispatch cannot advance animations.
- Touch pan and pinch are implemented through Windows pointer messages but
  still need manual validation with touch hardware; CI does not inject touch.
- After editing source geometry, callers must invalidate the map view's render
  scene; there is no automatic edit notification.
