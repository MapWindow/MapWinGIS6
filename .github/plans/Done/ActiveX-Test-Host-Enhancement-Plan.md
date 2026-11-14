# MapWinGIS6 – ActiveX Test Host Enhancement Plan

## Objective

Enhance `samples/CSharp/ActiveXTestHost` into a practical interactive GIS test application for manually validating the new MapWinGIS6 architecture.

Repository: https://github.com/MapWindow/MapWinGIS6

Target branch: `development`

The application must test the functionality already implemented in `MapWinGIS.Core` and expose missing functionality through `MapWinGIS.ActiveX`.

The goal is not to recreate MapWindow5 or build a complete GIS application.

## Important architectural rules

1. Keep `MapWinGIS.Core.dll` independent of COM, ATL, MFC, WinForms and Windows UI.
2. Keep the ActiveX control relatively slim.
3. Reuse the existing `mwg::MapView` and `mwg::Viewport` implementations.
4. Do not implement GIS navigation or coordinate transformations in C#.
5. Preserve existing COM interfaces and their IIDs. Add new functionality through new derived interfaces with new IIDs.
6. Maintain existing GDI and OpenGL rendering implementations.
7. Preserve the existing C# automated test mode, JSON reports and x86/x64 support.
8. Do not introduce breaking changes to existing functionality.
9. Follow AGENTS.md and the repository coding standards.
10. Do not modify CMake files or interfere with the ongoing CMake PRs. Keep the existing MSBuild solution and C# host build working.
11. Do not implement projections, raster rendering, tile services or vector editing in this task.

## Phase checklist

- [x] PR 1: Expose map navigation through ActiveX.
- [x] PR 2: Improve the C# manual testing interface.
- [x] PR 3: Implement interactive mouse navigation.
- [x] PR 4: Expand automated tests, diagnostics and documentation.

Each phase must be independently buildable and testable.

---

## PR 1 – Expose map navigation through ActiveX

Core already implements the following methods in `mwg::MapView`:

- FitToMap()
- ZoomToLayer()
- ZoomIn()
- ZoomOut()
- ZoomAt()
- Pan()
- Resize()

Expose these methods through a new COM interface derived from the existing `IMapWinGISControl3`.

Do not modify existing published interfaces.

Expose:

- FitToMap()
- ZoomIn(factor)
- ZoomOut(factor)
- Pan(deltaX, deltaY)
- ZoomAt(x, y, factor)
- ZoomToLayer(layerId)

Also expose the layer information needed by clients:

- LayerCount property.
- LoadVectorDatasetEx(path), returning the newly created stable layer ID.
- GetLayerId(index), returning the stable Core LayerId.

Keep the existing LoadVectorDataset(path) method unchanged.

Implementation requirements:

- Update MapWinGIS.ActiveX.idl.
- Update MapWinGISControl.h and MapWinGISControl.cpp.
- Reuse the existing `_view` member.
- Do not introduce another map or viewport.
- Call FireViewChange() when the view changes.
- Validate input parameters.
- Translate Core exceptions into appropriate COM errors.
- Preserve existing DISPIDs and interface IIDs.
- Add automated tests for the new COM functionality.

Verify that all existing ActiveX tests still pass.

## PR 2 – Improve the C# manual testing interface

Enhance the existing WinForms application in:

`samples/CSharp/ActiveXTestHost`

Do not replace the existing application or introduce a new framework.

Create a practical layout with:

### Toolbar

- Open vector dataset.
- Clear all layers.
- Zoom in.
- Zoom out.
- Zoom to map.
- Zoom to selected layer.
- Directional pan buttons.

### Layer panel

- Display all loaded layers.
- Display layer names.
- Display feature counts.
- Allow selecting a layer.
- Provide zoom-to-layer functionality.

### Map surface

- Keep the ActiveX control filling the available space.
- Maintain existing resizing and repaint behavior.
- Support loading multiple datasets.

### Status bar

Display:

- Core initialization status.
- Layer count.
- Feature count.
- Active rendering backend.
- Rendering status.

### File loading

- Use OpenFileDialog.
- Allow drag-and-drop of vector datasets onto the application.
- Allow loading multiple datasets.
- Use the existing ActiveX LoadVectorDataset functionality.
- Use the new layer ID functionality from PR 1.

Use the existing generated GeoJSON dataset for initial testing.

Do not implement raster visualization. The existing ASCII Grid file is used for GDAL conversion testing only.

Implementation note: ActiveX exposes no per-layer name or feature-count
property. The host displays each source file name and derives its feature count
from the change in the aggregate count after loading.

## PR 3 – Implement interactive mouse navigation

Implement mouse interaction in the ActiveX control.

Required behavior:

- Left mouse drag: pan.
- Mouse wheel up: zoom in.
- Mouse wheel down: zoom out.
- Mouse wheel zoom: anchor zoom to the cursor.
- Resize: maintain the current map center and resolution.

Implementation requirements:

- Handle the appropriate Windows mouse messages in MapWinGISControl.
- Reuse MapView::Pan and MapView::ZoomAt.
- Use the existing viewport resolution to translate pixel movement into map-coordinate movement.
- Handle mouse capture and release correctly.
- Handle capture loss and control destruction.
- Request redraw only when the view changes.
- Avoid unnecessary render-scene rebuilding.
- Support both GDI and OpenGL rendering backends.

The C# application should not implement its own GIS transformations.

Add automated tests for the underlying navigation behavior wherever practical.

The hosted mouse-message test is opt-in and requires the per-user registration
script; it is not part of the default CI test run.

## PR 4 – Expand automated testing and documentation

Extend the existing automated test mode.

Do not remove or weaken existing checks.

Add checks for:

- Multiple dataset loading.
- Correct layer count.
- Stable layer IDs.
- Zoom to map.
- Zoom to individual layer.
- Zoom in and zoom out.
- Pan.
- ZoomAt.
- Clear all layers.
- Reloading after clearing.
- Navigation after resizing.
- Invalid dataset handling.
- Invalid layer ID handling.
- Navigation on an empty map.
- Both rendering backends where supported.

Keep the automated mode non-interactive and suitable for CI.

Continue using the existing JSON result report and timeout mechanisms.

Add a README.md in `samples/CSharp/ActiveXTestHost` describing:

- How to build the application.
- How to run it.
- How to load vector datasets.
- How to test layer management.
- How to test navigation.
- How to test rendering backends.
- Known limitations.

Implementation note: The C# host exercises the navigation calls and checks
layer state. Native Core and ActiveX tests check the resulting viewport
behavior because the host API does not expose viewport extents. Automated
checks need an interactive Windows desktop and are not part of headless CI.

## Validation requirements

After each PR:

1. Build Debug | x64.
2. Run the existing Core unit tests.
3. Run the ActiveX automated test host.
4. Verify the manual application starts and hosts the control.
5. Check that existing functionality remains intact.
6. Review x86 compatibility when the relevant build is available.
7. Ensure there are no new compiler warnings.

## Definition of done

The final application should allow a developer to:

1. Start the C# application without registering the OCX globally.
2. Load one or more vector datasets.
3. See all loaded layers.
4. Select individual layers.
5. Zoom to the complete map.
6. Zoom to individual layers.
7. Zoom in and out.
8. Pan using buttons and the mouse.
9. Zoom using the mouse wheel.
10. Drag and drop vector files.
11. Switch between GDI and OpenGL where supported.
12. Clear and reload datasets.
13. Observe the rendering backend and initialization status.
14. Repeatedly test the control without restarting the application.

Prioritize a clean architecture and reliable manual testing over implementing additional GIS features.