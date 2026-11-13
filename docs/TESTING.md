# Testing

- Framework: GoogleTest from the vcpkg `tests` feature (enabled by default
  through the MSBuild `MwgVcpkgFeatures` setting and when `MWG_BUILD_TESTS` is
  on for CMake).
- Core test project: `tests/MapWinGIS.Core.Tests`, built with the solution or
  CMake.
- ActiveX test project: `tests/MapWinGIS.ActiveX.Tests`, built with the solution.
- CMake currently registers the Core suite, not the ActiveX-specific tests.
- ActiveX implementation units shared by the OCX and its tests are built as
  `MapWinGIS.ActiveX.Impl.lib`.
- Shared fixtures and helpers live in `TestData.h/.cpp` (`TempDirectory`,
  `MakeSampleShapefile`, `MakeProvincesDataset`, `MakeRectanglesDataset`, and
  layer constants). Extend these instead of writing per-file fixture code.
  `MakeRectanglesDataset` writes one rectangle per given extent, so map tests
  can place layers at chosen coordinates or create an empty layer.

## Running the legacy MSBuild tests

Run the MSBuild test executables from the repository root:

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe
bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe
bin\Release\x64\MapWinGIS.Core.Tests.exe --gtest_output=xml:testresults.xml   # CI
bin\Release\x64\MapWinGIS.ActiveX.Tests.exe --gtest_output=xml:activex-testresults.xml
```

Also runnable from Visual Studio Test Explorer (GoogleTest adapter picks it
up automatically).

### CMake and CTest

The CMake target registers the Core GoogleTest sources and fixtures; the
ActiveX-specific tests remain MSBuild-only:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

`gtest_discover_tests()` registers individual cases for CTest. Each test runs
from the build's runtime directory, which contains the OCX, dependent DLLs,
and GDAL/PROJ data. GoogleTest writes per-test XML files under `test-results`
in the build directory. The `x64-debug` test preset already shows output on
failure and excludes tests labelled `interactive`, which change per-user COM
registration or require a desktop. CTest can also write a single JUnit report:

```powershell
ctest --test-dir out\b\x64d --output-on-failure -LE interactive `
    --output-junit out\b\x64d\ctest.xml
```

To run one case from CTest, match its fully qualified GoogleTest name:

```powershell
ctest --test-dir out\b\x64d -R 'VectorDatasetTest.ReportsTheDriverAndPath' --output-on-failure
```

### CMake and CLion

Use the imported `CMakePresets.json` profile for the desired
architecture/configuration. CMake registers individual GoogleTest cases for
IDE discovery; CLion's GoogleTest integration can display them after reload.
Run or debug a test from the test tree, and use the GoogleTest filter field to
run a subset. The executable and its runtime dependencies are colocated, so
CLion can launch it directly without a CTest environment setting.

From the command line, CTest can run a GoogleTest filter by selecting matching
registered test names with `-R`. To pass a GoogleTest filter directly to the
test executable, run it from its CMake runtime directory:

```powershell
Push-Location out\b\x64d\bin
.\MapWinGIS.Core.Tests.exe '--gtest_filter=VectorDatasetTest.*'
Pop-Location
```

## ActiveX host integration checks

### Registered native control check

The native verification script registers the control for the current user,
creates it through its ProgID, checks the automation boundary, and runs the
registered-control hosting test. It checks both per-user registry views first
and refuses to run if the control's CLSID or either ProgID is already
registered. Run it from a PowerShell process that matches the target
architecture:

```powershell
pwsh -File tests\MapWinGIS.ActiveX.Tests\Verify-ActiveXActivation.ps1 -Configuration Debug -Platform x64
```

The native script requires `MapWinGIS.ActiveX.Tests.exe` and its
`ActiveXHosting.ResizesRepaintsAndRecreatesControl` test, which CMake does not
currently build. Use the MSBuild output for this verification until CMake
registers the ActiveX test suite. CMake's `Verify-ActiveXActivation` CTest
entry currently uses `MapWinGIS.Core.Tests.exe`, which does not contain that
hosting test and is not a substitute for this check.

## Build configuration tests

```powershell
pwsh -File build\scripts\Test-BuildConfiguration.ps1
```

These tests pass `tests%3Bfull-drivers` on the MSBuild command line in a path
with spaces and inspect the resulting dependency arguments.
They also make sure that only Core deploys GDAL and PROJ data to the shared
output folder. CI runs them before the solution build. Builds with
`full-drivers` also test the release driver set in `GdalEnvironment` tests.
Pull requests run both Debug and full-driver Release tests for x64.

The native `MapWinGIS.ActiveX.Tests` project covers COM activation, registered
control rendering through GDI and OpenGL, and GDAL progress/completion events.
Its registered-control host test also sends a wheel zoom and captured drag to
the hosted control and verifies their viewport effects. Run this opt-in test
with `tests\MapWinGIS.ActiveX.Tests\Verify-ActiveXActivation.ps1`.

### C# ActiveX test host

The .NET 10 WinForms host exercises the real ActiveX control through `AxHost`
using registration-free COM activation from its embedded application
manifest. It has a visible manual mode and an `--automated` mode that uses the
same host, resize, repaint, and dispose paths, with no COM registry changes.
The automated checks also run a GDAL raster conversion and check progress and
completion events, including operation IDs and progress arguments.

In manual mode, use `Open` or drag files onto the host to add vector
datasets. The layer list shows names, feature counts, stable IDs, and file
path tooltips. Each dataset contributes only its first layer.
The file formats available depend on the GDAL build.

Use `Clear` to remove all layers, `Zoom +` and `Zoom -` to change zoom,
`Map` to fit all layers, and `Layer` to fit the selected layer.
The arrow buttons pan by the `Pan units` value in dataset coordinates.
The status strip shows core initialization, layer and feature totals,
the rendering backend, and rendering status or operation errors.
`Recreate` restores the startup layer, and `Repaint` requests a redraw.

From an interactive Windows desktop session, run:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

The launcher makes no COM registration changes. Automated mode creates a
visible window and runs the UI message loop, checks the `CoreInitialized`
automation property, loads a generated two-polygon GeoJSON dataset through the
control, resizes the host, and samples rendered polygon and background pixels
for GDI and a requested OpenGL path. The OpenGL request can use GDI when
OpenGL is unavailable or the scene is unsupported. The host recreates the
control and reloads the layer. It calls the original `LoadVectorDataset`
method and reads `LayerLoaded` and the absolute `FeatureCount` on each fresh
control. It also checks failed dataset loads, loads two layers through
`LoadVectorDatasetEx`, and checks their IDs, counts, and feature-count deltas.
It exercises map fitting, per-layer fitting, zoom in and out, pan, cursor
anchored zoom, navigation after resizing, invalid layer IDs, and navigation
with no layers. It clears and reloads layers and makes sure that failed loads
and navigation do not change layer or feature counts. The host writes
individual outcomes to
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
For CMake output, the C# launcher accepts `-BinDirectory out\b\x64d\bin`;
this override applies to the C# host and does not change the native
verification script's test executable.
For build and manual-use instructions, see the
[ActiveX Test Host README](../samples/CSharp/ActiveXTestHost/README.md).

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
bin\Release\x64\MapWinGIS.ActiveX.Tests.exe --gtest_filter=RenderingBaseline.*
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
- `ViewTransform` tests check that the renderers' transform gives the same
  screen positions as `Viewport::MapToScreen` and requires a valid viewport.
- Multi-layer rendering is tested in three places, none of which needs a
  window or ActiveX: `MapRenderSceneTests.cpp` covers scene order, hidden and
  empty layers, and per-layer failures; `MapViewTests.cpp` covers fitting,
  resizing, and scene invalidation; and `VectorLayerRendererTests.cpp` draws
  layers with different extents into memory bitmaps and samples the pixels at
  `Viewport::MapToScreen` positions, for GDI and, when available, OpenGL.
- Native `ActiveXMapTest` cases activate the real OCX through its class factory
  without registration. They cover stacked loads, rejected loads, cached
  feature totals, dataset release through `ClearLayers`, `WM_SIZE`, and draws
  into alternate-sized targets that must not change the live fitting size.
- `VectorReadTestDriver` simulates unknown feature counts and read failures.
  Its features include polygon, null, and empty geometries. Tests cover retrying
  failed ID scans, exact counts, and scene reuse through Core and ActiveX.
  Read counters detect repeated traversal during loading and the first draw.
- Renderer regressions compare edge-adjacent pixels at quarter- and
  three-quarter-pixel rectangle coordinates for GDI and OpenGL. Overlapping
  layers make drawing order visible: the top fill covers the bottom outline.
  Map view tests fit point and zero-span extents, and tiny targets with default
  padding. Map scene tests isolate standard geometry-read exceptions, reuse
  the resulting scene, and let allocation failures propagate.
- `ViewportTests.cpp` covers pan, center-zoom, and anchored-zoom calculations
  and their limits, and
  `MapViewTests.cpp` covers zoom to map and layer, zoom in and out, empty and
  missing-layer cases, and pending-fit handling on `MapView`. Pending-fit
  tests cover replacement in both directions between map and layer fits,
  cancellation by pan and center zoom, and cancellation by missing or empty
  layers. They also cover unchanged navigation at a resolution limit and
  `ZoomAt` on a zero-sized viewport, which keep the pending fit.
  Viewport tests cover anchor movement when the new center raises the
  minimum resolution, and direct zoom-out multiplication without a reciprocal.
  The opt-in registered-control host test also verifies cursor-anchored wheel
  zoom and drag-to-pan pixel scaling and direction.
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
