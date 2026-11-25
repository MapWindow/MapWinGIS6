# ActiveX architecture

The OCX is the COM and host-integration boundary around Core. Its control owns
one `mwg::MapView`; it forwards host size and drawing work to that view and
passes the resulting scene to the renderer. Dataset ownership and map state
remain in Core.

## Automation contracts

- `IMapWinGISControl` through `IMapWinGISControl5` are versioned derived
  interfaces with distinct IIDs.
  Do not add vtable members to a published interface or change its IID.
  Extend the contract with a new derived interface when needed.
- The default control interface is `IMapWinGISControl5`. It adds asynchronous
  vector loading to `IMapWinGISControl4`; the independent generic GDAL
  capability uses `IMapWinGISControlGdal`, and both operations use the
  separate `IMapWinGISControlEvents` dispinterface.
- These are new v6 interfaces, not the MapWinGIS 5 type-library contracts.
  The current control does not claim v5 binary compatibility.
- Keep COM conversions and COM error translation at this boundary. Core
  exceptions are translated for supported automation calls.

## Rendering and lifetime

The ActiveX rendering coordinator selects a backend for a Core render scene.
GDI is the CPU fallback; OpenGL has a narrower supported-scene range and can
fall back to GDI. With the `Automatic` preference, a Fill-mode frame whose
layers have no fill meshes yet is painted with GDI (reported via
`RenderingBackend`/`RenderingStatus`) so the paint path never triangulates;
OpenGL is retried on later frames once meshes exist. Nothing prepares those
meshes during `Automatic` drawing today (Phase 2 adds background
preparation), so default Fill stays on GDI until an explicit `OpenGL` draw
caches the meshes or a scene supplies them. Only the explicit
`OpenGL` preference triangulates lazily on the drawing thread. Backends consume Core-produced geometry and do not own
datasets or view state. The OCX owns the host-specific rendering resources.
The control composes ordinary screen frames in an offscreen DIB-section
`FrameBuffer` and presents them with a single `BitBlt`; alternate-size
printing/scaled draws use temporary buffers. During a drag, the cached layer
frame is reused only for whole-pixel offsets; fractional or out-of-frame shifts
fall back to rendering. `WM_ERASEBKGND` is suppressed because `OnDraw` paints
an opaque frame.

Alternate-size draws use a temporary OpenGL renderer, preserving both the
screen CPU staging allocation and its GPU framebuffer. Clearing the screen
staging vector for a temporary draw loses that allocation even when the next
screen frame uses a cached blit. After a lost context is released, failed
replacement initialization must retain its failure state for the 60-draw
retry delay. A size change bypasses the delay.
See `OpenGlVectorLayerRenderer::Draw` and the renderer/control regression tests.

The OpenGL lazy fill-mesh cache belongs to the renderer and is shared with
temporary renderers. It is keyed by `LayerId` and the immutable scene identity:
reordering, hiding, viewport changes, and backend selection preserve entries,
while a changed scene for one layer invalidates only that entry. The coordinator
prunes removed IDs before every backend choice, and explicit layer or scene
invalidation releases the retained CPU mesh storage.

The screen OpenGL renderer also caches context-local vertex and index buffers
for immutable layer geometry when OpenGL 1.5 or
`GL_ARB_vertex_buffer_object` is available. Stored vertices are localized to
one origin for the layer, so pan and zoom change a model-view transform without
reuploading coordinates. Scene changes and explicit invalidation release the
affected buffers; the client-array path remains the fallback. Temporary
renderers share CPU fill meshes but own separate GPU buffers because buffer
names belong to their OpenGL contexts. A framebuffer readback remains required
for GDI presentation, but the bottom-up BGRA buffer can be passed to a
bottom-up DIB without a CPU row reversal. See
`OpenGlVectorLayerRenderer.cpp`.

Core objects are not thread-safe by default. The control serializes their use.
ActiveX-specific implementation units are also compiled into
`MapWinGIS.ActiveX.Impl.lib`, allowing native tests to exercise them without
requiring COM activation.

Alternate-size draws (printing, scaled metafiles) fit the live visible extent
into the target size, so they show what the screen shows instead of the
viewport's scale at a different pixel size.

GDAL notifications are delivered one drained batch per dispatch message.
Notifications queued while sinks run post their own wake, and a wake consumed
by a sink's nested message pump is re-posted after the batch. Do not
reintroduce a drain-until-empty loop: it keeps the STA thread busy for as long
as an operation reports progress.

Test-only exports such as `MapWinGISControlSetVectorPreparationHookForTesting`
exist only when `MWG_ACTIVEX_TEST_HOOKS` is defined
(`MWG_ENABLE_ACTIVEX_TEST_HOOKS` in CMake, which defaults to `MWG_BUILD_TESTS`;
`MwgEnableActiveXTestHooks` in MSBuild). Tests that need them skip when absent.

## Current limits

The control exposes a deliberately small v6 automation API. It does not expose
the broad MapWinGIS 5 map, layer, shape, event, or editing surface. The current
render scene is built in source coordinates, without reprojection; backend
support differs: GDI draws the original rings with an alternate fill rule,
while OpenGL fill draws require triangle meshes. Normal Core scene
creation is mesh-free; OpenGL fills prepare missing meshes lazily and fall
back to GDI if preparation fails or an explicitly prepared mesh is unusable.
GDI fills the original rings. The OpenGL renderer also has an outline mode
that draws those rings without triangulation.
Consult the current implementation and architecture documentation before
assuming a capability or compatibility guarantee.

## References

- [Architecture documentation](../../../docs/ARCHITECTURE.md#activex-and-rendering-architecture)
- [Porting and compatibility guidance](../../../docs/PORTING.md#current-activex-compatibility-boundary)
- [COM interface definitions](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.idl)
- [Control declaration](../../../src/MapWinGIS.ActiveX/MapWinGISControl.h)
- [ActiveX test project](../../../tests/MapWinGIS.ActiveX.Tests/MapWinGIS.ActiveX.Tests.vcxproj)
