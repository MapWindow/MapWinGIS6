# Architecture

## Two layers

MapWinGIS6 splits the monolithic MapWinGIS 5 control into two layers:

1. **MapWinGIS.Core.dll** — the mapping engine, a plain C++ DLL. GIS behavior
   and backend-neutral render data live here. No COM, ATL, MFC, windowing, or
   graphics API dependencies are allowed in this layer.
2. **MapWinGIS.ocx** — an ActiveX/COM wrapper. It exposes the intentionally
   implemented v6 automation surface, translates between COM and core
   types, converts core exceptions into COM errors, and integrates the core
   with the host control surface and graphics backend.

Core behavior must be testable through the core alone. OCX-specific
host/rendering integration must also be testable as native components without
COM activation; it must not contain GIS rules or core data ownership.
Generic GDAL algorithms are exposed through the native `MapWinGIS.Core.dll`
API and the ActiveX wrapper's separate `IMapWinGISControlGdal` interface.
The wrapper handles COM translation, worker dispatch, progress, cancellation,
and operation results; GDAL execution and dataset ownership remain in Core.
The GDAL automation interface is an independent, early-bound COM capability.
Its `SAFEARRAY(BSTR)` arguments and 64-bit (`hyper`) operation IDs are intended
for clients that consume its type information; it is not a late-bound scripting
contract. The primary control interface remains the default `IDispatch` surface.
The control persists its rendering backend preference through streams and
property bags. A seekable legacy stream containing only the ATL version is
accepted as an empty property stream.

The ActiveX GDAL event source is suspended while its control window is absent
and resumes dispatch when that window is recreated. `IOleObject::Close` stops
and cancels the current operation lifetime; a new non-null client site or a
recreated control window starts a fresh operation lifetime. A closed control
can therefore be reactivated and used again. Diagnostic strings are marshalled
lossily: malformed UTF-8 and embedded NUL bytes become U+FFFD so invalid
messages cannot prevent release of a completed operation. Its retry timer is
created on the owner thread only while operation slots are outstanding and is
stopped as soon as the last slot is ended.

## Module design

- GDAL execution support, including `GdalAlgorithmRunner`, is implemented in
  `MapWinGIS.Core.dll`. The runner configures algorithms through GDAL's
  registry, returns structured diagnostics, and transfers dataset ownership
  with `GdalDatasetPtr`.
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

- Operations that can fail throw `CoreError` or a subclass such as
  `OpenFailed` or `InvalidArgument`. See `mapwingis/core/Error.h`.
- Offer a `TryX` companion that returns `nullptr`/`std::optional` instead of
  throwing when probing is a normal use case (e.g. `Open`/`TryOpen`).
- `std::optional` reports absent *values* (null cells, missing doubles), not
  operational failure. Keep the two distinct.
- Long-running GDAL algorithm operations deliberately report ordinary
  execution failure through a boolean result and `GdalOperationResult`, which
  preserves diagnostics and cancellation state. Rejected algorithm arguments
  and compatibility utility arguments can also produce failure diagnostics.
  Configuration helpers and typed conversions throw `InvalidArgument` for
  invalid values that they check. Callback re-entry throws `CoreError`.
  Progress callback exceptions become failure diagnostics.
  Diagnostic-capture and log-handler exceptions propagate after GDAL returns.

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
- Public GDAL headers separate configuration (`GdalConfig.h`), diagnostics
  (`GdalDiagnostics.h`), progress (`GdalProgress.h`), and operation results
  (`GdalOperationResult.h`). The native progress callback ABI is internal.
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
  lock. GDAL can report progress on worker threads. The shared progress adapter
  serializes callbacks, which cannot re-enter these operations.
  Direct dataset access still needs caller synchronization.
  Releasing a dataset reference or destroying a runner never takes the
  execution lock, so release cannot block behind or deadlock with an
  in-flight operation; the released dataset must not be in active use by
  another thread.
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

## Map model

`mwg::Map` (`mapwingis/map/Map.h`) is the authoritative owner of the layer
collection and map-level layer state. It has no windowing, COM, or rendering
state; hosts and renderers read from it.

- **Identity.** `Map::AddLayer` returns a `LayerId`. Identities start at 1,
  increase, and are never reused by the same map, even after `RemoveLayer` or
  `Clear`. `LayerId{}` (0) identifies no layer. Neither display names nor
  positions identify a layer.
- **Ordering.** Position 0 is the bottom layer and is drawn first; new layers
  are appended on top. Positions shift on removal; identities do not.
- **Ownership.** The map owns its `MapLayer` objects. A `MapLayer` reference
  stays valid until that layer is removed, the map is cleared, or the map is
  destroyed; changes to other layers do not invalidate it. Each `MapLayer`
  shares ownership of its `VectorDataset` (`std::shared_ptr`), so the dataset
  lives as long as any map layer or other owner uses it.
- **Duplicates.** The same source layer may be added more than once; each
  addition is an independent map layer with its own identity and visibility.
- **Visibility.** Layers are visible when added. `Map::SetLayerVisible`
  changes visibility. Hidden layers are excluded from
  `GetExtent(ExtentScope::VisibleLayers)`, the default and the extent used for
  zoom-to-map, and included in `GetExtent(ExtentScope::AllLayers)`.
- **Extents.** Map extents are the union of valid layer extents
  (`Extent::IsValid`: non-empty and finite). They are computed on each call
  rather than cached, so visibility changes, added or removed layers, and
  source edits are reflected without invalidation. A map with no valid layer
  extent reports an empty `Extent`.
- **Projection.** `mwg::Projection` is the shared immutable CRS value. It
  imports EPSG, WKT, and PROJ4 definitions, prefers EPSG identity, and provides
  stateless coordinate transformations in traditional GIS axis order.
  `VectorLayer::Projection()` reads the layer CRS; WKT export remains available
  from the value type rather than through a parallel string property.
  For RFC 7946 GeoJSON without a `crs` member, GDAL supplies WGS 84 (EPSG:4326).
  A layer with no defined CRS returns an empty projection.
- **Map projection.** When `Map` has no projection, adding the first layer with
  a readable CRS sets the map projection. If the map cannot read the layer's
  CRS, it logs a warning and still adds the layer. A host can call
  `SetProjection` or `ClearProjection` explicitly. Removing layers and
  `Clear()` do not change the map projection.
- **Coordinates.** Layers are assumed to share compatible source coordinates.
  Phase 1 records CRS state but does not reproject; layers in different
  coordinate reference systems do not align until display-time coordinate
  transformations are implemented.

Vector layer extent queries cache successful results. Read failures throw
without caching an empty result. If a driver cannot provide an extent and
reports no failure diagnostic, Core scans the geometries with read-error checks.
The scan asks the driver to ignore attributes.
Attribute edits keep the cached extent. After geometry edits, the next extent
query scans once and caches the result.

Call `VectorDataset::Flush()` before closing after geometry edits to persist
exact Shapefile and GeoPackage bounds. The flush runs one O(n) stored-extent
scan per dirty layer, rather than one per edit. A recomputation failure throws
without rolling back geometry writes and leaves the layer dirty for retry.
Other formats retain their driver's stored-extent behavior.

## Viewport

`mwg::Viewport` (`mapwingis/map/Viewport.h`) is the view of map coordinates on
a pixel area. It converts between map and screen coordinates and fits an
extent, typically `Map::GetExtent()`, into the display area. It does not
reference a `Map` and holds no windowing, COM, or rendering state. It is a
small copyable value type, like `Extent`, so renderers and navigation code can
take snapshots.

- **Coordinates.** Map X increases right and map Y increases up. Screen X
  increases right and screen Y increases down, in pixels from the top-left
  corner. Screen coordinates are continuous `double` values: pixel `(i, j)`
  covers `[i, i + 1) x [j, j + 1)`. `ToPixel` rounds down to the containing
  pixel; round only there.
- **State.** A size in pixels plus, once set, a view: the map point at the
  viewport center and one resolution (map units per pixel) for both axes. The
  visible extent is derived from these, so it always has the viewport's
  aspect ratio and geometry is never distorted. Resizing keeps the center and
  resolution.
- **Validity.** A viewport is valid when both dimensions are positive and it
  has a view. On an invalid viewport, transformations return `std::nullopt`
  and `VisibleExtent` returns an empty `Extent`. A zero-sized viewport keeps
  its view. Non-finite inputs or results give `std::nullopt`; negative sizes,
  non-finite views, and invalid padding throw `InvalidArgument`.
- **Fitting.** `FitExtent` centers the extent and picks the smallest
  resolution that shows it on both axes, so the other axis expands. Padding
  is kept free on every side. Extent edges map to the centers of the
  outermost pixels inside the padding, so pixel rounding never pushes
  boundary geometry outside the viewport. If padding does not fit, the extent
  spans half a pixel around the center instead. A zero-span axis does not constrain
  the resolution. A single point keeps the current resolution, or uses
  `kDefaultPointResolution` when no view exists. Invalid
  extents and zero-sized viewports return `false` without changing the view.
- **Limits.** Sizes are limited to `kMaxDimension` pixels. Centers are clamped
  to `±kMaxCoordinate` and resolutions to `[MinResolution(center),
  kMaxResolution]`, where the minimum is relative to the center's magnitude so
  that pixels stay many representable doubles wide.
- **Navigation.** `Pan` moves the center in map units. `Zoom` divides the
  resolution by its factor, and `ZoomOut` multiplies it directly. `ZoomAt`
  keeps the map coordinate under a screen anchor unless the limits intervene.
  Moving the center can raise `MinResolution` and change the resolution
  again, so the anchored coordinate can move even without center clamping.
  `ZoomAt` needs a valid viewport. Center zoom and pan only need a view.

## Map view and multi-layer rendering

`mwg::MapView` (`mapwingis/map/MapView.h`) owns one `Map`, the one `Viewport`
all its layers are drawn through, and the render scene prepared from the map.
It is the only map and view state: the ActiveX control owns a `MapView` and
has no extent or transform of its own.

- **Render scene.** `rendering::CreateMapRenderScene`
  (`mapwingis/rendering/MapRenderScene.h`) copies the polygon geometry of
  every visible layer into an immutable, shared per-layer scene in a
  `MapRenderScene`, bottom layer first. Each
  layer is converted on its own: hidden layers, layers without polygon
  geometry, and layers that fail (for example, non-polygon layers) are listed
  in `skippedLayers` with a reason and do not stop the other layers.
  This includes `std::exception` errors except `std::bad_alloc`, which propagates.
  Exceptions outside that hierarchy also propagate. The scene
  holds no view state and no references to the map or its datasets. The
  lower-level scene options can apply an OGR spatial filter so callers load
  only feature envelopes that intersect a requested extent.
- **Shared viewport.** Renderers draw the scene through the `Viewport` passed
  with it, so every layer uses the same transformation. `MapView` builds
  automatically prepared layer scenes from the visible extent plus a
  64-pixel margin. Small pans and zoom-in operations therefore retain the
  immutable scene and backend mesh caches. A resize or navigation operation
  that moves outside the cached processing extent rebuilds only the affected
  layer scenes; prepared scenes supplied by a caller are complete snapshots
  and remain reusable for every view.
- **Redraws.** Map changes go through `MapView` (`AddLayer`, `RemoveLayer`,
  `ClearLayers`, `SetLayerVisible`), which marks the scene stale; `Scene()`
  assembles it from cached layer scenes on the next call.
  The `AddLayer` overload that accepts a prepared
  vector scene transfers that geometry into the current scene without another
  traversal. Its layer cache and the assembled scene share that geometry
  instead of copying it. Existing scene storage stays unchanged when it is current.
  It assembles a stale scene before adding the new layer.
  Every mutator returns whether the frame may
  have changed, and the host requests a redraw when it does. Edits to source
  geometry are not observed. Call `InvalidateLayerScene` for each affected
  map layer, or `InvalidateScene` for all layers. Failed layers keep their
  diagnostics until invalidated. Duplicate map layers have separate caches,
  even when they share a source.
- **Initial view and resizing.** While the viewport has no view, adding a
  layer fits the view to the visible map extent. `Resize` keeps the center
  and resolution, so resizing neither moves nor rescales the map. A fit
  requested while the viewport has no size, for example before the host
  window exists or while it is minimized, is done by the first `Resize` that
  gives it a size.
  If reading the pending extent throws, `Resize` propagates the error after
  applying the size and discarding the pending fit. The center and resolution
  stay unchanged. The host must request a redraw after this error.
- **Navigation.** `FitToMap` (zoom to map: visible layers only, with
  padding), `ZoomToLayer` (by `LayerId`, also for hidden layers), `ZoomIn`
  and `ZoomOut` (center zoom by a factor above 1, default
  `kDefaultZoomFactor`), `ZoomAt`, and `Pan` change the shared viewport and
  return whether a redraw is needed. Empty maps, missing layers, and layers
  without a valid extent return `false` and keep the view. The latest request
  wins: a new fit replaces a pending one, and a navigation step that changes
  the view cancels it. A missing layer or an invalid extent also cancels a
  pending fit. Navigation that returns `false` keeps it, including `ZoomAt`
  on a zero-sized viewport and center zoom at a resolution limit.
  None of this depends on COM, mouse messages, or a renderer. The ActiveX
  control maps left-button drags and cursor-anchored wheel input onto these
  calls.

## ActiveX and rendering architecture

The first ActiveX and rendering vertical slice is implemented across the
existing Core and ActiveX projects; the boundaries below describe current
responsibilities, not a requirement to split them into more DLLs.

| Component | Responsibility | Must not depend on |
|---|---|---|
| `MapWinGIS.Core` | Own GIS data, reusable map/layer state, and backend-neutral render data. Keep its public API independent of windowing, COM, and graphics APIs. | COM, ATL, MFC, windowing APIs, OpenGL, OpenCL |
| `MapWinGIS.ocx` | Own COM activation, host-facing control lifecycle, and translation between the external API and core operations. Integrate core rendering with the host surface and selected backend. | GIS rules, core data ownership, rendering-specific types in its public COM contract |
| Rendering coordinator (in the OCX) | Try OpenGL for supported scenes and use GDI when OpenGL is unavailable or the scene is unsupported. Keep backend selection independent of COM activation. | GIS rules, COM interface layout, backend-specific resource types outside the backend |
| GDI backend (in the OCX) | Paint supported polygon scenes, including interior rings, into a device context; provide the Windows CPU fallback. | Core dataset ownership, COM interface layout, GPU availability |
| OpenGL backend (in the OCX) | Fill polygons from their Core triangulated meshes, outline the original rings, and copy pixels to the host device context through a reusable WGL context. | Core GIS data ownership and public COM interfaces |
| Future compute work | Host optional compute acceleration only for workloads justified by profiling. | OpenGL being installed or selected |

The OCX is the composition root for control, render-coordinator, and
host-specific graphics-context lifetimes. The control owns a `MapView`, which
owns the map, its layers' datasets (shared), the viewport, and the prepared
`MapRenderScene`; each `VectorDataset` owns its `VectorLayer` objects. Core
creates backend-neutral render input; the coordinator binds it to the control
target and backend. Backends must not retain references to dataset-owned
objects after a draw, and they keep no view state. Core dataset and layer
objects are not thread-safe, so the control must serialize their use.
The OpenGL backend owns its WGL context and reusable pixel buffers; those
resources are released with the control. `WM_SIZE` resizes the `MapView`
viewport. For windowless hosts, `SetObjectRects` supplies the live size.
`OnDraw` uses a viewport copy sized to the drawing target. Printing, metafile
output, and scaled draws do not change the live viewport or its pending fit,
and use temporary frame/staging buffers rather than resizing the interactive
screen buffers. When the target size differs from the live size, the copy is
fitted to the live visible extent, so the target shows the same map area as
the screen at the target's own scale.
If no view exists yet, drawing fits only that copy to the visible map extent.
Resizing updates the OpenGL viewport and staging buffers without changing
ownership of map data. A left-button drag pans the shared `MapView` by the
pixel delta converted with the viewport's map-units-per-pixel resolution. The
cached frame is blitted only for whole-pixel offsets (within a small
floating-point epsilon); fractional offsets and shifts at least as large as
the frame trigger a full render.
Wheel input calls `MapView::ZoomAt` at the cursor. The control acquires and
releases mouse capture through either its window or its windowless in-place
site, and ends a drag on capture loss, cancellation, close, or destruction.
Automatically built `MapView` scenes use a viewport-relative processing margin:
small view changes reuse the scene, and a larger change rebuilds only affected
layer selections. The ActiveX control currently commits caller-prepared complete
scenes, so its view changes continue to reuse the complete snapshot and do not
request filtered scenes. Alternate-size draws use the live scene; filtered
direct-host scenes are therefore valid only through the live viewport. Hosts
that draw a different viewport must supply complete prepared scenes.

Rendering consumes backend-neutral input prepared from core data; neither the
coordinator nor a backend loads datasets or implements GIS rules. For each
visible map layer, Core sequentially reads Polygon, MultiPolygon,
CurvePolygon, and MultiSurface features into an owned vector scene,
linearizing curved geometries. Scene creation preserves source rings and
rendering metadata without triangulating polygons. A caller can explicitly
request fill-mesh preparation when a fill operation needs indexed,
backend-neutral triangles; a polygon that cannot be triangulated keeps its
rings and has no mesh. `ViewTransform` captures the `Viewport` once
per frame so both backends transform every vertex of every layer with the
same arithmetic as `Viewport::MapToScreen`; GDI draws a screen position in
the pixel that contains it (`ToPixel`). One backend draws the whole frame.
Triangulation uses the internal earcut.hpp dependency first; an area-mismatch
or rejected polygon falls back to GEOS constrained Delaunay triangulation and,
if that also fails, retains the source rings for GDI. Earcut consumes the ring
coordinates without constructing OGR geometries and does not perform the
previous `IsValid` precheck. Large mesh jobs are split across at most four
workers.
The generated meshes remain associated with that immutable scene and are
reused by later draws; they do not mutate the Core scene. OpenGL outline
mode draws the source rings without requiring fill meshes. With the
`Automatic` preference, the coordinator instead paints a Fill-mode frame
whose layers have no fill meshes yet with the GDI backend, so the paint
path never blocks on triangulation. The control queues mesh generation for
the immutable scene on its owner thread. A background coordinator reads only
the scene snapshot, and a posted message publishes completed meshes on the
control thread and invalidates the layer frame. Draw never publishes them in
this mode. Posted messages are dispatched before `WM_PAINT`, so when the
completion arrives before the first frame of a new or changed layer set has
been presented, the control defers publication and warm-up until that frame
has been drawn. The next frame can switch to
OpenGL. Replaced, removed, and explicitly invalidated scene identities cancel
their queued tokens, so stale results are never published. An explicit
`OpenGL` preference still supports synchronous lazy preparation when a caller
draws a mesh-free scene directly. GDI checks each
layer before drawing it; a layer with non-finite or out-of-range device coordinates is
skipped and reported (`GetFailedLayers`, `RenderingStatus`) while the other
layers are still drawn.
OpenGL uses a context created on a hidden, never-presented window. When OpenGL
buffer objects are available (OpenGL 1.5 or `GL_ARB_vertex_buffer_object`), it
uploads each layer's immutable, scene-localized fill and outline coordinates
once to a reusable vertex buffer and indices to an element buffer. Pan and zoom
change the fixed-function model-view transform without rebuilding the source
coordinates. The client-side transformed-array path remains as a compatibility
fallback. When background fill meshes become ready, the control initializes
the context, staging buffer, and framebuffer for its current viewport before
invalidating the GDI frame. The following OpenGL draw then reuses those target
resources instead of performing their one-time allocation on the paint path.
Drawing uses a framebuffer object (OpenGL 3.0 or
`GL_EXT_framebuffer_object`) and reads pixels back in bottom-up BGRA order into
a CPU buffer before copying them to the host device context.
BGRA stores blue, green, red, and alpha bytes in that order.
The screen renderer retains its staging allocation and GPU framebuffer.
Alternate-size draws use a temporary OpenGL renderer that releases both after
the draw and shares the screen renderer's lazy fill meshes, leaving the screen
CPU, framebuffer, and geometry-buffer resources unchanged. Temporary renderers
use their own context-local GPU geometry buffers because OpenGL buffer names are
not portable between contexts.
Rendering into a framebuffer object avoids undefined pixels from window
pixel-ownership rules. `SetDIBitsToDevice` consumes the bottom-up OpenGL scanlines
without a second CPU row-swap pass, so presentation still requires one readback
but not an additional full-frame copy.
Interior rings and concave boundaries draw identically in both backends
because both consume the same scene. A failed lazy triangulation, an unusable
explicitly prepared mesh, missing framebuffer-object support, initialization
failures, and drawing failures use the GDI backend. GDI fills and outlines
the original rings, so a self-intersecting ring that GEOS cannot triangulate
can still be displayed.
The GDI backend draws the original rings of the whole supported polygon scene,
including its interior rings and concave boundaries. Both backends draw source coordinates without reprojection,
and scene preparation currently supports Polygon, MultiPolygon, CurvePolygon,
and MultiSurface features.

The `RenderingBackendPreference` automation property is configurable as
`Automatic`, `OpenGL`, or `GDI`. `Automatic` paints with GDI while fill
meshes are pending and otherwise tries the OpenGL backend first; it falls
back to GDI when the scene, hardware, allocation, or drawing operation is
unsupported. `OpenGL` always tries OpenGL first, triangulating lazily on
the calling thread when a fill mesh is missing. The control reports the
active backend and the reason for a fallback through `RenderingBackend` and
`RenderingStatus`.
These rendering properties are on `IMapWinGISControl2`, which derives from
`IMapWinGISControl`. `IMapWinGISControl3` adds `ClearLayers` at DISPID 8.
`IMapWinGISControl4` derives from `IMapWinGISControl3` and adds map navigation,
layer count, and stable layer IDs at DISPIDs 9-17. The coclass default is
`IMapWinGISControl5`, which derives from `IMapWinGISControl4` and adds
asynchronous vector loading at DISPIDs 18-20. The earlier interfaces, their
DISPIDs, and their IIDs remain unchanged.
OpenGL capability detection checks framebuffer-object entry points, the
maximum renderbuffer size, and the maximum viewport dimensions at runtime;
targets exceeding either limit use GDI. CPU staging for OpenGL readback is
limited to one 64 MiB buffer per frame, including retained capacity and
reallocation peaks; larger targets use GDI instead. A failed rendering
context is released once and its replacement is attempted on the next draw.
If initialization fails, OpenGL skips 60 draws before retrying.
A target-size change triggers an immediate retry.
GDI renders frames during this delay. Switching backends does not discard map state. The GDI backend
remains usable without a GPU or OpenGL runtime; Core itself has no
graphics-runtime dependency.

Test Core scene preparation and the renderer coordinator independently of COM
activation. OpenGL-specific integration checks need a usable WGL context;
GDI fallback and coordinator tests do not require a GPU. The registration-free
C# host exercises COM integration and samples the host window's rendered
pixels; it requires an interactive Windows desktop.

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

## Current limits and next work

The control exposes a small v6 COM surface: Core initialization, vector
dataset loading, layer-loaded state, feature count, renderer preference,
active backend and status, plus asynchronous vector loading and generic GDAL
operations. The GDAL API has a new interface and does not claim MapWinGIS 5
`IGdalUtils` binary compatibility. Both asynchronous capabilities share one
dispatcher and operation limit: at most sixteen operations may be active or
awaiting result cleanup. `GdalOperationCompleted` signals either kind; call
`EndGdalOperation` or `EndLoadVectorDataset`, respectively, to retrieve the
result and release its slot. Completion is delivered on the owning apartment
thread. For vector loads, the control commits the prepared dataset and scene
before raising the completion event. Cancellation discards the candidate and
leaves the live map unchanged. Destroying the in-place window suspends
notification delivery without cancelling operations; a subsequent `WM_SIZE`
resumes delivery. Calling `Close` or destroying the control after the client's
final COM reference is released cancels outstanding work and discards queued
notifications. An in-progress event dispatch holds a temporary COM reference,
so releasing the client's reference inside an event does not destroy the
control until dispatch returns. Workers never retain COM references or wait for
event handlers.

`LoadVectorDataset` adds the first vector layer of a dataset on top of the
map. Earlier layers stay loaded, so calling it again shows several layers.
This includes repeated loads of the same dataset. `LoadVectorDatasetEx`
returns the stable ID of the new layer, and `GetLayerId` returns a layer ID by
its current 0-based position. Call `ClearLayers` before switching or refreshing
datasets to avoid duplicate layers. It releases all loaded datasets and clears
the feature total, but keeps the view.
`IMapWinGISControl5::BeginLoadVectorDataset` performs the same preparation on
the GDAL worker and returns an operation ID. It uses the existing
`GdalOperationCompleted` event; clients retrieve the result and new layer ID
with `EndLoadVectorDataset`. `CancelLoadVectorDataset` does not block. If
cancellation is accepted before the completion event is delivered, the
candidate is discarded and the map is unchanged. The original synchronous
`LoadVectorDataset` and `LoadVectorDatasetEx` methods keep their behavior.
The layer must be polygon-compatible: a dataset whose first layer has another
geometry type fails to load and is not added, even if a later layer is
polygonal. An empty polygon layer loads but draws nothing. The control reads
and validates the candidate layer before adding it, so a rejected load cannot
change the live viewport or schedule a fit. The control transfers the validated
scene into `MapView`. Loading another layer and drawing the first frame reuse
the prepared geometry of existing layers.
`LayerLoaded` reports whether the map has any layer, `LayerCount` reports its
current layer total, and `FeatureCount` is the cached total over all loaded
layers. The control updates the total after a successful load and resets it on
`ClearLayers`. Scene preparation counts
all source features, including null and empty geometries, during its geometry
traversal. Drivers that report an unknown count can still load. The native
`VectorLayer::FeatureCount` also returns an exact count. If the driver cannot
provide one, it builds the feature ID index and reports read failures.
COM property reads do not scan datasets or rebuild the scene. `FitToMap`,
`ZoomToLayer`, `ZoomIn`, `ZoomOut`, `ZoomAt`, and `Pan` forward navigation to
the control's one `MapView` and request a redraw only when its view changes.
Each successful load still fits the view to the combined map extent. Raster
layers and other geometry types are not rendered. There is no reprojection, so
coordinates remain in the source coordinate system and layers are assumed to
share one.

For direct `CreateVectorRenderScene` and `CreateMapRenderScene` calls, Core
prepares and retains the complete supported polygon scene in memory. `MapView`
instead applies a padded visible-extent spatial filter to
automatically prepared layers when the driver reports
`OLCFastSpatialFilter`, while a caller-supplied prepared scene remains
complete. Drivers without a native fast filter retain complete scenes so a pan
does not repeatedly force a full-layer scan.
Core reads feature geometries sequentially without WKT serialization and
reparsing. MapView caches one filtered or complete scene per layer: visibility
changes reassemble the map snapshot without rereading visible layers, hidden
layers are prepared when first shown, and removing or clearing layers drops
only the corresponding cache entries. InvalidateLayerScene rebuilds one
layer after source edits; InvalidateScene clears all layer caches. Adding a
prepared layer seeds its cache directly. Layer scenes are immutable and shared through
`std::shared_ptr<const VectorRenderScene>`, so assembling the map snapshot
shares cached coordinates and meshes instead of copying them. An automatically
built layer holds its selected geometry once, and a prepared layer holds its
complete geometry once. Rebuilding a selection temporarily retains both the old
scene and the new selection until the assembled snapshot is replaced.
OpenGL's generated fill meshes have a second, backend-owned cache keyed
by stable `LayerId` and the immutable scene identity. Reordering, hiding,
redrawing, viewport changes, and backend selection retain valid entries.
Replacing one layer scene invalidates only that layer's entry; removed layers
and explicit invalidation cancel pending work and release their generated mesh
storage. Pending, ready, and failed states remain distinct in the cache. A
parallel context-owned GPU buffer cache uses the same layer and scene identity. Scene
replacement, context recreation, and coordinator invalidation release or queue
the affected vertex and index buffers; viewport changes do not.
When a driver cannot provide a fast extent, scene preparation computes bounds
during the geometry traversal. It caches those bounds on the vector layer,
so a later map fit does not scan the layer again.

`VectorRenderScene::polygons` uses `PolygonBuffer`: one coordinate buffer,
one array of ring ranges, and one array of polygon ranges. Each range stores
an offset and count. `PolygonView` borrows rings as read-only spans without
allocating coordinate vectors. A buffer copy owns separate storage.
Appending, assigning, or destroying a buffer invalidates its views.
Scene preparation reuses temporary coordinate and ring-offset arrays for
each polygon. Both backends read the flat buffers directly. Triangulation
accepts a view without reconstructing nested vectors. Standalone `Polygon`
and `Ring` types remain available for caller-created triangulation input.

There is no tiling or paging. GDI skips a layer whose device coordinates
exceed its range. It clips drawing to the requested target rectangle and the
host's existing clip region, then restores the device context. Fill
triangulation runs when scene preparation explicitly requests it, during
background preparation for the control, or when an explicit OpenGL draw first
needs a missing mesh. View changes inside the scene's processing
margin and redraws reuse retained meshes; changes outside the margin replace
the affected scene and its backend mesh entry. The opt-in
rendering baseline (see [TESTING.md](TESTING.md#rendering-performance-baseline))
tracks scene preparation, mesh storage, and backend frame costs on both a
convex grid and a complex mix of concave, holed multipart, and many-vertex
polygons. OpenGL fills every polygon from a supplied or lazily generated
triangle mesh and copies each frame through CPU memory because the ActiveX
offscreen-DIB composition contract still presents through GDI. Its reusable CPU
staging buffer is limited to 64 MiB per frame; target dimensions are also
limited by the detected framebuffer and viewport capabilities. GDI is the supported
Windows CPU fallback; Core has no independent platform-neutral renderer.

The C# host's pixel assertions, activation, and native tests pass for Debug
and Release on Win32 and x64 in an interactive desktop session.
Core navigation on the shared `MapView` (zoom to map and layer, zoom in and
out, anchored zoom, and pan) is exposed through `IMapWinGISControl4`; the
ActiveX control also supports left-button drag panning and cursor-anchored
wheel zoom. Deep zoom can push a layer past the GDI coordinate range until
clipping exists. Measure scene preparation with representative datasets
before adding caching, paging, or broader OpenGL geometry support.
OpenCL and full MapWinGIS 5 API parity remain deferred.
