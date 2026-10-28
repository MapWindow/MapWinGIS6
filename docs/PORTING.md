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

The initial `MapWinGIS.ocx` exposes a deliberately small v6 automation
contract. `IMapWinGISControl` reports Core initialization, loads a vector
dataset, and reports whether a layer is loaded and its feature count.
`IMapWinGISControl2` adds renderer preference, active backend, and status.
New members belong on a new derived interface; do not change a published
interface IID or silently add vtable members to it.

These are new interfaces, not the MapWinGIS 5 type-library interfaces, and the
control does not claim binary or API compatibility with existing v5 clients.
The control loads only the first vector layer and currently prepares
Polygon/MultiPolygon scene data. It does not expose the broad v5 map, layer,
shape, event, or editing API. It draws in source coordinates without
reprojection. OpenGL only handles convex single-ring polygons; GDI handles
the supported polygon scene as the CPU fallback.

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
