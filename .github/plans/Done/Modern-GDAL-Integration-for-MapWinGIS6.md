# Modern GDAL Integration for MapWinGIS6

## Progress Checklist

Update this checklist as work progresses. Mark an item `[x]` only after its acceptance criteria have been met and verified. Do not mark work complete merely because code has been written.

- [x] **Phase 0 — Foundation:** GDAL dependency, `MwGdalCore`, lifecycle management, error handling, progress and cancellation.
- [x] **Phase 1 — Generic algorithm runner:** Implement `GdalAlgorithmRunner` using the GDAL `GDALAlgorithm` API and expose the generic native façade.
- [x] **Phase 2 — Typed APIs:** Implement strongly typed option structures and high-priority algorithm wrappers.
- [x] **Phase 3 — Backward compatibility:** Provide deprecated MapWinGIS5-compatible `GdalUtils` methods and migration documentation.
- [x] **Phase 4 — Modern GDAL capabilities:** Implement cloud/virtual filesystems, COG, VRT, in-memory processing, pipelines and optional multidimensional support.
- [x] **Phase 5 — Error handling and logging:** Complete structured results, warning logging, cancellation plumbing and operation lifecycle handling.
- [x] **Phase 6 — Tests, samples and documentation:** Complete automated tests, integration tests, examples and user documentation.
- [x] **Phase 7 — ActiveX wrapper and COM progress dispatch:** Expose native GDAL operations through the ActiveX wrapper with thread-safe progress delivery, cancellation, and operation lifetime handling.
- [x] **Final verification:** All required tests pass, build configuration is reproducible, documentation is complete, and the final review is satisfactory.

**Phase 1 façade note:** At Phase 1, the generic API was exported through `MapWinGIS.Core.dll`; the existing `MapWinGIS.ocx` provided COM activation, control lifecycle, vector loading, and polygon rendering, but no asynchronous GDAL operation model. Phase 7 added GDAL COM access and progress dispatch to that existing wrapper; completed phases retain their native API and ownership contracts.

**Current phase:** Final verification (complete; [PR #28](https://github.com/MapWindow/MapWinGIS6/pull/28) open).

**Phase 5 verification:** Debug | x64 builds without new Level 4 warnings. The full suite passes 129 tests, with one opt-in network test skipped. Coverage includes warnings, failures, cancellation, callback/logger exceptions, finalization, execution serialization, and isolated concurrent diagnostics. There is no asynchronous operation model for `BeginGdalOperation` / `EndGdalOperation`; future COM progress dispatch remains a wrapper responsibility.

**Phase 6 verification:** Debug | x64 builds the solution, including the C++ translation sample, without new Level 4 warnings. The full suite passes 133 tests, with one opt-in network test skipped. The sample starts and validates its command-line arguments. GDAL and vector fixtures remain runtime-generated in temporary directories as required by `AGENTS.md`; no C# GDAL sample is possible until the Core has a managed binding or the ActiveX layer exposes the GDAL API.

**Phase 6 open limitations (recorded before Phase 7):**

- No C# GDAL sample was provided at Phase 6 because the native Core has no managed binding and the ActiveX control did not yet expose the GDAL API. Phase 7 added the ActiveX GDAL interface and `GdalOperationClient` sample.
- Test datasets are generated at runtime in temporary directories rather than committed as static fixtures, following the repository's test-data guidance. Reconsider only if fixture provenance or licensing requirements change.

**Phase 7 verification:** The Debug | x64 solution and C# ActiveX host build with Visual Studio MSBuild. The native suite passes 150 of 153 tests; the opt-in network test, hosted ActiveX test, and rendering baseline are skipped in the normal test run. The isolated ActiveX host script passes, including raster conversion pixel verification, progress-thread affinity, repeated cancellation, callback failures, warnings, GDAL failures, and unknown algorithms. The build reports no new Level 4 warnings.

**Phase 7 open limitations:**

- Cancellation before a worker begins is implemented but is not tested deterministically; the current tests cover cancellation during progress and dispatcher shutdown.
- The hosted COM test does not yet cover event-sink disconnection during an active operation or destruction of the control while a worker and notifications are pending. Native dispatcher shutdown coverage does not replace these host-lifecycle cases.
- The generic v6 COM API uses a new interface IID because the legacy `IGdalUtils` IID and operation-specific signatures are not compatible with this API. This is documented as an intentional compatibility boundary.

**Final verification:** Visual Studio 2026 MSBuild 18.11.1 completed the Debug | x64 solution build with no compiler warnings. The native suite ran 156 tests: 153 passed and 3 were skipped (opt-in network test, registered-host test in the normal run, and rendering baseline). `Verify-ActiveXActivation.ps1` passed the registered COM host test, including GDAL conversion and cancellation. The registration-free C# host passed all 13 automated rendering and lifecycle checks. Documentation review corrected stale descriptions of the ActiveX GDAL interface and migration boundary.

**Final verification open limitations:**

- Cancellation before a worker begins still lacks a deterministic test.
- Event-sink disconnection during an active operation and control destruction while a worker has pending notifications remain untested.
- The C# `GdalOperationClient` sample is not exercised by the automated C# host; COM GDAL behavior is covered by the registered native host test instead.
- The opt-in network GDAL test and rendering performance baseline were not run during final verification.

**Important:** Work on exactly one phase at a time. After completing a phase, stop and prepare a pull request for human review. Do not automatically proceed to the next phase, even if the current phase is successful.

---

## 1. Objective

Implement modern GDAL integration in the [MapWinGIS6 repository](https://github.com/MapWindow/MapWinGIS6).

This is a modernization and redesign, not a line-by-line port of MapWinGIS5.

The implementation must provide:

- A modern, generic GDAL algorithm execution engine.
- Strongly typed APIs for common operations.
- Backward compatibility with the relevant MapWinGIS5 `GdalUtils` methods.
- Structured error reporting, progress reporting and cancellation.
- Support for in-memory datasets, cloud data sources and modern GDAL capabilities.
- Automated tests, migration examples and maintainable documentation.

The scope is the GDAL integration and its supporting infrastructure. UI work, including menus and dialogs, is out of scope unless explicitly required by an existing API or integration point.

## 2. Repository Investigation — Before Making Changes

First inspect the current MapWinGIS6 repository and its project instructions.

1. Read `AGENTS.md`, `COPILOT-INSTRUCTIONS.md`, `.github/copilot-instructions.md`, or equivalent instruction files if present. Follow the actual instructions that apply to the files being changed.
2. Read the documentation referenced by those instructions.
3. Inspect the solution and build structure, including CMake configuration, vcpkg manifests, native libraries, COM interfaces, IDL files, existing error handling, logging, threading and testing infrastructure.
4. Identify existing GDAL dependencies or GDAL-related code, if any.
5. Inspect the existing MapWinGIS6 conventions for naming, namespaces, file organization, public interfaces, memory ownership and error handling.
6. Determine how native libraries and dependencies are built and distributed.
7. Identify the existing test framework and how tests are discovered and executed.

Do not assume that the repository has the same structure as MapWinGIS5 or that proposed class names and paths already exist.

Produce a short implementation assessment before editing files. Identify the existing components that should be reused and any genuine incompatibilities between this plan and the repository.

Use the supplied GDAL integration plan as the technical specification. Do not silently discard its requirements or redesign its agreed architecture. If a requirement conflicts with the actual repository or a verified GDAL API limitation, explain the conflict and propose the smallest necessary adjustment before proceeding.

Do not implement multiple phases during this initial investigation.

## 3. Technical Decisions

Treat the following as agreed architectural decisions.

### 3.1 Three-layer architecture

**Layer 1 — GDAL core**

Use GDAL's native APIs, drivers, OGR support and virtual filesystems.

**Layer 2 — Native algorithm and options layer**

Create a native C++ static library named `MwGdalCore`, without COM dependencies. It will contain the generic algorithm runner, typed option structures, error handling and operation support.

**Layer 3 — Public API**

Expose GDAL functionality through the appropriate MapWinGIS6 COM interfaces and, where appropriate, a .NET façade consistent with the existing architecture.

Keep COM-specific concerns outside `MwGdalCore`.

### 3.2 GDAL version and build requirements

- Target GDAL 3.12 or later, with an explicitly pinned version.
- Use the C++ `GDALGlobalAlgorithmRegistry::GetSingleton()` API when supported by the pinned version.
- If GDAL 3.11 compatibility becomes a requirement, use the appropriate C API and RAII wrappers instead, and document the decision.
- Require C++17 or later.
- Integrate GDAL using vcpkg and/or the existing CMake dependency mechanism.
- Do not vendor the GDAL source tree into MapWinGIS6.
- Record the dependency version in the appropriate build and dependency manifests.
- Document the supported version and its implications in `docs/gdal-version.md`.

Verify actual API availability against the pinned GDAL headers rather than relying solely on assumptions in this prompt.

### 3.3 API strategy

- Use `GDALAlgorithm` as the generic algorithm execution mechanism.
- Provide strongly typed option structures for common operations.
- Retain a legacy compatibility façade for existing MapWinGIS5-style calls.
- Provide structured errors, progress callbacks and cancellation.
- Support in-memory MEM/VRT datasets and algorithm pipelines where the selected GDAL version supports them.

These architectural decisions should not be revisited without a concrete technical reason.

## 4. Phase 0 — Foundation

### Tasks

1. Integrate GDAL into the build using the repository's existing conventions.
2. Create the `MwGdalCore` static library.
3. Implement `GdalInitializer` using RAII:
   - Register GDAL drivers safely.
   - Ensure registration occurs only once per process.
   - Handle shutdown and resource lifetime safely.
   - Avoid destroying global GDAL state while other components still depend on it.
4. Implement a global configuration-option helper:
   - Set configuration options.
   - Provide a scoped guard that restores previous values.
5. Implement `GdalErrorScope`:
   - Use `CPLPushErrorHandler` and `CPLPopErrorHandler` appropriately.
   - Capture error class, code and message.
   - Collect errors into `std::vector<GdalError>`.
   - Expose the last error code, last error message, detailed error message and full error list.
6. Implement `GdalProgressAdapter`:
   - Adapt `GDALProgressFunc` to a C++ callback such as `std::function<bool(double, const char*)>`.
   - Support cancellation through a cancellation token.
   - Return the correct GDAL callback result when cancellation is requested.
7. Integrate the new library with the existing native build and test infrastructure.

### Acceptance criteria

- The library builds successfully using the repository's supported build configuration.
- Initialization and shutdown can be exercised repeatedly without duplicate registration or unsafe global cleanup.
- A unit test verifies that a forced `CE_Failure` is captured correctly.
- A progress test verifies progress reporting from the start to completion of a suitable operation.
- A cancellation test verifies that the operation aborts cleanly and reports cancellation.
- Existing tests and unrelated functionality continue to work.

Do not begin Phase 1 until Phase 0 is complete and reviewed.

## 5. Phase 1 — Generic Algorithm Runner

Implement `GdalAlgorithmRunner` in `MwGdalCore`.

The runner must instantiate GDAL algorithms using paths such as:

- `raster convert`
- `raster reproject`
- `vector convert`

Provide typed argument-setting overloads for strings, booleans, integers, doubles, vectors of doubles and vectors of strings.

Provide `ParseCommandLineArgs()` as a compatibility mechanism for traditional GDAL command-line flags.

Provide:

- `Run()` with progress and cancellation support.
- `Finalize()` for flushing and finalizing successful executions.
- `TakeOutputDataset()` for transferring ownership of output datasets.
- Access to structured errors.
- `UsageAsJson()` to expose GDAL algorithm usage metadata.

Implementation requirements:

1. Instantiate algorithms through `GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate(...)`, using the actual API supported by the pinned GDAL version.
2. Check the return value of every argument-setting operation.
3. Record relevant GDAL error information when argument setting fails.
4. Permit `Run()` at most once per runner instance.
5. Always call `Finalize()` after a successful `Run()`.
6. Treat an operation as successful only when both `Run()` and `Finalize()` succeed.
7. Propagate errors from both execution and finalization.
8. Ensure dataset ownership and cleanup are unambiguous.
9. Return structured errors for unknown algorithms, invalid arguments and execution failures rather than crashing.
10. Use a fresh algorithm instance for every execution or retry.

### Public façade

Expose a generic COM-facing API consistent with the actual MapWinGIS6 interop architecture. Its conceptual shape is:

`RunAlgorithm(algorithmPath, argNames, argValues, openOptions, errorDetails)`

The API must make algorithms added in later GDAL versions accessible without requiring a new dedicated wrapper for each one.

Use the appropriate IDL, COM types and existing conventions. Do not copy the conceptual C# signature literally if it is incompatible with the repository's COM architecture.

### Acceptance criteria

- Translate a test GeoTIFF to PNG and compare its output against a known reference or equivalent GDAL CLI result.
- Verify that legacy command-line flags produce equivalent output to the corresponding typed argument path.
- Verify structured error reporting for unknown algorithms and invalid argument names.
- Verify that repeated execution on the same runner is rejected safely.
- Verify finalization failures are reported correctly.
- Verify output dataset ownership and cleanup.
- Verify the public API works through the repository's actual interop mechanism.

Stop after Phase 1 and prepare a pull request.

## 6. Phase 2 — Strongly Typed Option Structures

Introduce typed option structures with sensible defaults, validation and `ToArgs()` conversion.

Start with the highest-value operations.

### 6.1 Raster conversion

Create `RasterConvertOptions` for `raster convert`, including applicable options for:

- Output format and data type.
- Selected bands and NoData value.
- Georeferenced and pixel-based source windows.
- Output dimensions and resolution.
- Resampling.
- Creation and open options.
- Overview selection and statistics.

Use existing MapWinGIS6 geometry types where appropriate. If a suitable type does not exist, introduce a narrowly scoped type rather than duplicating existing functionality.

### 6.2 Priority algorithms

Implement typed APIs in this order, adjusting only when required by actual GDAL API support:

| GDAL algorithm | MapWinGIS5 equivalent |
|---|---|
| `raster convert` | `GdalRasterTranslate` |
| `raster reproject` | `GdalRasterWarp` |
| `vector convert` | `GdalVectorTranslate` |
| `raster overview add` | `GdalBuildOverviews` |
| `raster info` | New functionality |
| `vector info` | New functionality |
| `raster mosaic` | New functionality |
| `vector clip` | `ClipVectorWithVector` |
| `raster clip` | New functionality |
| `raster calc` | New functionality |

For each typed wrapper:

1. Provide a clearly defined options structure.
2. Implement `ToArgs()`.
3. Validate the resulting arguments against GDAL's algorithm usage metadata.
4. Use GDAL's own validation for mutually exclusive or otherwise invalid arguments wherever possible.
5. Return actionable, structured errors.
6. Add unit and integration tests.

### Acceptance criteria

- Typed options successfully execute their corresponding algorithms.
- Invalid and mutually exclusive options are rejected before execution where validation permits.
- `ToArgs()` output is validated against `UsageAsJson()`.
- Defaults and optional values behave consistently.
- Tests cover argument conversion and representative successful and unsuccessful executions.

Stop after Phase 2 and prepare a pull request.

## 7. Phase 3 — Backward Compatibility

Implement the relevant MapWinGIS5-compatible methods on top of the new runner:

- `GdalRasterTranslate`
- `GdalRasterWarp`
- `GdalVectorTranslate`
- `GdalBuildOverviews`
- `ClipVectorWithVector`
- `GdalVectorReproject`

Adapt the signatures to the actual MapWinGIS6 API and interop conventions.

Requirements:

1. Retain legacy parameter and error semantics wherever feasible.
2. Mark obsolete APIs appropriately, using the correct mechanism for native C++, COM/IDL and any .NET façade.
3. Route operations through the new algorithm runner or typed wrappers.
4. Preserve legacy flag compatibility where specified.
5. Map legacy error properties such as `LastErrorCode` and `ErrorMsg(code)` to the new error subsystem.
6. Document incompatible behavior explicitly rather than silently changing it.
7. Add a migration guide section titled **From GdalUtils (v5) to GdalUtils2 (v6)** with before-and-after examples.

### Acceptance criteria

Compile and run tests covering all legacy methods, including:

- Raster translation with output data type, resolution, resampling and projection-window options.
- Shapefile reprojection from EPSG:4326 to EPSG:28992.
- Vector clipping using another vector dataset.

Use small test fixtures and compare results with trusted reference output.

Stop after Phase 3 and prepare a pull request.

## 8. Phase 4 — Modern GDAL Capabilities

Implement these features incrementally.

### 8.1 Cloud and virtual filesystems

- Support GDAL VSI paths directly, including `/vsicurl/`, `/vsis3/`, `/vsizip/` and `/vsiaz/`, subject to driver and build support.
- Provide a helper for constructing VSI paths where useful.
- Support cloud configuration options without hard-coded credentials.
- Document credential handling and configuration scope.
- Do not log secrets or include credentials in test fixtures.

### 8.2 Cloud-Optimized GeoTIFF

- Provide a `WriteCog()` convenience API backed by the appropriate GDAL raster conversion functionality.
- Support COG creation options and appropriate compression, tiling and overview behavior.
- Verify actual defaults against the pinned GDAL version.
- Test generated COGs for correctness.

### 8.3 VRT and in-memory processing

- Support MEM and VRT output datasets.
- Enable compatible MapWinGIS6 raster layers to adopt or consume datasets without temporary files.
- Implement VRT-based mosaicking through `raster mosaic`.
- Define dataset ownership and lifetime explicitly.

### 8.4 Pipelines

Where supported by the selected GDAL version, expose a `RunPipeline()` API accepting an ordered collection of algorithm steps.

Each step contains an algorithm path and arguments. Pass dataset outputs between steps using the appropriate GDAL dataset argument representation, such as `GDALArgDatasetValue`, where supported.

Test a two-step pipeline that passes an intermediate dataset in memory without creating temporary files.

### 8.5 Multidimensional and Zarr support

Treat this as a stretch goal.

- Investigate applicable `gdal mdim` algorithms and Zarr driver availability.
- Add read-only support where feasible.
- Use version guards when necessary.
- Do not introduce UI work.

### Acceptance criteria

- Read a COG from a public `/vsicurl/` test source.
- Create a COG and verify its content and overview behavior.
- Execute a two-step in-memory pipeline.
- Test VRT mosaicking.
- Test optional capabilities only when supported by the pinned GDAL build.
- Ensure unavailable drivers or capabilities produce clear errors.

Use stable public test data and keep integration tests deterministic where possible. Do not make the entire test suite dependent on network availability.

Stop after Phase 4 and prepare a pull request.

## 9. Phase 5 — Error Handling, Logging and Cancellation

Complete the operation-result model and integration with MapWinGIS6.

The conceptual result should contain:

- `Success`
- `LastErrorCode`
- `ErrorMsg`
- `DetailedErrorMsg`
- `AllErrors`

Requirements:

1. Ensure every public operation returns consistent success and failure information.
2. Route GDAL warnings (`CE_Warning`) to the existing MapWinGIS6 logging mechanism.
3. Preserve detailed GDAL diagnostic information where useful.
4. Propagate cancellation through the native and public API layers.
5. Implement `BeginGdalOperation` / `EndGdalOperation` only if they fit the existing asynchronous operation model.
6. Ensure progress notifications are marshalled correctly across thread boundaries.
7. Avoid deadlocks, re-entrant callbacks and use-after-free errors.
8. Serialize GDAL calls using the existing MapWinGIS global lock or an appropriately scoped mutex, consistent with established threading behavior.
9. Document whether cancellation is distinguishable from other failures.

### Acceptance criteria

Tests cover success, warnings, failures, cancellation and finalization errors. Verify that error state is not accidentally shared between concurrent operations.

Stop after Phase 5 and prepare a pull request.

## 10. Phase 6 — Tests, Samples and Documentation

Complete the testing and documentation required for a maintainable release.

### Test data

Add small, redistributable fixtures to `testdata/`:

- Three small rasters.
- One shapefile dataset, including its required companion files.

Keep the total test data below approximately 1 MB where practical.

Document fixture provenance and licensing.

### Automated tests

Use the repository's existing test framework, preferably Google Test or Catch2 if appropriate.

Cover:

- Initialization and configuration.
- Error capture.
- Progress and cancellation.
- Generic algorithm execution.
- Argument validation.
- Typed options.
- Legacy compatibility.
- Raster and vector transformations.
- Dataset ownership.
- In-memory processing.
- COG, VSI and pipeline capabilities.
- Unsupported driver and version scenarios.

### Integration tests

Where practical, compare results against reference output generated using the corresponding GDAL CLI operation.

Use checksums or suitable semantic comparisons, taking into account nondeterministic metadata and differences that do not affect the actual raster or vector data.

Tests must not pass merely because an operation returns success; verify the output.

### Samples

Provide examples following the existing repository conventions, including equivalents of:

- `samples/Cpp/translate.cpp`
- `samples/CSharp/...`

Mirror relevant MapWinGIS5 documentation examples to help existing users migrate.

### Documentation

Create `docs/gdal-integration.md` covering:

- Architecture and responsibilities.
- GDAL version pinning and build requirements.
- Installation and driver availability.
- Generic algorithm execution.
- Typed API usage.
- Progress and cancellation.
- Error handling.
- In-memory datasets and pipelines.
- Cloud paths and credential configuration.
- Migration from MapWinGIS5.
- Limitations and troubleshooting.

Document axis-order behavior under GDAL 3 and explain any compatibility settings.

Where practical, generate algorithm reference documentation from `UsageAsJson()` rather than maintaining duplicated option lists manually.

### Acceptance criteria

- All required unit and integration tests pass.
- Build and test instructions work from a clean checkout.
- Samples compile where supported by the repository's build environment.
- Documentation matches the actual implementation.
- No unrelated regressions are introduced.

Stop after Phase 6 and prepare a pull request.

## 11. Phase 7 — ActiveX Wrapper and COM Progress Dispatch

Expose the native GDAL integration through the ActiveX wrapper.
COM progress dispatch delivers worker notifications to the owning COM/UI thread.
Keep GIS behavior, GDAL execution, and dataset ownership in the native core.
The wrapper owns COM translation, host integration, and notification delivery.

### Dependencies and scope

Complete and review Phases 0–6 before starting this phase.
Inspect the existing ActiveX implementation and determine whether an asynchronous operation model is available; none is currently provided for GDAL.
Follow `AGENTS.md`, `docs/ARCHITECTURE.md`, and the applicable ActiveX plan.
Build on the existing `MapWinGIS.ocx` and .NET 10 `AxHost` test host. Do not recreate COM activation, control lifecycle, vector loading, or rendering, and do not create a second control.

Keep the native dispatch component testable without COM activation.
Full legacy control parity, rendering backends, menus, and dialogs remain outside this phase.
Do not move COM, ATL, MFC, or windowing dependencies into `MapWinGIS.Core` or `MwGdalCore`.

### COM-facing GDAL API

1. Expose generic algorithm execution through the wrapper using COM-compatible arguments and results.
2. Reuse the native runner, typed options, and compatibility utilities without duplicating execution logic.
3. Inspect the MapWinGIS 5 type library before choosing interface identifiers, signatures, enums, or DISPIDs.
4. Preserve existing identifiers only for compatible interfaces.
5. Translate native results and exceptions into documented COM error information.
6. Expose success, cancellation, error code, summary, detailed diagnostics, and warnings.
7. Document intentional compatibility gaps and ownership rules.

Do not expose native pointers, datasets, C++ callbacks, or standard-library objects through COM.
Keep dataset ownership in the core and operation state alive until native execution ends.
Convert text explicitly between COM strings and UTF-8.

### Progress dispatch and cancellation

1. Run asynchronous GDAL work outside the owning COM/UI thread.
2. Copy progress values and messages into notifications with explicit operation identifiers.
3. Deliver notifications through the wrapper's dispatcher on the owning COM/UI thread.
4. Use COM marshalling when an interface reference must cross thread boundaries.
5. Never invoke an apartment-bound COM callback directly from a GDAL worker.
6. Bound pending progress notifications through coalescing or another documented queue policy.
7. Preserve terminal completion notifications when intermediate progress notifications are coalesced.
8. Connect COM cancellation requests to `GdalCancellationToken`.
9. Keep cancellation non-blocking while the worker holds the native execution lock.
10. Distinguish user cancellation from callback errors, GDAL failures, and finalization failures.

Never wait for UI callbacks while holding the native execution lock.
Dispatch COM events after releasing locks that event handlers can re-enter.
Define the behavior for event-handler re-entry and repeated cancellation.
Document that drivers without progress callbacks cannot guarantee prompt cancellation.

### Operation and control lifetime

Use the existing asynchronous operation model if one exists.
Add `BeginGdalOperation` / `EndGdalOperation` only when those names and semantics fit that model.
Otherwise, document the equivalent start, completion, and cleanup contract.

1. Keep the worker, cancellation token, and native result alive until execution and finalization end.
2. Associate each notification with its operation and control lifetime.
3. Discard stale notifications after operation replacement or control destruction.
4. Request cancellation when the control closes or the host disconnects.
5. Release callback references and queued notifications on the correct thread.
6. Prevent DLL unload while native workers still execute.
7. Guarantee at most one terminal notification for each accepted operation.
8. Document shutdown behavior without requiring a worker to wait for a blocked UI thread.

### Acceptance criteria

- A COM integration test executes a raster conversion and checks the output pixels.
- Tests cover generic arguments, invalid input, warnings, failures, and native-to-COM error translation.
- A worker-thread test proves that progress and completion reach the owning COM/UI thread.
- Tests cover cancellation before execution, during progress, and during shutdown.
- Cancellation remains distinct from callback, execution, and finalization failures.
- Tests cover callback re-entry, callback disconnection, and repeated cancellation.
- Tests cover concurrent operations without shared error state or mismatched notifications.
- Tests cover control destruction with pending notifications and an active worker.
- Tests detect stale callbacks, duplicate completion, resource leaks, deadlocks, and use-after-free errors.
- Native dispatch tests run without COM activation.
- COM activation and host tests use an isolated test setup without changing machine-wide registration.
- Existing native tests pass, and supported wrapper builds introduce no new Level 4 warnings.
- Documentation and a client sample show execution, progress, cancellation, results, and cleanup.

Stop after Phase 7 and prepare a pull request.
Run final verification only after this phase is complete and reviewed.

## 12. Cross-Cutting Implementation Rules

Apply these rules throughout all phases.

### GDAL execution and lifecycle

- Always pair a successful `Run()` with `Finalize()`.
- Both operations must succeed for the overall operation to be successful.
- Create a fresh algorithm instance for each execution.
- Never execute the same algorithm instance more than once.
- Check and report every argument-setting failure.

### Validation

- Prefer GDAL's own `ValidateArguments()` and usage metadata.
- Do not duplicate GDAL validation logic unnecessarily.
- Surface useful validation messages to callers.
- Add extra validation only when required by the typed API or repository conventions.

### Coordinate systems

- Accept EPSG identifiers, WKT and PROJ strings where applicable.
- Document GDAL 3 axis-order behavior.
- Decide whether legacy traditional GIS axis order is required during Phase 0.
- Add regression tests with known coordinates.

### Encoding and paths

- Support UTF-8 paths.
- Use appropriate GDAL string types and conversion utilities.
- Avoid unsafe ANSI conversions.

### Threading

- Follow existing MapWinGIS6 threading and locking conventions.
- Serialize GDAL operations using the appropriate global lock or mutex.
- Never assume all GDAL drivers are thread-safe.
- Marshal progress callbacks correctly across COM and UI thread boundaries.
- Do not allow C++ lambdas or other unsupported C++ objects to cross COM boundaries. Use suitable C-compatible callback signatures and explicit lifetime management.

### Memory and resources

- Use RAII for resources and handles.
- Define ownership transfer explicitly for output datasets.
- Restore scoped global configuration changes.
- Ensure exceptions or early returns cannot leave error handlers, locks or global state unbalanced.
- Avoid leaking datasets or destroying shared GDAL state prematurely.

### Code quality

- Follow repository naming, formatting, documentation and testing conventions.
- Prefer small, focused changes.
- Avoid unrelated refactoring.
- Do not introduce unnecessary dependencies.
- Do not add placeholder implementations that report success without performing the operation.
- Do not suppress warnings or weaken tests simply to obtain a successful build.

## 13. Pull Request Workflow

Every phase must be delivered as a separate, reviewable pull request.

For the current phase:

1. Confirm the intended base branch and follow the repository's branch and PR instructions.
2. Implement only the current phase and its prerequisites.
3. Build the affected projects.
4. Run relevant unit and integration tests.
5. Run existing regression tests where practical.
6. Inspect the final diff for unrelated changes, resource-management defects and incomplete implementations.
7. Update relevant documentation.
8. Update the progress checklist, marking only verified work complete.
9. Summarize the implementation, files changed, tests run and any limitations.
10. Create a pull request against the intended development branch if repository access and available tools permit it.

If a test or build cannot be run, state exactly why and do not claim it passed. If the environment cannot create a PR, prepare the changes and provide the proposed PR title, description and remaining steps instead.

**Stop after the pull request is ready. Wait for human review and approval before starting the next phase.**

Do not automatically merge the PR.

## 13. Required Completion Report

At the end of the current phase, provide a concise report containing:

1. **Phase:** Number and name.
2. **Implementation:** What was added or changed.
3. **Architecture:** How the changes fit into the existing MapWinGIS6 structure.
4. **Tests:** Commands or test configurations used, with actual results.
5. **Acceptance criteria:** Each criterion marked as passed, failed or not verified.
6. **Checklist:** Updated project checklist.
7. **PR:** Link if successfully created, otherwise the proposed title and description.
8. **Outstanding issues:** Any blockers, known limitations or decisions requiring human input.
9. **Next phase:** A brief description only. Do not start implementing it.

Begin with the repository investigation and Phase 0 only.
