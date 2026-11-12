# Architecture overview

MapWinGIS6 separates reusable GIS behavior from its Windows COM host. This
boundary is more important than matching the legacy project's class layout.

## Components

- `MapWinGIS.Core.dll` owns GIS data and behavior, including OGR-backed vector
  datasets, map and viewport state, GDAL execution, and backend-neutral render
  scenes. Its public API is native C++ and must not depend on COM, ATL, MFC,
  windowing, or graphics APIs.
- `MapWinGIS.ocx` owns COM activation, the host-facing control lifecycle,
  conversion between automation and Core types, and integration with the
  host's drawing surface and graphics backend. It uses Core rather than owning
  duplicate GIS state.
- `MapWinGIS.ActiveX.Impl` contains native ActiveX implementation components
  that are shared by the OCX and the ActiveX test executable. It is a static
  library, not another public runtime layer.

## Where changes belong

Put reusable GIS rules and data ownership in
`src/MapWinGIS.Core/include/mapwingis/<area>/` and the matching
`src/MapWinGIS.Core/src/<area>/` implementation. Put private shared Core code
under `src/MapWinGIS.Core/src/internal/`. Keep COM conversion, activation,
host event handling, graphics-context lifetime, and backend selection in
`src/MapWinGIS.ActiveX/`.

Core prepares backend-neutral render input; the ActiveX renderer consumes that
input and does not load datasets or implement GIS rules. This lets Core tests
exercise map and scene behavior without COM activation, while native ActiveX
tests can exercise renderer components separately from a hosted control.

## References

- [Architecture documentation](../../../docs/ARCHITECTURE.md#two-layers)
- [Core project](../../../src/MapWinGIS.Core/MapWinGIS.Core.vcxproj)
- [ActiveX project references](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.vcxproj)
- [ActiveX implementation project](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.Impl.vcxproj)
- [ActiveX control declaration](../../../src/MapWinGIS.ActiveX/MapWinGISControl.h)
