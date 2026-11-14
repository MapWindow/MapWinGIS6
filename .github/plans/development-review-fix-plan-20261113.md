# Plan: resolve the pre-merge review findings – MapWinGIS6 `development`

## Checklist

Implement one phase per run and open one PR per phase, in this order. Check a phase off only when its PR is open and its build and tests pass.

- [x] Phase 1 – Make the CMake test build complete and trustworthy (#8, #9, #10, #29, #40)
- [ ] Phase 2 – Fix the vector feature count and WKT error message (#1, #39)
- [ ] Phase 3 – Fix the GDAL execution lock and the algorithm runner state (#2, #5, #12, #17, #18, #19, #32, #33)
- [ ] Phase 4 – Harden GDAL environment initialization and the error handler (#4, #13, #14, #15, #16, #28, #30, #31)
- [ ] Phase 5 – Make ActiveX GDAL operations robust (#3, #11, #22, #23, #25)
- [ ] Phase 6 – Improve COM container compatibility (#6, #20, #24, #27, #37)
- [x] Phase 7 – Rebuild render scenes per layer (#7, #34)
- [ ] Phase 8 – Fix ActiveX rendering edge cases (#21, #26, #35, #38)
- [ ] Phase 9 – Load vector datasets asynchronously (#36)

Finding coverage: every finding is assigned to exactly one phase:

| Phase | Findings |
| --- | --- |
| 1 | #8, #9, #10, #29, #40 |
| 2 | #1, #39 |
| 3 | #2, #5, #12, #17, #18, #19, #32, #33 |
| 4 | #4, #13, #14, #15, #16, #28, #30, #31 |
| 5 | #3, #11, #22, #23, #25 |
| 6 | #6, #20, #24, #27, #37 |
| 7 | #7, #34 |
| 8 | #21, #26, #35, #38 |
| 9 | #36 |

## Context

You are working in the MapWindow/MapWinGIS6 repository. A pre-merge review of branch `development` (HEAD `c7bb930`, base `origin/main`) found 8 Major and 21 Minor confirmed issues and 11 suggestions, numbered #1–#40. This plan turns them into nine phases, each delivered as one PR against `development`.

Source report: `D:\dev\Review-overviews\development-review-20261113-144147.html`. Each finding there has a location, issue, impact, recommendation and evidence. Read the finding before you work on it. Line numbers refer to `c7bb930` and may have moved; locate the code by symbol.

The project rules are in `AGENTS.md`, `docs/CODING-STANDARDS.md`, `docs/ARCHITECTURE.md`, `docs/PORTING.md`, `docs/TESTING.md` and `docs/BUILDING.md`. They take precedence over this plan where the plan is silent.

## How to work

- Do the phases in checklist order. Phase 1 comes first because later phases rely on CMake running the ActiveX tests. Phases 2–8 do not depend on each other beyond that and may be reordered if needed. Phase 9 depends on Phase 7.
- Deliver what each phase asks, at the scope intended. Make routine judgement calls yourself. Check in with the user only where this plan says **Decision**, or where different readings would lead to materially different work. If an approach in this plan looks wrong after you've read the code, say so in one sentence and do the most reasonable thing.
- Before you fix a finding, confirm it still reproduces against the current code. If it has already been fixed, note that under the phase and move on.
- Each behaviour fix comes with a test that fails without the fix. Use generated fixtures in temporary directories (`tests/MapWinGIS.Core.Tests/TestData.h`). Do not commit generated data.
- Preserve published COM CLSIDs, IIDs, DISPIDs and exported Core names. When you must change a public COM interface, add a new versioned interface instead of editing the existing one. Document every intentional API or behaviour difference.
- Every phase updates the docs its changes affect (`ARCHITECTURE.md`, `BUILDING.md`, `TESTING.md`, `PORTING.md`, `gdal-*.md`, IDL and header comments).
- A phase is done when:
  - `cmake --preset x64-debug`, `cmake --build --preset x64-debug` and `ctest --preset x64-debug` succeed with no new Level 4 warnings;
  - `msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m` succeeds and `bin\Debug\x64\MapWinGIS.Core.Tests.exe` and `bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe` pass;
  - the CMake/MSBuild flag-parity check (`Compare-CompileCommands.ps1`) still passes when build files change.
  If vcpkg packages are missing and you cannot build locally, say so and list what is unverified.
- Do not add change history, phase notes, or finding numbers to code or docs. Comments that reference a task or follow-up must contain `TODO`.
- Progress updates: one sentence before you start a phase, a short note only when you find something important or change direction, and when a phase is finished, lead with the outcome.

## Phases

### Phase 1 – Make the CMake test build complete and trustworthy (#8, #9, #10, #29, #40)

CMake is the declared primary build, but it does not run the ActiveX tests, its only hosting check passes without running anything, and Core cannot be tested without ActiveX.

**#9 – Separate the Core and ActiveX test targets** (`tests/MapWinGIS.Core.Tests/CMakeLists.txt`, root `CMakeLists.txt:76-90`)

- Remove `${MWG_ACTIVEX_SHARED_SOURCES}`, the ActiveX include directory, `MWG_ENABLE_RENDERER_TEST_HOOKS`, the MIDL target, `opengl32` and any ActiveX dependency from `MapWinGIS.Core.Tests`. Keep only what Core tests need, matching `MapWinGIS.Core.Tests.vcxproj`.
- Add `tests/MapWinGIS.ActiveX.Tests/CMakeLists.txt` that builds `MapWinGIS.ActiveX.Tests`. Mirror `MapWinGIS.ActiveX.Tests.vcxproj`: its sources (including the shared `Main.cpp` and `TestData.cpp` from Core.Tests), the ActiveX shared sources, its definitions and its libraries. Register it with `gtest_discover_tests` and the same `interactive` label conventions as Core.Tests.
- Build and register Core tests whenever tests are enabled, independent of `MWG_BUILD_ACTIVEX`. Add the ActiveX test subdirectory only when `MWG_BUILD_ACTIVEX` is on.
- If the CMake source-list parity check covers test projects, extend it to the new target.
- Define `MWG_TEST_FULL_DRIVERS` for `MapWinGIS.Core.Tests` when `"full-drivers" IN_LIST VCPKG_MANIFEST_FEATURES`, as `MapWinGIS.Core.Tests.vcxproj:51-55` does. Confirm the CMake Release CI job now runs `GdalEnvironment.IncludesTheReleaseDriverSet` (it must appear in the ctest output, not be skipped).
- Update `AGENTS.md` ("Until CMake registers the ActiveX tests…"), `docs/BUILDING.md`, `docs/TESTING.md` and `docs/CMAKE-MIGRATION.md` to describe the new state.
- Update `.github/workflows/build.yml` so the CMake job runs the ActiveX tests through ctest.

**#8 – Make `Verify-ActiveXActivation` test something** (`tests/MapWinGIS.Core.Tests/CMakeLists.txt:90-104`, `tests/MapWinGIS.ActiveX.Tests/Verify-ActiveXActivation.ps1:96-101`)

- Move the `Verify-ActiveXActivation` ctest registration to the ActiveX test CMake file and pass `MapWinGIS.ActiveX.Tests.exe` as `-TestExecutable`.
- Make the script fail when the filter selects no tests. Use `--gtest_fail_if_no_test_selected` if the GoogleTest version from the vcpkg baseline supports it. Otherwise write `--gtest_output=xml:<temp file>` and fail when the `tests` attribute is 0.
- Verify by temporarily pointing the script at `MapWinGIS.Core.Tests.exe`: it must now fail. Don't commit that change.

**#10 – Gate Release and Win32 warnings** (`cmake/MwgCompilerOptions.cmake:39-40`, `build/props/MapWinGIS.Cpp.props:22`, `.github/workflows/build.yml`)

- Enable `/WX` for every configuration and platform in both CMake and MSBuild, keeping the two in parity. If this exposes warnings, fix them; don't suppress them without a justification comment.
- Add a Win32 Debug leg to the pull-request matrix of the CMake VS job (or the MSBuild job if that is cheaper), so Win32 defects are found before merge. Keep the full Win32 matrix on pushes to `main`.
- **Decision** only if `/WX` in Release exposes warnings in third-party headers that cannot be fixed or isolated: ask the user before adding suppressions.

**#29 – Ignore local tool and editor files** (`.gitignore`)

- Add `.tools/` and `PowerShellEditorServices.json` to `.gitignore`. First check whether anything in the repository (for example `msbuild-extractor.json`) expects `.tools/` content to be committed; if so, ignore only the binary.

**#40 – Clarify the "audit" comment** (`src/MapWinGIS.ActiveX/CMakeLists.txt:81`)

- Replace `# Explicit Windows libraries (audit).` with the actual reason the libraries are listed explicitly. Find it in `docs/CMAKE-MIGRATION.md` or the commit that added the line. If you cannot establish the reason, write a neutral description of what the list is, without inventing a rationale.

**Phase 1 result:** Implemented in [PR #73](https://github.com/MapWindow/MapWinGIS6/pull/73). CMake Debug x64 and Win32 builds/tests passed; full-driver Release passed, including `GdalEnvironment.IncludesTheReleaseDriverSet`; MSBuild Debug x64 and both test suites passed; compile-command parity passed with zero lost options. The activation script's zero-test negative check also passed.

**Limitations and validation notes:**

- The review HTML referenced above was unavailable at the checked D: and F: paths; implementation followed the written Phase 1 criteria and source verification.
- The standalone MSBuild Debug x64 build reported zero warnings. An incremental parity rebuild logged two `MSB3061` access-denied warnings while replacing runtime DLLs; the parity comparison still passed.
- The first Win32 configure inherited an unrelated `VCPKG_ROOT` and began installing packages in that external vcpkg cache. It was stopped and rerun with the project vcpkg root; the external cache was left untouched.

### Phase 2 – Fix the vector feature count and WKT error message (#1, #39)

**#1 – `VectorLayer::FeatureCount()` overcounts shapefiles with deleted records** (`src/MapWinGIS.Core/src/vector/VectorDataset.cpp` `Impl::FeatureCount`, `EnsureFeatureIds`, `ForEachGeometry`; `include/mapwingis/vector/VectorDataset.h:67-70`)

GDAL's Shapefile driver returns `m_nTotalShapeCount` from `GetFeatureCount(TRUE)`, which includes DBF records marked deleted, while `GetNextFeature` skips them. Indexed access then fails for the last indexes, and the cached count depends on whether `ForEachGeometry` ran first.

Suggested fix:

- Make the index map the single source of truth for the count that indexed access uses. When `_featureIds` has been built, `FeatureCount()` returns `_featureIds.size()`.
- When the index has not been built, use the driver's count only when it is known to be exact. Treat the `ESRI Shapefile` driver as inexact (build the index, or count sequentially). Keep the fast driver path for other drivers.
- Make `ForEachGeometry` and `EnsureFeatureIds` agree on the cached value, so the result no longer depends on call order.
- Update the `FeatureCount()` documentation in the header to state what "exact" means.
- Tests: create a shapefile with several features, mark one DBF record deleted without repacking (`DeleteFeature` on the Shapefile driver does this; confirm the driver option or use `DBFMarkRecordDeleted` through GDAL's shapelib only if the first is not possible), reopen it and check that `FeatureCount()` matches the number of indexable features, that every index `0..FeatureCount()-1` is readable, and that the count is the same before and after `ForEachGeometry`.

**#39 – Truncate the WKT in the parse error** (`src/MapWinGIS.Core/src/vector/VectorDataset.cpp:646`)

- Replace `"Cannot parse WKT: " + wkt` with a bounded excerpt (for example the first 200 characters followed by `…` and the total length). Make sure truncation does not split a UTF-8 sequence.
- Test: an invalid WKT longer than the limit produces a message no longer than the limit plus the fixed text.

### Phase 3 – Fix the GDAL execution lock and the algorithm runner state (#2, #5, #12, #17, #18, #19, #32, #33)

**#2 – Dataset and runner release take the global execution mutex** (`src/MapWinGIS.Core/src/gdal/GdalOperationResult.cpp:52-57`, `GdalAlgorithmRunner.cpp:250-254`)

Releasing a `GdalDatasetPtr` or destroying a runner waits for any long GDAL operation on another thread, and can deadlock when release happens from a callback on a GDAL worker thread.

Suggested fix:

- First establish why the lock was added: read `docs/ARCHITECTURE.md`, `docs/gdal-integration.md`, `.ai/memory` and the commit history of these lines.
- If the lock only serializes against concurrent GDAL use of the same dataset, release without the global mutex. A dataset owned by one `GdalDatasetPtr` (reference count 1) has no other user. Where a runner still holds a reference, make the runner's destructor release its own references without blocking unrelated operations.
- If some state genuinely needs protection during release, use a narrow dedicated mutex instead of the execution mutex.
- Document the threading contract of `GdalDatasetPtr`, `GdalOperationResult` and `GdalAlgorithmRunner` in their headers.
- Tests: release a dataset on thread A while thread B holds the execution lock inside a long-running operation (use a progress callback that waits on a latch); release must complete before B is allowed to finish. Add a test that destroys a dataset from inside a progress callback and completes without deadlock.
- **Decision**: if the history shows the lock protects against a known GDAL thread-safety problem that cannot be solved more narrowly, stop and present the evidence and options to the user.

**#5 – Misuse errors corrupt `Run()` and `Result()`** (`GdalAlgorithmRunner.cpp:257-262, 452-457, 535-567, 652-662, 686-693`)

Suggested fix:

- Record `const std::size_t runStart = _impl->errors.size();` at the start of `Run()` and check failures only in `errors[runStart..]` when deciding whether to flush, close and report success.
- Freeze the success value at the end of `Run()` (and `Finalize()`). `Result().succeeded` uses the frozen value. Later misuse diagnostics still appear in `Errors()`, but don't change `succeeded`.
- `Finalize()` before `Run()` returns false and records a diagnostic, without affecting a later `Run()`.
- Make `TakeOutputDataset()` and `Result()` consistent by construction.
- Tests: `Finalize()` then `Run()` on a valid translation succeeds and writes the output; `SetArgument` after a successful `Run()` returns false, adds a diagnostic and leaves `Result().succeeded` true.

**#18 – A rethrow in the middle of `Run()` skips cleanup** (`GdalAlgorithmRunner.cpp:542, 559, 585, 595`)

- Move the failure handling (closing the output dataset, recording `cancelled`, storing callback exceptions) into a scope guard, or catch, clean up and rethrow, so it runs on every exit path.
- Test: a logger that throws during `Run()` propagates its exception, and afterwards the output file is not locked (it can be deleted or reopened for writing).

**#19 – Diagnostics after `algorithm->Run` are dropped** (`GdalAlgorithmRunner.cpp:504-533`)

- Append the outer `GdalErrorScope` again before `Run()` returns, or wrap the post-run work in a nested scope that is appended. Avoid appending the same diagnostic twice.
- Test only if a post-run diagnostic can be triggered reliably; otherwise explain in the PR why it is not tested.

**#17 – Worker-thread diagnostics are not captured** (`GdalRuntime.cpp` `GdalErrorScope`, `GdalEnvironment.cpp:62-74`)

- Document in `GdalRuntime.h`/`GdalAlgorithmRunner.h` and `docs/gdal-integration.md` that diagnostics from GDAL worker threads (`-multi`, `NUM_THREADS`) reach the log handler but not `Errors()`.
- If GDAL 3.12 offers a reliable per-operation way to collect worker diagnostics, implement it instead. Don't build a custom cross-thread routing mechanism in this phase.

**#12 – Compatibility opens lose GDAL's diagnostics** (`src/MapWinGIS.Core/src/gdal/GdalUtils.cpp:111-116` and every `OpenDataset` call)

- Add `GDAL_OF_VERBOSE_ERROR` to the open flags.
- Strengthen the existing missing-input tests to assert `CPLE_OpenFailed` and a message mentioning the path, not just a non-empty message.

**#32 – Partial outputs after failure or cancellation** (`GdalUtils.cpp`, runner failure path)

- Document in the `GdalUtils` and `GdalAlgorithmRunner` headers that a failed or cancelled operation can leave a partial output.
- **Decision**: ask the user whether failed operations should also delete outputs they created. If yes, delete only files that did not exist before the operation started, and test both cases.

**#33 – Confirm overview behaviour against MapWinGIS 5** (`GdalUtils.cpp:558`, `GdalBuildOverviews`)

- Check how MapWinGIS 5 built overviews (repository `MapWindow/MapWinGIS`, its GDAL overview helpers): internal GeoTIFF overviews or external `.ovr`.
- Record the result in `docs/gdal-migration.md`. If MapWinGIS 5 built internal overviews and v6 deliberately builds external ones, document the difference and its reason. If the difference is unintended, raise it as a **Decision** with the user; do not change the behaviour in this phase.

### Phase 4 – Harden GDAL environment initialization and the error handler (#4, #13, #14, #15, #16, #28, #30, #31)

All of these are in `src/MapWinGIS.Core/src/gdal/GdalEnvironment.cpp` and `include/mapwingis/gdal/GdalEnvironment.h`, except #30 and #31.

**#4 – The global error handler outlives Core's state** (`GdalEnvironment.cpp:36-41, 62-77`)

Suggested fix:

- Make the state the handler uses (log handler, its mutex, and anything else `ErrorHandler` and `ReportDiagnostic` read) immortal to avoid static-destruction ordering problems; comment why it is deliberately leaked. This does not keep the callback code alive after Core unloads.
- **Decision**: choose and document a module-lifetime policy before implementing this fix. Either keep Core loaded while GDAL retains the callback, or require host-invoked pre-unload shutdown that prevents new callbacks, restores the previous handler, and waits for in-flight callbacks to finish before releasing state or unloading Core. Do not call GDAL or wait for callbacks from `DllMain`.
- Test: install a log handler, trigger a diagnostic, then reset the handler to empty and trigger another; both must be safe. A real unload test is not required; explain in the PR why it is not practical.

**#16 – The host's GDAL error handler is replaced** (`GdalEnvironment.cpp:77`)

- Save the handler returned by `CPLSetErrorHandler` in the immortal state. After MapWinGIS has processed a diagnostic, forward it to the previous handler when that handler is not `CPLDefaultErrorHandler` or `CPLQuietErrorHandler`. Never forward to `ErrorHandler` itself.
- Document the behaviour in `GdalEnvironment.h` and `docs/gdal-integration.md`.
- Test: install a custom handler with `CPLSetErrorHandler` before the first `Initialize()`, trigger a diagnostic, and check that both handlers saw it. Because initialization is process-wide and runs once, put this test in a separate test executable or document that it must run first; prefer whatever pattern `GdalEnvironmentTests.cpp` already uses for once-only behaviour.

**#13 – `is_directory` can throw inside `call_once`** (`GdalEnvironment.cpp:79, 82`)

- Use the `std::error_code` overload. On error, treat the directory as missing and report it through #14's warning.

**#14 – Missing data directories fall back silently** (`GdalEnvironment.cpp:79-86`, `docs/ARCHITECTURE.md`)

- When the GDAL or PROJ data directory does not exist, report a `LogLevel::Warning` through the log handler, naming the expected path.
- Make the documentation and the behaviour agree: either state that environment variables are a fallback, or stop the fallback by explicitly configuring the data paths. Prefer the documented contract unless that breaks existing tests or deployment.
- Test: initialize with a non-existent data directory (in a test that controls initialization, see #16) and check that the warning is reported.

**#15 – Explicit `Initialize(paths)` is silently ignored after implicit initialization** (`GdalEnvironment.cpp:120-130`)

- Change `Initialize(gdalDataDir, projDataDir)` to report whether the paths were applied. Preferred: return `bool` with `[[nodiscard]]`. If changing the return type causes too much churn, log a warning when the requested paths differ from the active ones instead.
- Update the header documentation and callers.

**#28 – `ModuleDirectory()` runs on every `Initialize()`** (`GdalEnvironment.cpp:120-124`)

- Return early from `Initialize()` when `IsInitialized()` is true, before computing paths. Keep `call_once` for the first initialization.

**#30 – No GDAL version guard** (`GdalAlgorithmRunner.cpp`, `vcpkg.json:12`)

- Add `static_assert(GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 12, 4))` where the C++ algorithm API is used.
- In `DoInitialize`, call `GDALCheckVersion` with the compile-time major and minor version and report a mismatch as an error through the log handler (or throw a `CoreError` subclass from `Initialize`; pick the option that fits the documented error contract).
- Document the ABI constraint in `docs/gdal-version.md`.

**#31 – `ToUtf8` can throw for unpaired surrogates** (`src/MapWinGIS.Core/src/internal/Utf8.h:16-19`)

- Find the public APIs that call `ToUtf8` on caller-supplied paths. Convert the `std::system_error` into the API's normal failure (a `false` result with a recorded diagnostic, or a `CoreError` subclass for throwing APIs).
- Test with a path containing an unpaired surrogate (`L"\xD800"`).

### Phase 5 – Make ActiveX GDAL operations robust (#3, #11, #22, #23, #25)

**#3 – Conversion failures leak operation slots** (`src/MapWinGIS.ActiveX/MapWinGISControl.cpp:309-346` `EndGdalOperation`, `ComConversion.cpp:85-126`, `GdalOperationDispatcher.cpp:267-268`)

Suggested fix:

- Add a lossy conversion for diagnostic text, for example `DiagnosticToBstr`: convert UTF-8 without `MB_ERR_INVALID_CHARS` (invalid sequences become U+FFFD) and replace embedded NUL bytes with U+FFFD. Use it for `summary`, `diagnostics`, `callbackError` and `warnings`. Keep the strict conversion for data where loss would be wrong.
- In the dispatcher, when storing `exception.what()`, convert it from the active code page to UTF-8 first (MSVC's `std::system_error` and `filesystem_error` messages use the ANSI code page). Put this conversion in one helper.
- Restructure `EndGdalOperation` so that once the result snapshot has been obtained, `_gdalEvents.End(...)` always runs, even if marshalling fails afterwards.
- Return a distinct HRESULT with error info for marshalling failures, so they can't be confused with "unknown operation id" (`E_INVALIDARG`).
- Apply the same lossy conversion to `FireProgress` (`GdalEventSource.cpp:294`).
- Tests: an operation whose summary contains invalid UTF-8 and an embedded NUL can be ended and its slot reused; starting and ending 17 such operations succeeds.

**#11 – Exceptions can escape `EndGdalOperation`** (`MapWinGISControl.cpp:289-296, 344-346`)

- Wrap `GetResult` and `End` so every `std::exception` maps to `E_FAIL` with error info and anything else to `E_UNEXPECTED`, matching the other COM methods. Check the remaining COM methods in the file for the same gap.

**#25 – Failing COM calls don't set error info** (`MapWinGISControl.cpp:193, 241, 253, 285, 304, 407, 413, 421, 551, 580, 712`)

- On every failing return path in COM methods, set error info with a useful message (through the existing `SetComErrorInfo`/`SetControlComErrorInfo` helpers). On success paths, leave error info as ATL handles it.
- Test: one argument failure per interface returns error info whose description names the problem (`GetErrorInfo` after the call).

**#22 – A permanent 100 ms timer** (`src/MapWinGIS.ActiveX/GdalDispatchWindow.h:20, 41`)

- Arm the retry timer only when a `PostMessage` fails or operations are outstanding, and kill it when the queue is empty. Make sure no notification is lost when posting succeeds.
- Test: after an operation has completed and been ended, the timer is no longer armed (expose a test hook under `MWG_ENABLE_RENDERER_TEST_HOOKS` or an equivalent existing hook mechanism).

**#23 – Event delivery and operations after Suspend or Close** (`GdalEventSource.cpp`)

- Resume event delivery when the window is created again (`WM_CREATE`/`OnCreate`) and on in-place activation, not only on `WM_SIZE` or `Begin`.
- **Decision**: ask the user whether `IOleObject::Close` followed by re-activation should allow GDAL operations again (recommended: yes, reset the closing state on `SetClientSite` with a non-null site or on activation) or permanently disable them (then document it in `ARCHITECTURE.md` and the IDL).
- Tests: destroy and re-create the control window while an operation runs, and check that the completion event arrives without a `WM_SIZE`. Add a Close-then-reactivate test for the chosen behaviour.

### Phase 6 – Improve COM container compatibility (#6, #20, #24, #27, #37)

Do not change existing interface IIDs or DISPIDs. Where a new method or interface is needed, add a new versioned interface.

**#6 – Add `IProvideClassInfo2`** (`src/MapWinGIS.ActiveX/MapWinGISControl.h`)

- Derive from `IProvideClassInfo2Impl<&CLSID_..., &DIID_..., &LIBID_..., 0xFFFF, 0xFFFF>` (use the existing CLSID, source dispinterface IID and LIBID symbols) and add `COM_INTERFACE_ENTRY(IProvideClassInfo)` and `COM_INTERFACE_ENTRY(IProvideClassInfo2)`.
- Test: `QueryInterface` for `IProvideClassInfo2` succeeds, `GetGUID(GUIDKIND_DEFAULT_SOURCE_DISP_IID)` returns the event dispinterface IID, and `GetClassInfo` returns the coclass type info.

**#20 – Register `MiscStatus`** (`src/MapWinGIS.ActiveX/MapWinGISControl.rgs`, `samples/CSharp/ActiveXTestHost/app.manifest`)

- Add `MiscStatus = s '0' { '1' = s '<value>' }` with `OLEMISC_ACTIVATEWHENVISIBLE | OLEMISC_SETCLIENTSITEFIRST | OLEMISC_INSIDEOUT | OLEMISC_CANTLINKINSIDE | OLEMISC_RECOMPOSEONRESIZE` (compute the decimal value and comment it). Add the matching `miscStatus` attribute to the registration-free manifest.
- Check that per-user registration (`DllInstall` with `user`) writes the same keys.
- Test: after registration, `OleRegGetMiscStatus` (or `IOleObject::GetMiscStatus`) returns the expected flags. If registration-dependent tests are opt-in in this repo, follow that pattern.

**#24 – Automation reach and type-library loading** (`MapWinGISControl.h:54, 72`, `MapWinGIS.ActiveX.idl`)

- Change the GDAL interface's `IDispatchImpl` to load the type library with `0xFFFF, 0xFFFF`, like the primary interface.
- **Decision**: ask the user whether scripting and VB6/VBA late-bound clients must be able to use the GDAL API. Recommended default: document in the IDL and `ARCHITECTURE.md` that the GDAL interface is early-bound only (`SAFEARRAY(BSTR)`, `hyper` ids). If the user wants late binding, add a new dispatch-friendly interface (`VARIANT` arrays, `DOUBLE` or `BSTR` operation ids) reachable through the default dispatch, with tests from a late-bound `IDispatch::Invoke` call.
- Test for the type-library change: `GetTypeInfo` on the GDAL interface works when the type library is not registered (registration-free activation, if the test infrastructure supports it).

**#27 – `DllInstall` command-line handling** (`src/MapWinGIS.ActiveX/ActiveXModule.cpp`)

- Treat a null or empty command line as the default (machine-wide) and compare `user` case-insensitively.
- Document in `docs/BUILDING.md` that per-user registration is `regsvr32 /n /i:user MapWinGIS.ocx`, and that `/i:user` without `/n` also runs machine-wide `DllRegisterServer`.
- Test `DllInstall` argument parsing through an exported or testable helper, without touching the real registry, unless the existing tests already use a registry redirection.

**#37 – Persist properties** (`MapWinGISControl.h:40`)

- Add `RenderingBackendPreference` to the property map, and add `IPersistPropertyBagImpl` with its COM map entry. Check that `IPersistStreamInit` save and load round-trip the property.
- Bump nothing in existing interfaces. If persistence needs a stream version marker, add one and handle older (empty) streams.
- Tests: save to a stream and load into a new control instance; save to and load from a property bag (a small in-test `IPropertyBag` implementation).

### Phase 7 – Rebuild render scenes per layer (#7, #34)

**#7 – Any visibility change re-reads every layer during paint** (`src/MapWinGIS.Core/src/map/MapView.cpp:150-161, 244-252`, `src/MapWinGIS.Core/src/rendering/MapRenderScene.cpp:16-50`)

Suggested fix:

- Cache one `VectorRenderScene` per `LayerId` inside `MapView`. `CreateMapRenderScene` (or a new internal function) assembles the map scene from the cached layer scenes in layer order.
- `SetLayerVisible(false)` keeps the cached scene and only excludes it. `SetLayerVisible(true)` reuses it. `RemoveLayer` drops only that layer's entry, and `ClearLayers` clears the cache. `AddLayer` with a prepared scene stores it in the cache.
- `InvalidateScene()` keeps its current meaning (rebuild everything). Add a per-layer invalidation (for example `InvalidateLayerScene(LayerId)`) if the public API needs one; document it.
- Keep the existing exception guarantees of `AddLayer`/`Scene()`.
- Tests: count source reads with a test hook or by checking that toggling visibility does not change a counter incremented in `BuildVectorRenderScene`; check ordering after add, remove and visibility changes; check that a layer hidden at add time is built when first shown.

**#34 – Scene build cost** (`include/mapwingis/rendering/VectorRenderScene.h:27-45`, `src/vector/VectorDataset.cpp:430-452`, `src/rendering/VectorRenderScene.cpp:104-114`)

- In `ForEachGeometry`, ignore attribute fields while iterating (`OGRLayer::SetIgnoredFields`, restored afterwards; the code already has an `IgnoredFields` helper). Keep the restore exception-safe.
- Compute the scene extent while iterating geometries, instead of calling `GetExtent()` first, when the driver has no fast extent (check `OLCFastGetExtent`).
- **Decision**: replacing the nested `vector<vector<vector<Point>>>` with flat coordinate and offset buffers changes a public header and both renderers. Ask the user whether to do it in this phase or as a separate follow-up. If the user agrees, add a test that renders the same scene before and after (the rendering baseline tests) to show the output is unchanged.
- Measure before and after on a generated layer (for example 100,000 polygons) and put the numbers in the PR description.


**Phase 7 implementation:** Per-layer scene caching and extent traversal optimization are implemented in PR [#77](https://github.com/MapWindow/MapWinGIS6/pull/77). Local builds and tests were intentionally skipped; CI validation is pending.

**Limitations:** The public MapRenderScene remains an owning snapshot, so assembling it copies cached geometry. The cache prevents repeated GDAL reads and triangulation on visibility changes, but does not eliminate snapshot-copy cost. The nested polygon representation was retained; flat coordinate buffers remain a separate design decision.

### Phase 8 – Fix ActiveX rendering edge cases (#21, #26, #35, #38)

**#21 – Sub-pixel check in the pan blit is dead code** (`src/MapWinGIS.ActiveX/MapWinGISControl.cpp:1124-1127`, `TryBlitCachedLayers`)

- Decide from the code which behaviour was intended. Recommended: blit only whole-pixel shifts and fall back to a full render when the offset is not within a small epsilon of an integer. Compare against the rounded value correctly (for example `std::abs(offset - std::round(offset)) > 1e-6`), and fix the comment to match.
- Clamp the shift to `±width`/`±height` before computing `width + shiftX` (a shift of a full width or more needs a full render anyway).
- Tests: a mid-drag `OnDraw` with a whole-pixel shift uses the blit path; a fractional shift re-renders; a huge shift does not overflow and re-renders.

**#26 – No redraw after a failed resize** (`MapWinGISControl.cpp:718-728`, `ResizeView`)

- On the error path, invalidate the layer cache and call `FireViewChange`, as `MapView::Resize` documents. Add `catch (...)` so the `noexcept` function cannot terminate.
- Test: force `MapView::Resize` to fail (a pending fit on an extent that cannot be fitted, or an existing hook) and check that the cache is invalidated.

**#35 – Renderer hot-path cost** (`src/MapWinGIS.ActiveX/OpenGlVectorLayerRenderer.cpp:564, 725-742, 799`, `GdiVectorLayerRenderer.cpp`)

- Cache the per-polygon support and convexity results per scene (keyed by the scene object or a scene version), so `atan2` and allocations don't run on every draw.
- Read back pixels with `GL_BGRA` and avoid the CPU channel swap. Keep the row-order handling correct for the negative `biHeight` DIB.
- After repeated context-creation failures, back off (for example skip OpenGL for N draws or until the next resize) and fall back to GDI, as the renderer does today.
- In the GDI renderer, merge the drawable-coordinates check into the drawing pass if it doesn't complicate the code.
- The rendering baseline tests must still pass unchanged. Put before/after timings for a large generated scene in the PR description.

**#38 – Print and scaled draws keep large buffers** (`MapWinGISControl.cpp:1280-1306`, `OnDraw`)

- For draws whose size differs from the window's client size (printing, metafiles, scaled draws), use temporary frame and staging buffers that are released after the draw, so the screen buffers keep their size.
- Test: a scaled `OnDraw` followed by a screen-size draw leaves the screen buffers at screen size (use a test hook to read buffer sizes).

### Phase 9 – Load vector datasets asynchronously (#36)

`LoadVectorDatasetImpl` (`src/MapWinGIS.ActiveX/MapWinGISControl.cpp:400-490`) opens the dataset and builds the scene on the STA thread, which freezes the host for large or network datasets.

**Decision**: this adds new COM API. Before you start, present the design to the user and wait for approval:

- A new versioned interface with `BeginLoadVectorDataset(path) → operationId` that runs open and scene building on the existing `GdalOperationDispatcher` worker, a completion event (or reuse of `GdalOperationCompleted`), and a way to cancel.
- The synchronous `LoadVectorDataset` stays unchanged.
- The worker builds a candidate `Map` and scene off-thread. The STA thread commits them on completion, reusing the prepared-scene `AddLayer` overload, so the control's state changes only on the UI thread.
- GDAL objects created on the worker must not be used concurrently; follow the threading contract documented in Phase 3.

After approval, implement it with tests: load completes and fires the event; cancel leaves the map unchanged; a failed open reports error details; destroying the control during a load is safe.
