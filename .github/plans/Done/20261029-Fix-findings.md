# Plan: resolve the codebase conformance review findings – MapWinGIS6 `development`

## Checklist

- [x] **Step 1** – Fix the four runtime defects from the branch review and add their tests (#2)
- [x] **Step 2** – Remove the Core ↔ MwGdalCore cycle and add the DLL export test (#1) — **decision: option (a) chosen 2026-10-30 (move MwGdalCore sources into Core, delete the project)**
- [x] **Step 3** – Use `CoreError` subclasses, document the result-object model, make `GdalUtils` deprecation and `Errors()` consistent, add exception tests (#4, #14)
- [x] **Step 4** – Split `GdalRuntime.h`, move `GdalOperationResult`/`GdalDatasetPtr`, hide native callbacks, add PImpl, fix `SetDatasetArgument` (#7)
- [x] **Step 5** – Extract `ComConversion` and `GdalEventSource`, split `Draw`, add shared `ViewTransform` with tests, move layer selection to Core, fix ActiveX standards deviations (#3, #9, #10, #12)
- [x] **Step 6** – Split test projects, replace test hooks, add OCX renderer tests, narrow the 4996 pragma, decide on test tags (#6, #8)
- [x] **Step 7** – Unify the interface-versioning rule, fix "Current limits", remove phase logs from `TESTING.md` (#5, #11)
- [x] **Step 8** – Enable `TreatWarningAsError` in props or CI and fix surfaced warnings (#13)

## Context

You are working in the MapWindow/MapWinGIS6 repository, branch `development` (reviewed at `73da4f27`). A codebase conformance review found 3 Major, 8 Minor and 3 Suggestion findings. This plan turns them into eight work steps. Each step groups related findings so it can land as one focused commit or PR.

The project rules are in `AGENTS.md`, `docs/CODING-STANDARDS.md`, `docs/ARCHITECTURE.md`, `docs/PORTING.md` and `docs/TESTING.md`. Read them before you start. They take precedence over this plan where the plan is silent.

Source reports:

- This plan's review: `"F:\dev\MapWindow-new\_reviews\20261029-Review.html"`.
- The earlier branch review that Step 1 depends on: `development-pre-merge-review-20261029-164819.html`, findings #2–#5 and sections 6–7.

The file-level conformance is already good: SPDX and `@file`/`@brief` headers, `#pragma once`, naming, Allman braces, `[[nodiscard]]`, a COM-free Core and clean public headers. Keep it that way in every file you add or change.

## How to work

- Do the steps in the order listed. Steps 1 and 2 must land first. Steps 3–8 are follow-ups that can each be a separate PR.
- Deliver what each step asks, at the scope intended. Make routine judgment calls yourself. Check in only where this plan says "Decision" or where different readings would lead to materially different work. If an approach in this plan looks wrong, say so in one sentence and continue as asked.
- Preserve published COM IIDs, DISPIDs and exported Core names. The Core public headers are not released yet, so header splits and PImpl changes are allowed now.
- Every step that changes behaviour or structure also updates the docs it affects (`ARCHITECTURE.md`, `PORTING.md`, `TESTING.md`, IDL comments, vcxproj comments).
- A step is done when the Debug x64 build has no new Level 4 warnings and all tests in `tests/MapWinGIS.Core.Tests` pass. If vcpkg packages are missing and you cannot build locally, say so and list what is unbuilt.
- Do not add change history, phase notes or PBI references to code or docs.
- Delegate to a subagent only for large, genuinely independent work. Do not use subagents to re-check your own work.
- Progress updates: one sentence before you start a step, a short note only when you find something important or change direction, and when a step is finished, lead with the outcome.
- Keep written docs as long as the content needs and no longer.

## Steps

### Step 1 – Fix the open runtime defects (Finding #2)

Fix the four defects from the branch review exactly as recommended there:

- Use-after-free when the control is released inside its own event (`MapWinGISControl.cpp:771-788`, `997-1001`).
- `OnDestroy` cancelling every running GDAL job (`GdalOperationDispatcher.cpp:551-570`).
- Close-time write errors lost by the algorithm runner (`GdalAlgorithmRunner.cpp:145-150`, `440-447`).
- Synchronous, superlinear render-scene building on the UI thread, about 15 s for 100k features (`VectorRenderScene.cpp:85-123`).

Add the runtime tests listed in the branch review: release inside an event, deactivation while running, close-time write failure, failed save then read, curve geometries, and a throwing log handler.

If the fix for `OnDestroy` changes the lifetime contract, update the "Current limits" section of `ARCHITECTURE.md` to match (see Step 7).

**Step 1 validation:** Debug x64 build passed; 155 native tests passed and 3
optional tests were skipped. The registered ActiveX hosting test, including
release during a progress event, passed separately in Debug x64.

**Remaining limits:** The earlier branch-review report referenced above was
unavailable, so implementation followed this plan's finding summary. Registered
ActiveX hosting was not run for Win32 or Release.

### Step 2 – Resolve the Core ↔ MwGdalCore coupling (Finding #1)

Core and MwGdalCore depend on each other. MwGdalCore includes Core's private `src/internal/GdalExecution.h` and `internal/Utf8.h` and calls `GdalEnvironment::Initialize`/`ReportDiagnostic`. Core declares `GdalExecutionMutex()`/`GdalCallbackActive()` but MwGdalCore's `GdalRuntime.cpp` defines them. Core links MwGdalCore with `/WHOLEARCHIVE` and both define `MWG_CORE_BUILD`.

Locations: `src/MwGdalCore/MwGdalCore.vcxproj:44-45`, `src/MapWinGIS.Core/MapWinGIS.Core.vcxproj:45,48,76`, `src/MapWinGIS.Core/src/internal/GdalExecution.h:14-15`, `src/MwGdalCore/GdalRuntime.cpp:26-36`, `src/MapWinGIS.Core/src/gdal/GdalEnvironment.cpp:27-28`.

**Decision (2026-07-09): option (a) — the user chose it.** Move the five MwGdalCore sources into `src/MapWinGIS.Core/src/gdal/` and delete the MwGdalCore project. This matches `ARCHITECTURE.md:32`.

- (a) ✅ **Chosen.** Move the five MwGdalCore sources into `src/MapWinGIS.Core/src/gdal/` and delete the MwGdalCore project. This matches `ARCHITECTURE.md:32`.
- (b) Not chosen. Make MwGdalCore self-contained: move `GdalExecution`, `Utf8` and the diagnostic hooks into it, so only Core depends on it. Then document in `ARCHITECTURE.md` and both vcxproj files why the library exists and how `/WHOLEARCHIVE` and `MWG_CORE_BUILD` work.

With the chosen option, add a test that resolves a runner symbol dynamically from `MapWinGIS.Core.dll`, so a dropped export is caught.

**Remaining limits:** None identified for this step.

### Step 3 – Align the GDAL error model and the legacy `GdalUtils` API (Findings #4, #14)

The GDAL API returns `bool`/`GdalOperationResult` with details in `Errors()`, also throws `CoreError` (callback re-entry) and rethrows logger exceptions. Config and typed-option code throws `std::invalid_argument`/`std::runtime_error` (`GdalRuntime.cpp:47,110`, `GdalTypedOptions.cpp:83`).

- Document the result-object model for long-running GDAL operations as a deliberate exception in `ARCHITECTURE.md:54-58`.
- Replace the std exceptions with `CoreError` subclasses, for example a new `InvalidArgument` subclass.
- Add `\throws` to every affected declaration.
- `GdalUtils.h:64,70-153`: deprecate both overload sets consistently, or document why the progress overloads stay supported. Document on the declarations that `useSharedConnection` has no effect. Align `GdalUtils::Errors()` with `GdalAlgorithmRunner::Errors()` (const reference, `noexcept`).
- Add tests for `GdalConfig::SetOption` with an empty or NUL-containing name or value, and for a typed-option formatting failure. Assert the `CoreError` subclass.

**Step 3 validation:** Debug x64 build passed with no warnings; 164 native tests ran, with 161 passing and 3 optional tests skipped.

**Remaining limits:** Public COG/VSI curl, registered ActiveX hosting, and renderer timing tests were skipped by the test suite.

### Step 4 – Restructure the GDAL public headers (Finding #7)

`include/mapwingis/gdal/GdalRuntime.h` declares eight loosely related types and exposes GDAL's C callback plumbing.

- Split `GdalRuntime.h` into config, diagnostics and progress headers.
- Move `GdalOperationResult` and `GdalDatasetPtr` out of `GdalAlgorithmRunner.h:36` into their own header.
- Move `NativeCallback` and `HandleProgress` (`GdalRuntime.h:133,146-151`) to an internal friend.
- Give `GdalProgressAdapter` and `ScopedGdalConfigOption` a PImpl.
- Change `SetDatasetArgument(std::string_view, GDALDataset*)` (`GdalAlgorithmRunner.h:90`) to take `const GdalDatasetPtr&`, or document the borrow as an approved exception in `ARCHITECTURE.md`.
- Update all includes in Core, ActiveX, tests and `samples/Cpp`.

**Step 4 validation:** Debug x64 solution build passed with no warnings. The native suite ran 160 tests: 157 passed and 3 optional tests were skipped.

**Remaining limits:** No implementation limitations identified. The public COG/VSI curl, registered ActiveX hosting, and renderer timing tests were skipped.

### Step 5 – Decompose and tidy the ActiveX layer (Findings #3, #9, #10, #12)

`MapWinGISControl.cpp` (1,001 lines) mixes conversion helpers, GDAL COM methods, dispatcher plumbing and event firing. `ARCHITECTURE.md:108` says the control does activation and API mapping only.

- Move the BSTR/SAFEARRAY/UTF-8 conversions (lines 24-283) into `ComConversion.h/.cpp`.
- Move the dispatch window, dispatcher glue and sink-invoke loop (lines 663-960) into a `GdalEventSource` class with one `FireEvent(DISPID, DISPPARAMS&, …)` helper. Merge the near-duplicate `FireGdalProgress` and `FireGdalOperationCompleted`.
- Split `OpenGlVectorLayerRenderer::Draw` (`OpenGlVectorLayerRenderer.cpp:518-784`) into `ValidateTarget`, `EnsureStaging`, `RenderScene` and `CopyToDeviceContext`.
- Add one shared `ViewTransform` (extent validation, scale, offset, margin) and use it in `GdiVectorLayerRenderer.cpp:23,166-171` and `OpenGlVectorLayerRenderer.cpp:107,537-542`. Remove both `kMargin = 12` copies. Add tests that both backends produce the same transform, including degenerate and zero-area extents.
- Move the layer-selection rule (`LayerCount() == 0` throws, always `Layer(0)`, `MapWinGISControl.cpp:557-560`) into a Core helper such as `CreateVectorRenderScene(const VectorDataset&)`. The OCX only calls it and translates errors.
- Standards fixes in `GdalOperationDispatcher.h`, `VectorRenderBackend.h`, `OpenGlVectorLayerRenderer.h/.cpp`, `GdalOperationDispatcher.cpp:107-128`:
  - Give the five `enum class` declarations an explicit underlying type.
  - Replace the output parameters of `GetResult`/`End`/`IsTargetWithinLimits` with `std::optional`, a struct or a variant.
  - Replace raw owning `new (std::nothrow)` (`FramebufferFunctions`, `WorkerExitContext`) with `std::make_unique` (catch `bad_alloc` where `noexcept` is required) and `release()` at the `CreateThread` handoff, with a comment explaining the handoff.
  - Document `\throws` and owner-thread requirements on the dispatcher's public methods.

**Step 5 validation:** Debug x64 solution build passed with no compiler
warnings; 162 native tests passed and 3 optional tests were skipped. The
registration-free ActiveX host passed all 13 automated checks.

**Remaining limits:** GDAL progress/completion COM event sinks do not have a
dedicated automated end-to-end test. Win32 and Release configurations were not
built.

### Step 6 – Restructure the test projects and tags (Findings #6, #8, test tags)

The Core test executable compiles four ActiveX sources directly, defines `MWG_ENABLE_RENDERER_TEST_HOOKS` and also activates the registered OCX (`tests/MapWinGIS.Core.Tests/MapWinGIS.Core.Tests.vcxproj:44-45,69-72,80`).

- Put the testable ActiveX units in a static library (for example `MapWinGIS.ActiveX.Impl.lib`) linked by both the OCX and a new `MapWinGIS.ActiveX.Tests` project. At minimum, split Core tests from ActiveX tests.
- Replace the test-hook preprocessor definition with an interface where you can.
- Add at least one test through the registered control for each renderer path (GDI and OpenGL).
- Add a registered-control end-to-end test that runs a GDAL operation and checks progress and completion delivery to COM event sinks, including event arguments.
- `GdalUtilsTests.cpp:27-28,492`: remove the file-wide `#pragma warning(disable: 4996)`. Narrow it to the deprecated `GdalUtils` calls through a helper, or add a documented exception for deprecated-API tests to `CODING-STANDARDS.md:31-32`.
- **Decision – ask the user** whether `ts`/`tc` test tags are required. If yes, adopt `RecordProperty("ts", …)`/`RecordProperty("tc", …)`, document the convention in `TESTING.md`, and apply it to all ~132 tests in `tests/MapWinGIS.Core.Tests/*.cpp`.

Update `TESTING.md` for the new project layout.

**Step 6 validation:** Debug x64 solution build passed with no compiler
warnings. The Core suite ran 269 tests (268 passed; the opt-in network test
was skipped); the ActiveX suite ran 43 tests (41 passed; registered hosting
and the rendering baseline were skipped in the normal run). The registered
control integration script passed, including GDI/OpenGL rendering and GDAL
progress/completion events.

**Remaining limits:** Win32 and Release configurations were not built.
Rendering baseline and network tests remain opt-in. Test tags were not adopted
per the user's decision.

### Step 7 – Align the documentation (Findings #5, #11)

- Settle one interface-versioning rule: when to add a derived interface (`IMapWinGISControl2 : IMapWinGISControl`) and when a separate one (`IMapWinGISControlGdal : IDispatch`, idl:67). State it in `docs/PORTING.md:49-56` and the IDL comment at `MapWinGIS.ActiveX.idl:36-37`. Mention `IMapWinGISControlGdal` and `IMapWinGISControlEvents` in `PORTING.md`.
- Make the "Current limits" lifetime text in `ARCHITECTURE.md` match the behaviour after Step 1.
- Remove the dated phase validation logs from `docs/TESTING.md:52-130` ("Phase 3a validation", "Follow-up validation of open Phase 5–8 findings"). Keep only conventions and current, reproducible validation steps. Put the removed text in the PR description so it is not lost.

**Step 7 validation:** Debug x64 solution build passed with no warnings. The
Core suite ran 269 tests (268 passed; the opt-in network test was skipped).
The ActiveX suite ran 43 tests (41 passed; registered hosting and the
rendering baseline were skipped).

**Remaining limits:** Win32 and Release configurations and the interactive
ActiveX host were not run for this documentation-only step. The rendering
baseline and network tests remain opt-in.

### Step 8 – Enforce warnings as errors (Finding #13)

`AGENTS.md` requires no new Level 4 warnings in Debug x64, but nothing enforces it. Enable `<TreatWarningAsError>true</TreatWarningAsError>` in `build/props/MapWinGIS.Cpp.props:15-35`, or at least in CI (`.github/workflows`, for example `-p:TreatWarningAsError=true`). Fix any warnings this surfaces. Do this step last so earlier steps are not blocked by unrelated warnings.

**Step 8 validation:** Debug x64 solution build passed with warnings treated
as errors and no compiler warnings. The Core suite ran 269 tests (268 passed;
the opt-in network test was skipped). The ActiveX suite ran 43 tests (41
passed; registered hosting and the rendering baseline were skipped).

**Remaining limits:** Win32 and Release configurations were not built. The
optional COG/VSI curl, registered ActiveX hosting, and rendering baseline tests
were skipped.
