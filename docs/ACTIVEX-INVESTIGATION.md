# ActiveX and Rendering Investigation

Phase 0 findings for the ActiveX Control and Modern Rendering Architecture
plan. These findings describe the current code and the MapWinGIS 5 reference;
they do not define the final architecture.

## What exists

`MapWinGIS.Core.dll` is a native C++20 DLL with GDAL setup and OGR-backed
vector dataset and layer access. `MwGdalCore` provides GDAL execution support
as a static library. The solution contains those projects and the GoogleTest
project; the core has no COM, ATL, MFC, or rendering dependency.

The public vector API can open a dataset, enumerate layers, inspect extents
and attributes, and read feature geometry as WKT. It has no map/layer state
model, raster-layer API, coordinate-level geometry API, or renderer. Tests can
create a raster directly with GDAL, but this is test fixture code rather than
a core raster API. GDAL must be initialized through `GdalEnvironment`.

The build already defines Debug and Release for Win32 and x64 and uses vcpkg
for GDAL, GEOS, and GoogleTest. Pull requests build and test Debug x64; pushes
to `main` also cover Debug and Release for Win32 and x64.

## MapWinGIS 5 reference

The legacy map control is `CMapView`, derived from `COleControl`. Its control
class handles `OnDraw` and `OnSize`; drawing code redraws map layers into
GDI+ bitmaps and invalidates the control. Its type library defines the
`_DMap` dispatch interface (version 5.4) and `_DMapEvents`, with published
dispatch IDs and event IDs. Those names, IDs, and observable behaviors are
compatibility references; the GDI+/buffering implementation is not a
requirement for the new renderer.

These observations can be checked against the MapWinGIS 5 source at commit
[`c08894b`](https://github.com/MapWindow/MapWinGIS/tree/c08894b481fdc3c0184de1e29a12f45701f62e02):
the [`CMapView` declaration](https://github.com/MapWindow/MapWinGIS/blob/c08894b481fdc3c0184de1e29a12f45701f62e02/src/MapControl/Map.h#L44-L77),
the [drawing implementation](https://github.com/MapWindow/MapWinGIS/blob/c08894b481fdc3c0184de1e29a12f45701f62e02/src/MapControl/Map_Drawing.cpp#L23),
the [`_DMap` and `_DMapEvents` type-library definitions](https://github.com/MapWindow/MapWinGIS/blob/c08894b481fdc3c0184de1e29a12f45701f62e02/src/MapWinGIS.idl#L6779-L7253),
and the [legacy project settings](https://github.com/MapWindow/MapWinGIS/blob/c08894b481fdc3c0184de1e29a12f45701f62e02/src/MapWinGIS.vcxproj#L38-L39).

The legacy `.vcxproj` declares `UseOfMfc` and `UseOfAtl` as false even though
the control derives from `COleControl`. Do not copy its project settings
without reconciling that configuration against the chosen modern COM
implementation.

## Gaps and implementation path

The first map-rendering slice can reuse `VectorDataset`, `VectorLayer`, GDAL
initialization, and the generated vector fixtures. It still needs a map model
or equivalent layer state, a way to provide geometry coordinates to a
renderer, a backend-independent rendering boundary, and a hostable ActiveX
project with a repeatable activation test.

The smallest sequence is to define component boundaries and compatibility
scope; add COM activation; establish paint, resize, and cleanup; expose the
geometry needed by a CPU renderer and display a real vector layer; then add
OpenGL and capability selection, strengthen fallback and resource policies,
validate platforms and hosts, and document the implementation.

The CPU-first vertical slice is complete when a repeatable test activates the
control in a supported host, resize/repaint/cleanup behavior is covered, and a
rendering test draws a real vector fixture. Complete these checks before
starting OpenGL work.

The plan establishes that COM belongs outside the core and that OpenGL and
CPU rendering need a replaceable boundary. It does not settle whether the
new control should use ATL or MFC, or which legacy dispatch members should be
implemented first. Those choices must be made explicitly before adding the
ActiveX project or publishing compatible COM interfaces; the investigation
does not assume either from the legacy source.
