# Testing

- Framework: GoogleTest, provided by the vcpkg `tests` feature (enabled by
  default through `MwgVcpkgFeatures`).
- Test project: `tests/MapWinGIS.Core.Tests`, built with the solution.
- Shared fixtures and helpers live in `TestData.h/.cpp` (`TempDirectory`,
  `MakeSampleShapefile`, `MakeProvincesDataset`, and layer constants). Extend
  these instead of writing per-file fixture code.

## Running

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe
bin\Release\x64\MapWinGIS.Core.Tests.exe --gtest_output=xml:testresults.xml   # CI
```

Also runnable from Visual Studio Test Explorer (GoogleTest adapter picks it
up automatically).

## ActiveX host integration checks

The .NET 10 WinForms host exercises the real ActiveX control through `AxHost`
using registration-free COM activation from its embedded application
manifest. It has a visible manual mode and an `--automated` mode that uses the
same host, resize, repaint, and dispose paths, with no COM registry changes.
From an interactive Windows desktop session, run:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

The launcher makes no COM registration changes. Automated mode creates a
visible window and runs the UI message loop, checks the `CoreInitialized`
automation property, loads a generated two-polygon GeoJSON dataset through the
control, resizes the host, samples rendered polygon and background pixels for
both the GDI preference and the default (OpenGL-first) path, recreates the
control, reloads the layer, and writes individual outcomes to
`bin\Debug\x64\ActiveXTestHost-results.json`. Pixels are sampled from the host
window's own composed content (`PrintWindow` with `PW_RENDERFULLCONTENT`), not
from the screen, so another window on top or a lost foreground race does not
affect the result. A watchdog enforces a 30-second
default timeout; `-TimeoutSeconds` changes it from 5 to 300 seconds. The first
run after a fresh build can be slow while the new native DLLs load (one Win32
run exceeded 30 seconds); rerun it before treating a timeout as a hang. These
integration checks require Windows and an interactive desktop session; they
are not currently part of CI.

The host executable accepts `--automated`, `--results <path>`, and
`--timeout-seconds <5-300>`. The PowerShell launcher supplies these options
and starts the host without changing COM registration.

### Phase 3a validation

On the Debug x64 build, the native test executable ran 135 tests: 133 passed
and 2 were skipped (the opt-in network test and the separately exercised
native hosting check). `Verify-ActiveXActivation.ps1` passed its independent,
registration-based native COM hosting test. The C# automated host passed all
10 checks without COM registration, including activation, automation access,
resize, visible repaint, disposal, recreation, and a second paint. Manual mode
opened visibly and closed through the normal window-close path. The managed
host also compiles for Win32/x86; matching native x86 hosting was verified
during Phase 7.

### Phase 4 validation

The Debug x64 native suite ran 137 tests: 135 passed and 2 were skipped. Both
polygon scene tests passed for Shapefile and GeoPackage fixtures. The
registration-free C# host passed all 12 checks, including loading a generated
GeoJSON layer, sampling both polygon fills and the gap between them, and
reloading and repainting after control recreation. These integration checks
require an interactive Windows desktop and are not currently part of CI.

### Phase 5 validation

The native renderer tests exercise the OpenGL backend directly when a WGL
context with framebuffer objects is available, then verify that the
coordinator selects it. They also verify that polygons with interior rings use
the GDI fallback and that concave rings hidden by duplicate vertices and
self-intersecting rings are rejected by OpenGL. Scenes with non-finite or
out-of-range device coordinates fail with `E_INVALIDARG` before GDI draws
anything. The OpenGL drawing test skips
only when the backend reports that no suitable target is available (`S_FALSE`);
drawing, readback, or presentation failures fail it. The GDI fallback test
does not require OpenGL. The Debug x64 build completed without compiler
warnings, and the native suite ran 143 tests: 141 passed and 2 were skipped
(the opt-in network test and registered-host check). The
registration-free C# host activated the control, loaded the test layer, and
resized it, but its screen-pixel checks could not be verified in this
noninteractive session because the host window was absent from the captured
desktop. Rerun that check from an interactive Windows desktop.

### Phase 6 and 7 validation

The renderer coordinator tests cover runtime backend preferences, hardware
capability rejection, and GDI fallback. At Phase 7, recreation of a failed
OpenGL context on later draws was not covered by an automated test (the
ActiveX hosting test recreates the entire control, not a failed WGL context);
the follow-up validation below adds that test. Phase 7 passed Debug and Release solution builds and native test runs
on Win32 and x64, with no Level 4 compiler warnings. Each test run reported
146 tests: 144 passed and 2 were skipped (the opt-in network test and the
separately exercised registered-host check). The native registration and
host-lifecycle check passed for both architectures in Debug.

The managed host builds for both architectures. At Phase 7 it had not been
run against both native controls, and the screen-sampled pixel assertions were
unverified; the follow-up validation below resolves both.

### Follow-up validation of open Phase 5–8 findings

The earlier pixel failures were caused by the check, not the control: it read
the screen DC, which returns whatever window is on top. A capture of the host
window showed both polygons and the gap rendered correctly by OpenGL while the
screen showed another application at that position. The host now samples its
own window content (see above) and also checks pixels under the GDI preference.

On an interactive console session, the registration-free host passed all 13
checks for Debug and Release on both Win32 and x64, and passed ten consecutive
Debug runs (five per architecture). `Verify-ActiveXActivation.ps1` passed for
Debug and Release on both architectures (32-bit Windows PowerShell for Win32).
The native suite ran 147 tests in each Release build: 144 passed and 3 were
skipped (the opt-in network, registered-host, and rendering-baseline tests).

A Win32 Release build with `MwgVcpkgFeatures=tests;full-drivers` installed the
extra GDAL dependencies and deployed the HDF5, NetCDF, PostgreSQL, and
SpatiaLite DLLs (libkml links statically). Its native suite, which by then
included the context-loss test below, ran 148 tests: 145 passed with the same
three opt-in skips. The host passed all 13 checks.

`VectorLayerRenderer.OpenGlRecreatesLostContextOnLaterDraw` destroys the
OpenGL backend's hidden render target after a successful draw. It verifies
that the next draw fails with `E_FAIL` (the coordinator falls back to GDI for
that frame) and that the following draw recreates the context and renders
with OpenGL again.

## Rendering performance baseline

`RenderingBaseline.MeasuresCoreSceneAndBackendCosts` is opt-in. It generates a
GeoPackage grid of convex 32-vertex polygons and separately reports Core
dataset open, render-scene preparation, process private-memory growth, the
median GDI frame, the first OpenGL frame (context creation and drawing), and
the median steady OpenGL frame, for a 1280x720 off-screen target. It records
the values as GoogleTest properties and fails only when a stage fails. Use a
Release build for meaningful numbers:

```powershell
$env:MWG_RUN_RENDERING_BASELINE = '1'
$env:MWG_RENDERING_BASELINE_FEATURES = '100000'   # optional; default 50000
bin\Release\x64\MapWinGIS.Core.Tests.exe --gtest_filter=RenderingBaseline.*
```

Measurements on one developer workstation (Release, hardware OpenGL); the
previous implementation measurements are retained for comparison:

| Measurement | x64, 10k (previous) | x64, 100k (previous) | x64, 100k (sequential) |
|---|---:|---:|---:|
| Core open dataset | 1.5 ms | 1.5 ms | 1.5 ms |
| Core prepare render scene | 717 ms | 15,225 ms | 91.5 ms |
| Private memory for the scene | 8.5 MiB | 62.8 MiB | 61.5 MiB |
| GDI frame (median) | 17.5 ms | 308 ms | 171.4 ms |
| OpenGL first frame | 157 ms | 350 ms | 233.7 ms |
| OpenGL frame (median) | 10.0 ms | 82.3 ms | 80.5 ms |
| Private memory, total | 91.7 MiB | 167.4 MiB | 167.2 MiB |

Scene preparation now reads geometries sequentially through OGR and copies
their coordinates directly, avoiding indexed lookups, WKT serialization, and
reparsing. The 100k-feature measurement dropped from 15,225 ms to 91.5 ms on
the recorded x64 run. OpenGL frames re-submit all geometry because there is no
unchanged-geometry cache. Total memory includes the OpenGL driver and context.

## Conventions

- One test file per area under test (`VectorDatasetTests.cpp`,
  `VectorLayerTests.cpp`, `VectorRenderSceneTests.cpp`,
  `GdalEnvironmentTests.cpp`).
- Fixture classes go in an anonymous namespace and derive from `testing::Test`
  or `testing::TestWithParam<T>` for parameterized cases. Use `protected:`
  `SetUp()` and production member naming (`_camelCase`).
- Test names are PascalCase behavior statements:
  `ReportsTheDriverAndPath`, `RejectsOutOfRangeFieldIndexes`. Use `TEST_F`
  with fixtures, `TEST_P` with parameterized fixtures, and plain `TEST` for
  one-off cases. Register parameter values with `INSTANTIATE_TEST_SUITE_P`.
- Prefer `EXPECT_*`; use `ASSERT_*` only when continuing would crash (e.g.
  before dereferencing).
- Cast `[[nodiscard]]` returns to `(void)` inside `EXPECT_THROW`.
- Append `<< value` to expectations on strings when the failure output
  matters.
- Test files carry the same SPDX and Doxygen header as production files.
- Cover for each feature: happy path, empty/missing input, out-of-range
  indexes, null values, non-ASCII paths, write-then-reopen round-trips, and
  error cases.
- Test data is generated at runtime inside a `TempDirectory`; do not commit
  shapefiles or databases to the repository.
- `MakeProvincesDataset` creates Shapefile and GeoPackage fixtures for layer
  tests. Each has four polygons in EPSG:28992, all five field types, null
  values, and UTF-8 names. Tests edit their own generated datasets.
- Wrap expected GDAL noise in `GdalEnvironment::QuietScope`.
- Tests use local generated data and do not require network access by default.
  Set `MWG_RUN_NETWORK_GDAL_TESTS=1` to enable the public `/vsicurl/` COG test.
