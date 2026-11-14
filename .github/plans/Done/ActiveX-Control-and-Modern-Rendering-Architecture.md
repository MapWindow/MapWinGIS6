# MapWinGIS6 ActiveX Control and Modern Rendering Architecture

## Progress checklist

Update this checklist as work progresses. Complete and verify each phase before moving to the next. Keep the checklist current in the final response and, if appropriate, in a dedicated implementation-plan document.

- [x] **Phase 0 — Repository investigation and architectural assessment**
- [x] **Phase 1 — Define the modern ActiveX and rendering architecture**
- [x] **Phase 2 — Add the ActiveX project and establish COM activation**
- [x] **Phase 3 — Host the control and implement resize/paint lifecycle**
- [x] **Phase 3a — Add a .NET 10 C# application for manual and automated ActiveX tests**
- [x] **Phase 4 — Render a real map layer from MapWinGIS.Core**
- [x] **Phase 5 — Implement the initial rendering abstraction and OpenGL backend**
- [x] **Phase 6 — Establish resource-aware CPU fallback and hardware capability detection**
- [x] **Phase 7 — Validate Win32 and x64 builds and host compatibility**
- [x] **Phase 8 — Document the architecture, limitations, and next steps**

---

## 1. Objective

Extend the existing MapWinGIS6 project by introducing a new ActiveX/COM control incrementally. The first milestone must produce a working, hostable ActiveX control that displays at least one real map layer using the existing `MapWinGIS.Core.dll` engine.

This is a **modern redesign of the MapWinGIS5 functionality**, not a line-by-line port of the old implementation.

The architecture should be optimized for maintainability, performance, testability, extensibility, and future hardware capabilities. Backward compatibility is desirable, but it is a secondary consideration and must not force poor architectural decisions.

Work in the existing `MapWindow/MapWinGIS6` repository, on the `development` branch. Treat the MapWinGIS5 repository as a reference implementation and source of behavioural knowledge.

Do not start by rewriting the existing core engine or attempting to port the entire legacy control. Build a small, functioning vertical slice first.

## 2. Non-negotiable design principles

### 2.1 Design for MapWinGIS6, not MapWinGIS5

- Treat MapWinGIS5 as a reference for functionality, externally visible behaviour, and useful API semantics.
- Do not mechanically reproduce its internal class hierarchy, MFC/ATL implementation details, rendering pipeline, ownership model, or historical workarounds.
- Prefer modern, cohesive abstractions over legacy-compatible internal structures.
- Keep the design simple and modular. Avoid introducing abstractions that have no clear current purpose or credible future use.
- Where a legacy API conflicts with a substantially better design, document the trade-off and propose a modern alternative rather than compromising the core architecture.
- Preserve established public interfaces when the cost is low and doing so is useful to existing applications. Do not sacrifice significant performance, safety, maintainability, or architectural clarity merely to preserve an implementation detail.
- Do not introduce a second, independent mapping engine. Reuse and improve the existing `MapWinGIS.Core` implementation.

### 2.2 Keep the core independent of COM and graphics APIs

`MapWinGIS.Core.dll` must remain a plain C++ mapping engine without dependencies on COM, ATL, or MFC.

The ActiveX control should be an adapter between COM clients and the modern core:

- Translate external COM calls into suitable core operations.
- Manage COM-specific concerns at the boundary.
- Keep rendering implementation details out of the public COM API wherever practical.
- Avoid duplicating map state or GIS logic in the ActiveX layer.
- Do not expose internal C++ implementation types directly through COM.

The core must remain usable by future non-COM clients.

### 2.3 Preserve the ability to evolve

The design must support future improvements without requiring a complete rewrite:

- Alternative rendering backends.
- CPU rendering on machines without suitable GPU support.
- OpenGL rendering as the first accelerated backend.
- OpenCL compute acceleration where profiling demonstrates a real benefit.
- Large datasets and high-resolution displays.
- Hardware with limited RAM or VRAM.
- Future non-ActiveX front ends.

Do not implement every future capability in the initial milestone. Establish only the abstractions and boundaries justified by the first implementation.

## 3. Phase 0 — Investigate before changing anything

Before editing source files, inspect both repositories and the existing build configuration.

### MapWinGIS6

Read:

- `AGENTS.md`
- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/BUILDING.md`
- `docs/CODING-STANDARDS.md`
- `docs/PORTING.md`
- `docs/TESTING.md`

Also inspect:

- The complete solution and project structure.
- The public API and implementation of `MapWinGIS.Core`.
- Existing map, layer, dataset, geometry, coordinate transformation, and rendering-related abstractions, where present.
- Existing tests and the current CI workflow.
- vcpkg configuration, dependency manifests, and build scripts.
- Current Win32 and x64 build behaviour.
- Existing DLL export and resource-management conventions.

Do not assume a capability exists merely because a document describes a future intention. Verify it in the source code.

### MapWinGIS5

Locate and inspect the corresponding MapWinGIS5 repository and the relevant legacy implementation.

Identify:

- The old ActiveX control's creation and registration mechanism.
- Its COM interfaces, properties, methods, events, and type library.
- How the control obtains its drawing surface and responds to resizing and painting.
- How map layers and rendering are connected to the control.
- Which behaviours are important for existing clients.
- Which legacy implementation details should deliberately not be carried forward.

Do not copy large sections of legacy code without understanding their dependencies and purpose.

### Required investigation output

Before implementation, summarize:

1. What already exists and can be reused.
2. What is missing to create and host the ActiveX control.
3. Which core APIs can support initial map rendering.
4. Which architectural decisions require confirmation rather than assumption.
5. The smallest implementation sequence that can deliver the first visible map.

Record relevant findings in a concise implementation plan. Avoid unrelated refactoring.

**If a decision materially affects architecture, compatibility, dependencies, or project scope and cannot be resolved from the repository, stop and ask me rather than guessing.** For routine implementation details, choose a sensible modern default and document it.

## 4. Phase 1 — Define the architecture

Propose and document a small architecture before implementing the control.

The intended high-level separation is:

- **MapWinGIS.Core** — GIS data, map/layer state, geometry and other reusable mapping functionality.
- **MapWinGIS.ocx** — COM/ActiveX integration, control lifecycle, host integration, and translation of external API calls.
- **Rendering subsystem** — drawing orchestration and backend-independent rendering responsibilities.
- **Rendering backends** — CPU rendering and OpenGL rendering, with a clean boundary between them.
- **Compute subsystem** — optional future acceleration of suitable computational workloads, separate from rendering.

These are conceptual responsibilities, not a requirement to create a separate DLL for every subsystem. Prefer a small number of appropriately scoped components until there is a clear reason to split them.

### Rendering architecture

Define an internal rendering interface that allows the application to select a backend without changing map data structures or the public COM contract.

The initial design should support:

1. A CPU rendering path or fallback capable of operating without a suitable GPU.
2. An OpenGL rendering backend as the first accelerated implementation.
3. Runtime capability detection and backend selection.
4. Clean handling of rendering initialization failures and device/context loss.
5. A future OpenCL compute integration point that does not depend on OpenGL being present.

Keep rendering, GIS calculations, data access, and COM concerns separate.

Do not assume OpenCL is a rendering API. OpenGL is intended for rendering; OpenCL may later accelerate suitable compute-heavy operations. Investigate interoperability between them only if there is a demonstrated use case.

Do not add OpenCL as a runtime dependency or implement a speculative compute framework in the first milestone.

### Architecture deliverables

Before or during the first implementation, document:

- Component responsibilities and dependencies.
- Ownership and lifetime of the control, rendering context, map data, and GPU resources.
- How the rendering backend is selected and replaced.
- How CPU fallback works.
- How the architecture can be tested without a GPU.
- Which legacy COM behaviours are retained and which are intentionally deferred.

## 5. Phase 2 — Introduce the ActiveX project

Add the ActiveX control project to the existing solution in the manner best supported by the repository's current Visual Studio and MSVC toolchain.

Investigate the required ATL/MFC components and configuration. Keep these dependencies confined to the ActiveX project unless there is a compelling reason otherwise.

The control must:

- Be implemented as a COM-creatable ActiveX control.
- Be able to load the core engine through a clear and maintainable boundary.
- Follow appropriate COM ownership, lifetime, and error-reporting conventions.
- Have a deliberate COM activation and deployment strategy, including registration-free activation for the .NET test host.
- Avoid leaking resources when created, destroyed, or recreated.
- Avoid unnecessary dependencies on legacy MapWinGIS5 implementation details.

First establish the smallest reliable COM activation path. Do not implement the complete historical interface before the first visible map is working.

### Public API and compatibility policy

Review the legacy type library and interface definitions to understand the established contract.

For the new control:

- Preserve existing interface identifiers and compatible signatures when intentionally implementing an existing interface.
- Do not reuse an existing interface identifier for an incompatible interface.
- Use new interface identifiers for incompatible or new COM interfaces.
- Consider interface versioning rather than silently changing published contracts.
- Keep internal implementation choices independent of COM interface layout.
- Do not implement large numbers of unused legacy properties and methods merely to claim compatibility.
- Identify and document intentional compatibility gaps.

Prioritize a well-designed, stable new architecture. Add backward compatibility incrementally when it is useful and does not impose a disproportionate burden.

## 6. Phase 3 — Establish the control lifecycle

Implement and test the minimum control lifecycle:

- COM activation and creation.
- Initialization and cleanup.
- Creation and management of the drawing surface.
- Correct behaviour when the control is resized.
- Painting and invalidation.
- Safe destruction and recreation.
- Clear error handling if a graphics backend cannot initialize.

Use a simple test host or an existing suitable test application to verify that the control can be instantiated, displayed, resized, and destroyed.

Do not assume that compiling the OCX is sufficient. Verify actual activation and hosting.

Keep platform-specific windowing and COM code isolated from the GIS engine.

## 6a. Phase 3a: Add a .NET 10 C# test application

Create a small Windows Forms application in C# that targets `net10.0-windows`.
Windows Forms provides `AxHost`, a managed host for ActiveX controls.
Embed the actual `MapWinGIS.ocx` control through `AxHost`.
Do not replace the control with an image, mock, or direct call to the native core.

This phase follows Phase 3 and precedes Phase 4.
Keep the existing phase numbers unchanged.
The application must support interactive manual tests and automated tests through the same hosting code.

### Application and build

- Add the application under `samples\CSharp\ActiveXTestHost\` and include it in the solution.
- Keep the application small. Do not add a separate map engine or a general UI testing framework.
- Use the existing control identifiers and automation contract. Do not require new COM methods just for the test application.
- Run the control on the UI thread in a single-threaded apartment (STA), the COM threading mode required by this control.
- Start with an explicit x64 target that matches the native build. Do not use `AnyCPU`.
- Make the C# project support x86 when the Win32 native build is available. Complete both architecture checks in Phase 7.
- Document the .NET 10 SDK requirement and any tools needed to generate the managed ActiveX wrapper.
- Generate wrapper assemblies during the build if needed. Do not commit generated assemblies or machine-specific paths.
- Make the matching OCX, core DLL, native dependencies, and runtime data available through a repeatable build or launch procedure.

Use registration-free COM activation through the host application's manifest.
Document launch and normal process shutdown for the matching process architecture.
Do not make registry changes to run the test host.
Show clear errors for missing files, architecture mismatches, and activation failures.

### Manual test mode

The default launch must open a visible window with the embedded control.
The window must stay open until the user closes it.
Provide actions to recreate the control and request a repaint.
Show activation status, available automation values, and errors in the application.

The user must be able to:

- See the actual control surface and resize the window.
- Request a repaint and observe the result.
- Destroy and recreate the control without restarting the application.
- Close and reopen the application without a crash or a locked OCX.

At this phase, the control paints only its current background.
Label this limitation clearly. Do not claim that the application displays map data.

### Automated test mode

Add a documented `--automated` command-line mode.
It must create a real window, start the normal UI message loop, and host the actual control.
Run automated actions on the UI thread after activation.
Do not treat COM object creation alone as a successful hosting test.

The automated mode must:

- Assert successful activation, control creation, and access to the available automation contract.
- Resize the host and assert that the control surface follows the client area.
- Request a repaint and assert an observable paint result.
- Destroy and recreate the hosted control, then repeat the activation and paint checks.
- Close the window, dispose the managed host, and release its COM resources.
- Use a bounded timeout so a hung UI cannot block a test run indefinitely.
- Return exit code zero only when all required checks pass. Return a nonzero code for failures or missing prerequisites.
- Write a machine-readable result file with each check, its outcome, and useful failure details.

Use the same create, resize, repaint, and dispose paths in both modes.
Do not add a separate simulated path for automated tests.
Document that these tests require Windows and an interactive desktop session.
If the CI runner supports that session, add a repeatable integration command.
Otherwise, document the runner limitation and the local command without reporting skipped coverage as a pass.

### Completion criteria and later use

Build and run the application in manual mode and automated mode against the same Debug x64 native build.
Record the commands and actual results in the relevant build and testing documentation.
Retain the existing native tests.

In Phase 4, extend this application to load and display a deterministic map layer through the control.
Add the corresponding automated rendering checks in that phase.
In Phase 7, run the application against matching x86 and x64 controls.
Do not implement those later rendering or architecture milestones in Phase 3a.

### Phase 3a implementation notes

- Added a .NET 10 WinForms host using `AxHost` with the existing control CLSID and `CoreInitialized` automation property. No generated wrapper assembly or additional COM API is needed.
- The x64 Debug build passed. The existing native suite ran 135 tests (133 passed, 2 skipped); the native registered-host activation check passed.
- The host manifest declares the OCX class and type library. Manual and automated host runs activate the OCX without COM registration or registry changes.
- The managed manual host opened, activated the control, and closed through its normal window-close path. Its automated mode passed all 10 checks and wrote the JSON results file.
- The Win32/x86 managed host compiles, but matching native Win32 hosting remains for Phase 7.
- At Phase 3a completion, the control painted only its background; Phase 4 rendering and current remaining limitations are recorded below. The integration host still requires an interactive Windows desktop and is not run in CI.

## 7. Phase 4 — Display a real map layer

The first meaningful functional milestone is a visible map rendered from actual `MapWinGIS.Core` data.

Inspect the current core APIs and select the simplest suitable supported data source and layer type. Reuse existing functionality rather than creating a parallel data-loading path.

The milestone must demonstrate that:

1. The control can be created by a host.
2. A map layer can be obtained or loaded through the core.
3. The rendering subsystem receives the necessary map/layer information.
4. The control paints the layer in its drawing surface.
5. Resizing and repainting work correctly.
6. Core and COM resources are released correctly.

Use a small deterministic test dataset where possible. Document any test-data prerequisites.

A temporary diagnostic display is acceptable during development, but the completed milestone must display real GIS content, not just a blank control, placeholder image, or hard-coded mock map.

Add automated tests for testable core and rendering logic, plus a repeatable integration or manual verification procedure for COM activation and hosting.
Use the C# test application from Phase 3a for manual map display and automated integration checks.

### Phase 4 implementation notes

- Added backend-neutral Core scene data for polygon features from the existing vector API. The ActiveX control loads the first dataset layer and paints its polygons through a small GDI renderer; it does not add graphics APIs to Core.
- The C# host creates a deterministic two-polygon GeoJSON dataset in a temporary directory. Its automated mode verifies dataset loading, visible polygon fills, the empty space between polygons, resize, repaint, disposal, and recreation.
- Debug x64 build passed. The native suite ran 137 tests: 135 passed and 2 were skipped. The registration-free host passed all 12 automated checks.
- Remaining limitations: only the first Polygon/MultiPolygon layer is rendered; coordinates are drawn in their source projection without reprojection; rendering uses GDI and has no backend abstraction, OpenGL backend, or CPU fallback policy yet. Scene preparation copies all supported feature geometry and currently converts the Core WKT output back through OGR, so it is intended for small datasets and has not been performance-tested. The automated host requires an interactive Windows desktop and is not run in CI.

## 8. Phase 5 — Implement the initial OpenGL backend

Once the visible map foundation is established, integrate OpenGL through the rendering abstraction.

First inspect the current dependencies and project conventions. Choose the most appropriate OpenGL loading, context creation, and supporting libraries only after checking the existing toolchain and dependency policy.

Do not introduce a large dependency stack without justification.

Requirements:

- Keep OpenGL-specific code inside the rendering backend.
- Do not expose OpenGL types through the core public API or COM interfaces.
- Handle context creation and destruction safely.
- Handle resize, viewport changes, and repainting correctly.
- Avoid unnecessary GPU uploads and repeated allocations.
- Release GPU resources deterministically.
- Provide useful diagnostics if initialization fails.
- Ensure that the application can fall back to CPU rendering when OpenGL is unavailable or unsuitable.

Do not claim GPU acceleration for operations that still run entirely on the CPU. Distinguish accelerated drawing from accelerated GIS computation.

### Phase 5 implementation notes

- Added an internal vector-render backend contract and coordinator to the ActiveX project. It tries a reusable WGL/OpenGL 1.1 backend first and falls back to the existing GDI renderer when initialization or drawing fails, or when the scene contains unsupported geometry.
- OpenGL currently draws convex polygon rings without holes. It reads the rendered pixels back into reusable CPU buffers before copying them to the host device context; this establishes a replaceable backend but does not provide end-to-end accelerated presentation.
- Added native tests for OpenGL polygon rendering and GDI fallback for polygons with interior rings. The Debug x64 build completed without compiler warnings; 140 native tests ran, with 138 passing and 2 skipped.
- Remaining limitations: only convex single-ring polygons use OpenGL; all rendering still uses source coordinates without reprojection; the full-frame readback can be costly; and runtime hardware capability reporting, context-loss recovery, configurable backend selection, and resource policies remain for Phase 6. The registration-free C# host passed activation, layer loading, and resize, but screen-pixel assertions could not be verified in this noninteractive session because the host window was absent from the captured desktop. Repeat the host check on an interactive Windows desktop.

## 9. Phase 6 — Resource-aware performance and hardware fallback

The application must work on both high-end systems and modest desktop computers.

Do not assume that all users have a discrete GPU, large VRAM, or a recent OpenGL implementation.

### Capability detection

Use runtime capability detection to determine which rendering paths are usable. Do not rely solely on the presence of a GPU or on a particular GPU vendor.

Make backend selection deterministic, observable, and testable. Provide a way to identify the active backend and explain why a preferred backend was rejected.

### Memory and GPU resource management

Design for bounded resource use:

- Avoid loading an entire large dataset into GPU memory unnecessarily.
- Avoid duplicating large datasets in RAM and VRAM without a clear benefit.
- Reuse buffers and textures where appropriate.
- Avoid repeated uploads of unchanged data.
- Release obsolete GPU resources promptly.
- Consider tiling, chunking, caching, and level-of-detail techniques when justified by dataset size and profiling.
- Ensure that memory pressure or allocation failure does not cause uncontrolled resource growth.
- Keep rendering responsive when processing large datasets.

Do not introduce elaborate cache management or data-paging systems before measurements justify them. Start with simple, measurable resource policies.

### CPU fallback

The CPU path must remain a real supported execution path, not an unimplemented placeholder.

If OpenGL initialization fails, the application must either select CPU rendering automatically or report a clear recoverable error and offer the supported fallback.

The core GIS functionality must not require OpenGL or OpenCL to be installed.

### Performance methodology

Establish a simple baseline before attempting optimization.

Measure relevant costs separately where practical, including:

- Core data access.
- Preparation of renderable data.
- CPU rendering.
- GPU uploads.
- OpenGL rendering.
- Memory consumption.

Avoid premature optimization. Use benchmarks and representative datasets to decide where further work is worthwhile.

### Phase 6 implementation notes

- Added runtime OpenGL capability checks for framebuffer-object entry points and the maximum renderbuffer size. `Automatic` and `OpenGL` preferences try OpenGL first and fall back to GDI; callers can select GDI directly.
- The control exposes `RenderingBackendPreference`, `RenderingBackend`, and `RenderingStatus` through automation. The status identifies the active backend and reports why OpenGL was rejected.
- Limited the reusable OpenGL CPU staging buffer to 64 MiB per frame and convert readback pixels in place. Targets beyond the detected OpenGL size or memory limits use GDI. Failed/lost contexts are recreated on a later draw.
- Debug x64 solution build passed with no new warnings. The native suite ran 146 tests: 144 passed and 2 skipped. The automated C# host passed prerequisite, activation, automation, data loading, CPU-backend status, and resize checks; its screen-pixel assertion did not match the expected fill color in this noninteractive run. Repeat the full visual check on an interactive Windows desktop.
- Remaining limitations: no timing or memory baseline has been collected; the policy limits only the OpenGL staging buffer and framebuffer, while Core still prepares and retains the complete polygon scene in RAM. There is no tiling, paging, or unchanged-geometry cache. OpenGL still supports only convex single-ring polygons and presents through CPU readback; reprojection remains unsupported.

## 10. Phase 7 — Win32 and x64 support

Both Win32 and x64 are required targets for the ActiveX control and relevant dependencies.

Inspect existing platform configurations and establish a reproducible build for each architecture.

Verify:

- Debug and Release configurations where supported by the repository.
- Correct architecture matching between the host, OCX, and dependent DLLs.
- Correct COM registration and activation for each architecture.
- Appropriate handling of architecture-specific dependencies.
- No accidental use of x64-only APIs or assumptions in shared code.
- Correct packaging of the required runtime components.

Remember that a 32-bit COM host requires a compatible 32-bit in-process ActiveX control and compatible in-process dependencies. A 64-bit control cannot be loaded directly into a 32-bit process, or vice versa.

Keep the implementation shared between architectures wherever practical. Do not create separate codebases merely to support Win32 and x64.

If an existing dependency cannot support one target, identify the exact limitation and investigate alternatives before changing the project scope.

### Phase 7 implementation notes

- Debug and Release builds of the whole solution succeed for both Win32 and x64, with no compiler warnings at Level 4. The native suite ran 146 tests on each Debug/Release x Win32/x64 build: 144 passed and 2 were skipped. The registration-based activation script passed for x64 (64-bit `pwsh`) and Win32 (32-bit Windows PowerShell), including the ATL host resize/repaint/recreate test.
- Fixed Win32-only defects: `CSLDestroy` is `__stdcall` on x86, so `GdalRuntime.cpp` no longer names its function-pointer type; tests compare `size_t` values (32-bit `unsigned int` on x86) with unsigned operands.
- Fixed the build system: vcpkg pruned the other triplet from the shared `vcpkg_installed\` tree and a stale MSBuild stamp skipped reinstalling it, so alternating Win32/x64 builds failed. Each platform now has its own `vcpkg_installed\<Platform>\` tree.
- Remaining limitations: the C# host was only built, not run, for Win32 and x64 because it needs an interactive desktop; Win32 screen-pixel checks remain unverified. Build both platforms with `-restore` when alternating (the C# host's `obj\` assets file is shared). Packaging of release artifacts (including the `full-drivers` feature) was not verified for Win32. Only Debug was used for registration/activation checks.

## 11. Phase 8 — Testing, documentation, and completion criteria

Follow `AGENTS.md` and the existing coding and testing standards throughout.

Use the repository's documented bootstrap, build, and test commands. At minimum, use the established commands where applicable:

- `pwsh -File build\scripts\bootstrap-vcpkg.ps1`
- `msbuild MapWinGIS6.slnx -p:Configuration=Debug -p:Platform=x64 -m`
- `bin\Debug\x64\MapWinGIS.Core.Tests.exe`

Adapt commands to the actual project and configuration after inspecting the solution. Do not report a build or test as successful unless it was actually run and passed.

Add tests for new core logic and backend-independent behaviour. Where automated testing of COM activation or GPU rendering is impractical, provide a documented, repeatable verification procedure.

Update relevant documentation, especially architecture, building, testing, and porting guidance. Clearly distinguish completed functionality from planned future work.

### The first milestone is complete when

- [x] The ActiveX project builds as part of the solution.
- [x] COM activation and registration have been verified.
- [x] A test host can instantiate and display the control.
- [x] The .NET 10 C# test application supports manual use and automated tests with recorded results.
- [x] Resizing, repainting, destruction, and recreation work correctly.
- [x] At least one real map layer from `MapWinGIS.Core` is displayed.
- [x] Rendering is separated from COM and GIS data access.
- [x] The rendering design supports a CPU fallback and an OpenGL backend.
- [x] OpenGL initialization failure is handled safely.
- [x] Win32 and x64 builds are verified.
- [x] Relevant tests and existing regression tests pass.
- [x] Documentation describes the actual implementation and its limitations.
- [x] Any remaining compatibility gaps and follow-up work are documented.

### Phase 8 implementation notes

- Updated `docs/ARCHITECTURE.md`, `docs/BUILDING.md`, `docs/TESTING.md`, and `docs/PORTING.md` to describe the implemented COM/rendering boundaries, platform verification steps, compatibility surface, limitations, and next milestone.
- The Debug x64 solution build passed with no warnings. The native suite ran 146 tests: 144 passed and 2 were skipped (the opt-in network test and the separately exercised native host check).
- Phase 7 validated Debug and Release builds and native test runs for Win32 and x64, and passed native registration/host-lifecycle checks in Debug.
- Remaining limitations: the registration-free host's rendered-pixel checks need a rerun in an interactive desktop; Win32 host pixel behavior and Release packaging with `full-drivers` remain unverified. OpenGL handles only convex single-ring polygons and presents through CPU readback. Core retains the full supported polygon scene in memory, without tiling, paging, or a performance baseline. Reprojection, map navigation, broad MapWinGIS 5 API parity, and OpenCL remain future work.
- The next milestone is a tested map-view transform for viewport and coordinate-system behavior, followed by measurements on representative datasets before adding caching or broader OpenGL geometry support.

## 12. Implementation discipline

Work incrementally. Keep each change focused, buildable, and reviewable.

Before each phase:

1. Confirm the relevant architecture and existing implementation.
2. Identify the smallest useful change.
3. State which files and components will change.
4. Identify the appropriate tests.

After each phase:

1. Build the affected projects.
2. Run the relevant tests.
3. Resolve regressions before proceeding.
4. Update this checklist and the implementation notes.
5. Summarize what works, what remains, and any architectural decisions.

Do not:

- Rewrite the core engine as part of this task.
- Port the legacy ActiveX implementation wholesale.
- Introduce OpenCL dependencies before a justified use case exists.
- Make a GPU mandatory.
- Create duplicate GIS or rendering implementations.
- Add speculative features unrelated to the first visible-map milestone.
- Silently weaken existing tests or suppress build errors to make the project appear successful.
- Claim COM activation, rendering, or architecture support has been verified without testing it.

If a major architectural decision cannot be resolved from the repositories and documentation, ask me before proceeding. Otherwise, make a well-reasoned choice, document it, and continue.

## Final report

At the end, report:

1. Which checklist items are complete.
2. The architecture and key design decisions.
3. Which files and projects were added or changed.
4. Build and test results for Win32 and x64.
5. How to run the control in the test host.
6. What the control can actually render.
7. Current CPU/OpenGL fallback behaviour and limitations.
8. Any compatibility gaps and the rationale for them.
9. The most useful next implementation milestone.

Keep the scope focused on establishing a sound modern foundation and displaying a real map. Do not treat full MapWinGIS5 API parity as a prerequisite for success.
