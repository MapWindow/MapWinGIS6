# Porting from MapWinGIS 5

[MapWinGIS 5](https://github.com/MapWindow/MapWinGIS) is a single large
MFC/ATL project (the `CMapView` control plus COM classes, with the type
library in `MapWinGIS.odl`). It is the **reference implementation**: when
re-creating a feature, its behavior is the specification. The goal is a
modern rewrite, not a line-by-line translation.

For GDAL-specific migration guidance, see [GDAL migration](gdal-migration.md).

## What "port" means here

1. **Study the v5 behavior.** Read the v5 sources for the feature: what the
   public API accepts and returns, edge cases, error behavior, units,
   coordinate order and defaults.
2. **Design the core API first.** Express it with the current conventions
   (PImpl class in `include/mapwingis/<area>/`, `std::optional`, exceptions
   with `TryX` companions). Resist mirroring v5 class hierarchies and COM
   signatures.
3. **Implement in the core and test against the spec.** Add GoogleTest
   coverage for the ported behavior, including edge cases and failures.
4. **Leave COM mapping to the ActiveX layer.** The wrapper maps the
   intentionally implemented subset of core types and exceptions onto the v5
   COM API and error codes. Broader API compatibility is implemented
   incrementally, not assumed.

## Behavioral contract to preserve

The ActiveX layer will be a mechanical translation, so the core must keep the
following true:

- **Feature/shape indexes are 0-based** and match v5 shape indexes exactly.
- **Enums that correspond to v5 type-library enums keep identical numeric
  values** (see `ShapeType`/`FieldType` in `mapwingis/core/Enums.h`). Add the
  same pinning note to every new ported enum.
- **Error semantics**: v5 reports failures via error codes and
  `LastErrorMessage`-style state; the core throws or returns
  `std::optional`, with the same information in the exception message.
- **Formatting**: where v5 formats values for display (e.g. `CellValue`
  text), the core keeps the same textual output.

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
