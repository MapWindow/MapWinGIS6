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

## Conventions

- One test file per area under test (`VectorDatasetTests.cpp`,
  `VectorLayerTests.cpp`, `GdalEnvironmentTests.cpp`).
- Fixture classes go in an anonymous namespace, derive from `testing::Test`
  and use `protected:` `SetUp()`; member naming follows production rules
  (`_camelCase`).
- Test names are PascalCase behavior statements:
  `ReportsTheDriverAndPath`, `RejectsOutOfRangeFieldIndexes`. Use `TEST_F`
  with fixtures; plain `TEST` for one-off cases.
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
