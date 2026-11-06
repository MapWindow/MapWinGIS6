# Core library

`MapWinGIS.Core.dll` is the native C++ library for GIS data and reusable map
behavior. It has no COM, ATL, MFC, windowing, or graphics API dependency.
Public headers are grouped under `include/mapwingis/<area>/`; implementations
mirror them under `src/<area>/`, and non-public shared code belongs in
`src/internal/`.

## API and ownership

Public APIs use namespace `mwg` and exported declarations. Public classes
commonly hide implementation details behind PImpl. Core owns its map and
dataset state with RAII and standard library ownership types; the ActiveX
layer should call those APIs rather than keep a second map model.

Operational failures use `CoreError` subclasses. A `TryX` API is for an
expected probe that may find no value; `std::optional` represents absence, not
operational failure. Core objects are not thread-safe unless their API says
otherwise.

## GDAL and rendering

Call `GdalEnvironment::Initialize()` before direct GDAL use. It configures
GDAL/PROJ runtime data deployed beside the Core DLL. Do not rely on
`GDAL_DATA` or `PROJ_LIB` environment variables. GDAL datasets and execution
remain Core responsibilities; COM callbacks and host dispatch stay in
ActiveX.

Algorithm outputs need different completion rules. Runner-owned persistent
outputs pass a checked close and reopen read-only before success. Anonymous
outputs retain their references for pipelines. Caller-supplied outputs remain
caller-owned, so the runner can check their flush but not their final close.
See `GdalAlgorithmRunner` and its output-failure and ownership tests.

Core render scenes are owned, backend-neutral snapshots. They are distinct
from viewport state and are rebuilt when relevant map data changes. Source
geometry edits made outside the map are not automatically observed; callers
must invalidate the scene as documented by `MapView`.

## References

- [Architecture documentation](../../../docs/ARCHITECTURE.md#module-design)
- [Coding standards](../../../docs/CODING-STANDARDS.md#api-shape)
- [GDAL integration](../../../docs/gdal-integration.md)
- [MapView public API](../../../src/MapWinGIS.Core/include/mapwingis/map/MapView.h)
- [Core project](../../../src/MapWinGIS.Core/MapWinGIS.Core.vcxproj)
