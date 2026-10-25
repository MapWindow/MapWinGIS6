# Architecture

## Two layers

MapWinGIS6 splits the monolithic MapWinGIS 5 control into two layers:

1. **MapWinGIS.Core.dll** — the mapping engine, a plain C++ DLL. All logic
   lives here. No COM, ATL or MFC anywhere in this layer.
2. **MapWinGIS.ocx** — a thin ActiveX/COM wrapper (not started yet). Its only
   jobs are to expose the v5 COM API, translate between COM and core types,
   and convert core exceptions into MapWinGIS error codes.

Everything that can be tested must be testable through the core alone.
Until the ActiveX wrapper exists, generic GDAL algorithms are exposed through
the native `MapWinGIS.Core.dll` API; the COM façade is not yet available.

## Module design

- `MwGdalCore` is a native static library linked into `MapWinGIS.Core.dll`.
  It contains GDAL execution support, including `GdalAlgorithmRunner`, and
  has no COM, ATL or MFC dependency. The runner configures algorithms through
  GDAL's registry, returns structured diagnostics, and transfers dataset
  ownership with `GdalDatasetPtr`.
- Public API lives in `src/MapWinGIS.Core/include/mapwingis/<area>/`, grouped
  by area (`core`, `gdal`, `vector`, ...). All of it is in namespace `mwg`.
- Implementation lives in `src/MapWinGIS.Core/src/<area>/`, mirroring the
  public header names. Shared internals that are not part of the public API go
  in `src/internal/`.
- Public classes use the PImpl idiom: a private `Impl` class held in a
  `std::unique_ptr<Impl> _impl`, constructed via
  `explicit X(std::unique_ptr<Impl>)`. Public headers stay free of
  third-party includes. Public API may forward-declare a GDAL type when needed
  to transfer ownership without exposing GDAL headers.
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

## ActiveX and rendering architecture

This section records the design baseline for the future ActiveX control and
rendering path. It describes intended boundaries, not implemented
functionality. The first implementation should remain a small vertical slice;
these responsibilities do not require separate DLLs.

| Component | Responsibility | Must not depend on |
|---|---|---|
| `MapWinGIS.Core` | Own GIS data access and reusable map/layer state. Keep its public API independent of windowing, COM, and graphics APIs. | COM, ATL, MFC, OpenGL, OpenCL |
| `MapWinGIS.ocx` | Own COM activation, host-facing control lifecycle, and translation between the external API and core operations. | Rendering-specific types in its public COM contract |
| Rendering coordinator (initially internal to the OCX) | Turn the current map view into backend-neutral render input, select a backend, and coordinate drawing and recovery. | COM interface layout and backend-specific resource types outside the backend |
| CPU backend | Provide the supported rendering path when GPU rendering is unavailable or disabled. | GPU or OpenCL availability |
| OpenGL backend | Provide optional accelerated drawing and own its context and GPU resources. | Core GIS data ownership and public COM interfaces |
| Future compute work | Host optional compute acceleration only for workloads justified by profiling. | OpenGL being installed or selected |

The OCX is the initial composition root for control, rendering, and graphics
context lifetimes. The control owns its core-side map/session object; that
object owns open dataset handles, and each `VectorDataset` owns its
`VectorLayer` objects. A render operation borrows the necessary map inputs
only for that operation. Backends must not retain pointers or references to
dataset-owned objects after a draw. Core dataset and layer objects are not
thread-safe, so the initial control must serialize their use. Backend-specific
contexts, buffers, and other GPU resources belong to the OpenGL backend and
are released when that backend or its control surface is destroyed. Resizing
updates the render target and viewport without changing ownership of the map
data.

Rendering consumes backend-neutral input prepared from core data; it does not
load datasets or implement GIS rules itself. The existing vector API exposes
feature geometry as WKT, but the core does not yet provide a map scene or
render-ready geometry. A later rendering slice must choose the smallest
appropriate conversion path without adding graphics dependencies to the core
or retaining `VectorLayer` internals in a backend.

Backend choice is internal to the rendering coordinator and is observable
through diagnostics, not through a graphics-specific COM API. A caller or
test can request CPU rendering; otherwise the coordinator may try OpenGL when
the target supports it. It must use runtime capability and initialization
results, not GPU vendor or presence alone. If OpenGL cannot initialize or
loses its context, release its resources, record the reason, and select the
CPU backend. Switching backends must not discard map state. The CPU backend
remains usable without a GPU, OpenGL, or OpenCL runtime.

The boundary must be testable without a GPU: test backend selection and
failure recovery with injected capabilities and initialization results, and
test CPU drawing with deterministic input and output. OpenGL-specific
integration checks may require a real context, but they must not be required
for core or CPU-rendering tests.

At the COM boundary, preserve established MapWinGIS behavior where it is
intentionally implemented, including the existing 0-based feature-index
contract and error translation. Before implementing a legacy interface,
inspect its type-library identifiers and signatures; retain identifiers only
for compatible interfaces. Full legacy API parity, the historical MFC/ATL
class hierarchy and rendering pipeline, and OpenCL integration are deferred.
Document intentional compatibility gaps as the corresponding interfaces are
introduced.

## Current state

The tree is early: GDAL environment setup and vector reading/editing
(`VectorDataset`, `VectorLayer`, backed by OGR) are the first slices. Areas
still to be ported (map control, rendering, raster, tiles, ...) get new
`<area>` folders under both `include/mapwingis/` and `src/`. The ActiveX and
rendering components described above are architectural plans, not implemented
or verified capabilities.
