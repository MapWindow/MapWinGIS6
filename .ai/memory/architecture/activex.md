# ActiveX architecture

The OCX is the COM and host-integration boundary around Core. Its control owns
one `mwg::MapView`; it forwards host size and drawing work to that view and
passes the resulting scene to the renderer. Dataset ownership and map state
remain in Core.

## Automation contracts

- `IMapWinGISControl`, `IMapWinGISControl2`, `IMapWinGISControl3`, and
  `IMapWinGISControl4` are versioned derived interfaces with distinct IIDs.
  Do not add vtable members to a published interface or change its IID.
  Extend the contract with a new derived interface when needed.
- The default control interface is `IMapWinGISControl4`. It adds navigation,
  layer count, and stable layer-ID access to `IMapWinGISControl3`. The
  independent generic GDAL capability uses `IMapWinGISControlGdal`, and
  notifications use the separate `IMapWinGISControlEvents` dispinterface.
- These are new v6 interfaces, not the MapWinGIS 5 type-library contracts.
  The current control does not claim v5 binary compatibility.
- Keep COM conversions and COM error translation at this boundary. Core
  exceptions are translated for supported automation calls.

## Rendering and lifetime

The ActiveX rendering coordinator selects a backend for a Core render scene.
GDI is the CPU fallback; OpenGL has a narrower supported-scene range and can
fall back to GDI. Backends consume Core-produced geometry and do not own
datasets or view state. The OCX owns the host-specific rendering resources.

Core objects are not thread-safe by default. The control serializes their use.
ActiveX-specific implementation units are also compiled into
`MapWinGIS.ActiveX.Impl.lib`, allowing native tests to exercise them without
requiring COM activation.

## Current limits

The control exposes a deliberately small v6 automation API. It does not expose
the broad MapWinGIS 5 map, layer, shape, event, or editing surface. The current
render scene is built in source coordinates, without reprojection; backend
support differs, and OpenGL is restricted to convex single-ring polygons.
Consult the current implementation and architecture documentation before
assuming a capability or compatibility guarantee.

## References

- [Architecture documentation](../../../docs/ARCHITECTURE.md#activex-and-rendering-architecture)
- [Porting and compatibility guidance](../../../docs/PORTING.md#current-activex-compatibility-boundary)
- [COM interface definitions](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.idl)
- [Control declaration](../../../src/MapWinGIS.ActiveX/MapWinGISControl.h)
- [ActiveX test project](../../../tests/MapWinGIS.ActiveX.Tests/MapWinGIS.ActiveX.Tests.vcxproj)
