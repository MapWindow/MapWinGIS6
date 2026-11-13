# Porting from MapWinGIS 5

[MapWinGIS 5](https://github.com/MapWindow/MapWinGIS) is a large MFC/ATL
project (the `CMapView` control plus COM classes, with the type library in
`MapWinGIS.odl`). It is a source of behavioral knowledge, not the blueprint
for the new architecture.

**Design priority: optimize the new version first, preserve backward
compatibility second.** Build clean modern APIs and architecture around the
actual needs of MapWinGIS6. Retain legacy behavior or API compatibility when it
provides value and fits the design; do not add complexity, preserve flawed
structures, or constrain improvements solely for parity. Where compatibility
is intentionally not retained, document the difference and rationale.

For GDAL-specific migration guidance, see [GDAL migration](gdal-migration.md).

## What "port" means here

1. **Understand the legacy behavior.** Inspect v5 sources and tests to learn
   expected semantics, edge cases, units, coordinate order, and defaults.
2. **Design for MapWinGIS6 first.** Choose clean modern C++ APIs and modular
   responsibilities. Do not mirror legacy class hierarchies or COM signatures
   unless there is a concrete compatibility benefit.
3. **Choose compatibility deliberately.** Preserve behavior that matters and
   fits the new design. If parity would distort the architecture, add needless
   complexity, or block a worthwhile improvement, prefer the better v6 design
   and document the difference.
4. **Implement and test.** Add GoogleTest coverage for intended behavior,
   including edge cases and failures. Keep COM mapping in the ActiveX layer;
   implement legacy API compatibility incrementally where justified.

## Compatibility decisions

Compatibility is a design consideration, not an absolute rule that overrides
architecture or maintainability. For each legacy behavior or API surface,
choose deliberately whether it belongs in v6 and document important departures.

- **Indexes:** use 0-based feature/shape indexes where that remains the chosen
  v6 API contract.
- **Enums:** pin numeric values to the v5 type library when an enum is intended
  to map directly to that COM contract; unrelated v6 enums need not inherit
  legacy numbering.
- **Errors:** prefer clear, structured core errors and modern result patterns.
  Translate them to legacy error codes at the COM boundary only where needed
  for a supported compatibility surface.
- **Formatting:** preserve legacy display formatting when existing consumers
  rely on it; otherwise prefer a consistent, documented v6 representation.

## Current ActiveX compatibility boundary

Use a derived interface with a new IID to add members to an existing
automation contract. This preserves the published interface while allowing
clients to query the newer version. Use a separate `IDispatch` interface for
an independent capability that is not an extension of that contract. Keep
events in a separate dispinterface.

The initial `MapWinGIS.ocx` exposes a deliberately small v6 automation
contract. `IMapWinGISControl` reports Core initialization, loads a vector
dataset, and reports whether a layer is loaded and its feature count.
`IMapWinGISControl2` adds renderer preference, active backend, and status.
`IMapWinGISControl3` adds `ClearLayers`. `IMapWinGISControl4` derives from it
and is the default automation interface; it adds view navigation, layer count,
`LoadVectorDatasetEx`, and stable layer-ID lookup by 0-based position. Loads
add layers, including duplicate loads of one dataset. Each successful load
requests a fit to the combined map extent, replacing any previous pan or zoom
when the extent is valid and the viewport has a size.
Call `ClearLayers` before switching
or refreshing datasets. It releases all layers and resets the cached feature
total while keeping the viewport.
`IMapWinGISControlGdal` is a separate interface for the independent GDAL
operation capability, and `IMapWinGISControlEvents` is a separate dispinterface
for notifications. Do not change a published interface IID or silently add
vtable members to it.

| Navigation member | Coordinates and factors |
|---|---|
| `Pan(deltaX, deltaY)` | Finite deltas in map-coordinate units; they move the view center. |
| `ZoomIn(factor)`, `ZoomOut(factor)` | A finite factor greater than 1, applied around the view center. |
| `ZoomAt(x, y, factor)` | A finite anchor in screen pixels from the top-left; a finite positive factor above 1 zooms in and below 1 zooms out. |
| `FitToMap()`, `ZoomToLayer(layerId)` | Fit the visible map or the named layer with 12 pixels of padding. |

Navigation returns `S_OK` even if the view did not change, for example when
there is no valid extent or a zoom limit is reached. A fit requested before
the viewport has a size is deferred until it is resized. An unknown layer ID
returns `E_INVALIDARG`.

Layer IDs and `FeatureCount` use Automation `hyper` (`VT_I8`). Hosts must
support 64-bit integer Automation values to use these members. VB6, 32-bit
VBA, and VBScript cannot directly use these signatures; use a host with
`VT_I8` support for layer-ID navigation. Other members do not require it.
The type library remains at version 1.0: the added interface has its own IID
and extends the existing library without changing published interfaces.

These are new interfaces, not the MapWinGIS 5 type-library interfaces, and the
control does not claim binary or API compatibility with existing v5 clients.
Each control load selects the first dataset layer. Core prepares an ordered
scene for all visible map layers, including Polygon, MultiPolygon,
CurvePolygon, and MultiSurface geometry. It
does not expose the broad v5 map, layer, shape, event, or editing API. It draws
in source coordinates without reprojection. OpenGL fills the triangulated
meshes Core prepares for convex, concave, holed, and multipart polygons; GDI
draws the original rings as the CPU fallback for scenes and systems OpenGL
cannot handle.

Keep these gaps visible when extending the COM surface. Reuse Core behavior,
add only the automation members required by a concrete use case, and retain a
legacy identifier only after verifying that its full signature and behavior
are compatible. Do not copy the v5 class hierarchy or rendering pipeline to
make the initial control appear compatible.

## Anti-patterns

- Do not copy the v5 file organization (flat folders, `stdafx.h`, COM maps).
- Do not port MFC containers (`CString`, `CArray`, `CMap`) — use
  `std::string`, `std::vector`, `std::map`.
- Do not port GDI rendering or window-message logic into the core. Put
  reusable map rendering and GIS behavior in the core rendering area. Keep
  host-surface, graphics-context, and backend integration in native OCX
  components; they forward render requests without implementing GIS rules.
- Do not "improve" behavior silently. Differences from v5 must be deliberate
  and recorded in the header's doc comment so the ActiveX implementation and
  future maintainers can see the decision.

## When v5 behavior is unclear or contradictory

Prefer the behavior the [MapWindow application](https://github.com/MapWindow/MapWindow5) and its existing tests depend
on, and record the decision in the relevant header doc comment.
