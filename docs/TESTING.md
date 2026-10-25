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

The .NET 10 WinForms host exercises the real registered ActiveX control through
`AxHost`. It has a visible manual mode and an `--automated` mode that uses the
same host, resize, repaint, and dispose paths. From an interactive Windows
desktop session, run:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

The command temporarily registers only the matching OCX for the current user,
refuses to replace existing registrations, and removes its registration after
the host exits. Automated mode creates a visible window and runs the UI message
loop, checks the `CoreInitialized` automation property, resizes the host,
samples the actual on-screen control background after repaint, recreates the
control, and writes individual outcomes to
`bin\Debug\x64\ActiveXTestHost-results.json`. A watchdog enforces a 30-second
default timeout; `-TimeoutSeconds` changes it from 5 to 300 seconds.
These integration checks require Windows and an interactive desktop session;
they are not currently part of CI. The control paints only its background, so
this host does not yet verify map rendering.

The host executable accepts `--automated`, `--results <path>`, and
`--timeout-seconds <5-300>`. The PowerShell launcher supplies these options
while it manages temporary COM registration.

### Phase 3a validation

On the Debug x64 build, the native test executable ran 135 tests: 133 passed
and 2 were skipped (the opt-in network test and the separately exercised
native hosting check). `Verify-ActiveXActivation.ps1` passed its registered
COM hosting test. The C# automated host passed all 10 checks, including
activation, automation access, resize, visible repaint, disposal, recreation,
and a second paint. Manual mode opened visibly and closed through the normal
window-close path. The managed host also compiles for Win32/x86; matching
native x86 hosting remains for Phase 7.

## Conventions

- One test file per area under test (`VectorDatasetTests.cpp`,
  `VectorLayerTests.cpp`, `GdalEnvironmentTests.cpp`).
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
