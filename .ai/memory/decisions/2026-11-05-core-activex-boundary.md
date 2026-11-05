# Keep the ActiveX layer as a Core boundary

- Date: 2026-11-05 (recorded here; original decision date is not recorded)
- Status: Accepted as current repository guidance

## Context

The product needs a Windows COM control, while GIS behavior should also be
usable and testable without COM. Putting GIS rules in the OCX would couple
data behavior to a host surface and make Core-only tests incomplete.

## Decision

Keep GIS data, reusable map behavior, GDAL execution, and backend-neutral
render data in `MapWinGIS.Core.dll`. Keep COM activation, automation
translation, host lifecycle, and graphics-backend integration in the ActiveX
layer. The OCX should remain a relatively slim adapter over Core.

## Consequences

Core stays free of COM, ATL, MFC, windowing, and graphics APIs. ActiveX code
translates Core operations and errors at the boundary, and delegates GIS
behavior to Core. Test Core behavior directly and test native ActiveX
components separately from COM activation where practical.

## References

- [Architecture documentation](../../../docs/ARCHITECTURE.md#two-layers)
- [Core project](../../../src/MapWinGIS.Core/MapWinGIS.Core.vcxproj)
- [ActiveX project references](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.vcxproj)
- [ActiveX interface definitions](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.idl)
