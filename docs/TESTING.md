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
control, resizes the host, samples rendered polygon and background pixels,
recreates the control, reloads the layer, and writes individual outcomes to
`bin\Debug\x64\ActiveXTestHost-results.json`. A watchdog enforces a 30-second
default timeout; `-TimeoutSeconds` changes it from 5 to 300 seconds. These
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
host also compiles for Win32/x86; matching native x86 hosting remains for
Phase 7.

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
self-intersecting rings are rejected by OpenGL. The OpenGL drawing test skips
only when the backend reports that no suitable target is available (`S_FALSE`);
drawing, readback, or presentation failures fail it. The GDI fallback test
does not require OpenGL. The Debug x64 build completed without compiler
warnings, and the native suite ran 142 tests: 140 passed and 2 were skipped
(the opt-in network test and registered-host check). The
registration-free C# host activated the control, loaded the test layer, and
resized it, but its screen-pixel checks could not be verified in this
noninteractive session because the host window was absent from the captured
desktop. Rerun that check from an interactive Windows desktop.

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
