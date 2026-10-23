# Architecture

## Two layers

MapWinGIS6 splits the monolithic MapWinGIS 5 control into two layers:

1. **MapWinGIS.Core.dll** — the mapping engine, a plain C++ DLL. All logic
   lives here. No COM, ATL or MFC anywhere in this layer.
2. **MapWinGIS.ocx** — a thin ActiveX/COM wrapper (not started yet). Its only
   jobs are to expose the v5 COM API, translate between COM and core types,
   and convert core exceptions into MapWinGIS error codes.

Everything that can be tested must be testable through the core alone.

## Module design

- `MwGdalCore` is a native static library linked into `MapWinGIS.Core.dll`.
  It contains GDAL execution support and has no COM, ATL or MFC dependency.
- Public API lives in `src/MapWinGIS.Core/include/mapwingis/<area>/`, grouped
  by area (`core`, `gdal`, `vector`, ...). All of it is in namespace `mwg`.
- Implementation lives in `src/MapWinGIS.Core/src/<area>/`, mirroring the
  public header names. Shared internals that are not part of the public API go
  in `src/internal/`.
- Public classes use the PImpl idiom: a private `Impl` class held in a
  `std::unique_ptr<Impl> _impl`, constructed via
  `explicit X(std::unique_ptr<Impl>)`. Public headers stay free of
  third-party includes — GDAL/OGR types never appear in public headers.
  Copying is disabled; there is no inheritance in public APIs.
- Exported classes and functions are marked `MWG_CORE_API`
  (`mapwingis/core/Api.h`).

## DLL boundary

Exceptions cross the `MapWinGIS.Core.dll` boundary (`CoreError` and its
subclasses). This is safe because every binary in the product is built with
one toolset and one CRT (`build/props/MapWinGIS.Cpp.props` disables warning
4251 for exactly that reason). The ActiveX layer is responsible for
translating these exceptions into the MapWinGIS error codes its consumers
expect.

## Errors

- Operations that can fail throw `CoreError` (or a subclass such as
  `OpenFailed`); see `mapwingis/core/Error.h`.
- Offer a `TryX` companion that returns `nullptr`/`std::optional` instead of
  throwing when probing is a normal use case (e.g. `Open`/`TryOpen`).
- `std::optional` reports absent *values* (null cells, missing doubles), not
  operational failure. Keep the two distinct.

## GDAL lifecycle

- Call `GdalEnvironment::Initialize()` before normal GDAL use. It is
  idempotent and thread-safe, configures deployed runtime data and delegates
  one-time driver registration to `GdalInitializer`.
- `GdalInitializer` registers drivers once and leaves GDAL's process-wide
  driver manager alive until process exit. Use `ScopedGdalConfigOption` for
  temporary process-wide configuration and `GdalErrorScope` to capture
  diagnostics from an operation on the current thread.
- `GdalProgressAdapter` maps GDAL progress callbacks to C++ callbacks and
  checks a thread-safe `GdalCancellationToken`. Re-throw callback exceptions
  with `RethrowCallbackException()` after GDAL returns.
- GDAL and PROJ data (`gdal-data`, `proj.db`) are deployed next to
  `MapWinGIS.Core.dll` by the build (`Directory.Build.targets`,
  `MwgDeployRuntimeData`). Code must point GDAL/PROJ at those locations via
  `GdalEnvironment` — never via `GDAL_DATA`/`PROJ_LIB` environment variables.
- Use `GdalEnvironment::QuietScope` to suppress GDAL diagnostics in tests and
  in code paths where failure is expected.
- Observe GDAL errors through the thread-local `LastErrorMessage()` /
  `SetLogHandler()`. Never write to stderr and never show message boxes from
  the core.
- The supported GDAL version and dependency pin are documented in
  `docs/gdal-version.md`.

## Threading

Core objects are not thread-safe unless their documentation explicitly says
otherwise. Even `const` methods may cache state (see `VectorLayer`). The
pattern is: one dataset object per thread, or serialized access.
`GdalEnvironment` initialization is the exception — safe to call from
anywhere.

## Current state

The tree is early: GDAL environment setup and vector reading/editing
(`VectorDataset`, `VectorLayer`, backed by OGR) are the first slices. Areas
still to be ported (map control, rendering, raster, tiles, ...) get new
`<area>` folders under both `include/mapwingis/` and `src/`.
