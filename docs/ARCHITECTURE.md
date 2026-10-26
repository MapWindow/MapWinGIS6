# Architecture

## Two layers

MapWinGIS6 splits the monolithic MapWinGIS 5 control into two layers:

1. **MapWinGIS.Core.dll** — the mapping engine, a plain C++ DLL. GIS behavior
   and backend-neutral render data live here. No COM, ATL, MFC, windowing, or
   graphics API dependencies are allowed in this layer.
2. **MapWinGIS.ocx** — an ActiveX/COM wrapper. It exposes the intentionally
   implemented subset of the v5 COM API, translates between COM and core
   types, converts core exceptions into COM errors, and integrates the core
   with the host control surface and graphics backend.

Core behavior must be testable through the core alone. OCX-specific
host/rendering integration must also be testable as native components without
COM activation; it must not contain GIS rules or core data ownership.
Generic GDAL algorithms are exposed through the native `MapWinGIS.Core.dll`
API; the COM façade currently exposes control lifecycle and vector loading.

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
- Algorithm runners, compatibility utilities, and pipelines share an execution
  lock. Their callbacks run on the calling thread and cannot re-enter these
  operations. Direct dataset access still needs caller synchronization.
  See [operation results and threading](gdal-modern-capabilities.md#operation-results-logging-and-cancellation)
  for result, cancellation, and callback lifetime contracts.
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
| `MapWinGIS.Core` | Own GIS data, reusable map/layer state, and backend-neutral render data. Keep its public API independent of windowing, COM, and graphics APIs. | COM, ATL, MFC, windowing APIs, OpenGL, OpenCL |
| `MapWinGIS.ocx` | Own COM activation, host-facing control lifecycle, and translation between the external API and core operations. Integrate core rendering with the host surface and selected backend. | GIS rules, core data ownership, rendering-specific types in its public COM contract |
| Rendering coordinator (initially internal to the OCX) | Bind core render requests to the control's target; select a backend and coordinate host-specific drawing and recovery. Keep this native component independent of COM activation. | GIS rules, COM interface layout, backend-specific resource types outside the backend |
| Initial GDI renderer (in the OCX) | Paint backend-neutral polygon scenes into the host drawing context. This is a first visible-map path, not the planned interchangeable backend abstraction. | Core dataset ownership, COM interface layout |
| Future CPU backend (in Core) | Provide the supported rendering path when GPU rendering is unavailable or disabled. | GPU or OpenCL availability |
| OpenGL backend (in the OCX) | Provide optional accelerated drawing and own its context and GPU resources. | Core GIS data ownership and public COM interfaces |
| Future compute work | Host optional compute acceleration only for workloads justified by profiling. | OpenGL being installed or selected |

The OCX is the initial composition root for control and host-specific graphics
context lifetimes. The control owns its core-side map/session object; that
object owns open dataset handles, and each `VectorDataset` owns its
`VectorLayer` objects. Core creates backend-neutral render input from map
state; the coordinator binds it to the current control target and backend.
Backends must not retain pointers or references to dataset-owned objects after
a draw. Core dataset and layer objects are not thread-safe, so the initial
control must serialize their use. Backend-specific contexts, buffers, and
other GPU resources belong to the OpenGL backend and are released when that
backend or its control surface is destroyed. Resizing updates the render
target and viewport without changing ownership of the map data.

Rendering consumes backend-neutral input prepared from core data; neither the
coordinator nor a backend loads datasets or implements GIS rules. Core now
converts Polygon and MultiPolygon features into an owned vector scene. The
initial OCX GDI painter consumes that scene without retaining layer-owned
objects. This vertical slice renders source coordinates directly and does not
reproject data. Scene preparation currently supports polygon layers only.

Backend choice is internal to the rendering coordinator and is observable
through diagnostics, not through a graphics-specific COM API. A caller or
test can request CPU rendering; otherwise the coordinator may try OpenGL when
the target supports it. It must use runtime capability and initialization
results, not GPU vendor or presence alone. If OpenGL cannot initialize or
loses its context, release its resources, record the reason, and select the
CPU backend. Switching backends must not discard map state. The CPU backend
remains usable without a GPU, OpenGL, or OpenCL runtime.

Test core scene preparation and CPU drawing through the core with deterministic
input and output. Test the native OCX coordinator without COM activation by
injecting capabilities, render targets, and backend initialization results;
verify backend selection and failure recovery there. OpenGL-specific
integration checks may require a real context, but they must not be required
for core or CPU-rendering tests.

Design the v6 architecture and APIs for clarity, maintainability, and
performance first; backward compatibility is secondary and must not force the
new version to reproduce legacy structure. At the COM boundary, preserve
selected established behaviors where they fit the supported compatibility
surface, including 0-based feature indexes where required and appropriate error
translation. Before implementing a legacy interface, inspect its type-library
identifiers and signatures; retain identifiers only when the interface is
actually compatible. Full legacy API parity, the historical MFC/ATL class
hierarchy and rendering pipeline, and OpenCL integration are deferred. Document
intentional compatibility gaps and the rationale when interfaces are introduced.

## Current state

The tree is early: GDAL environment setup and vector reading/editing
(`VectorDataset`, `VectorLayer`, backed by OGR) are the first core slices.
The OCX supports COM activation and the ATL resize/paint lifecycle. It can
load the first polygon layer from a vector dataset through Core and display
its geometry with a simple GDI painter. An interchangeable rendering
abstraction, CPU fallback backend, backend selection, and OpenGL remain
unimplemented.
