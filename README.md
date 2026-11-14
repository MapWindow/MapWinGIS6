# MapWinGIS6

MapWinGIS6 is the next major version of [MapWinGIS](https://github.com/MapWindow/MapWinGIS),
the open-source mapping engine behind MapWindow.

MapWinGIS 5 is one large MFC/ATL project that builds `MapWinGIS.ocx`. Version 6
splits that into two layers:

- **`MapWinGIS.Core.dll`** — the mapping engine as a normal C++ DLL, with no
  COM, ATL or MFC. It can be used and tested on its own.
- **`MapWinGIS.ocx`** — a thin ActiveX/COM wrapper over the core, exposing
  selected legacy API compatibility where it fits the v6 design.

MapWinGIS 5.x remains available as a behavioral reference. MapWinGIS6 prioritizes
a clean, maintainable modern architecture; backward compatibility is pursued
selectively and must not prevent structural improvements.

## Status

Early. The core currently includes GDAL setup and OGR-backed vector dataset
and layer access, including feature and field reading and updates. Available
formats depend on the GDAL drivers enabled in the build; tests cover shapefiles
and GeoPackage. The ActiveX layer supports COM activation, vector loading,
polygon rendering, and asynchronous generic GDAL algorithms with progress and
cancellation events.

The core also supports a shared map viewport with pan, center zoom, anchored
zoom, and fitting to the visible map or a named layer. These navigation calls
do not depend on a window. `IMapWinGISControl4` exposes this programmatic
navigation and stable layer IDs through ActiveX. The control also supports
left-button drag panning and cursor-anchored mouse-wheel zoom. Resizing keeps
the current map center and resolution.

Vector loads add layers rather than replace them. Call `ClearLayers` to
release the loaded datasets before switching or refreshing them.

## Build and test

### Prerequisites

- Windows
- Visual Studio 2026 (18.10 or later) with the **Desktop development with C++** workload, MSVC v145, and Windows SDK 10.0.28000.0
- The **C++ ATL for latest v145 build tools** component for each architecture to build (x64 and/or x86)
- CMake 3.28 or later; use Visual Studio 2026's bundled CMake for the `Visual Studio 18 2026` generator
- Ninja for the CLion and command-line presets
- .NET 10 SDK and PowerShell 7 (`pwsh`) to build and run the C# ActiveX test host
- Git
- About 15 GB of free disk space for dependencies and build outputs

### First build with CMake

From a Visual Studio 2026 Developer PowerShell in the repository root,
bootstrap vcpkg and build and test the x64 Debug preset. The Ninja generator
uses the compiler and Windows SDK environment from the Developer shell:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

The first configure installs GDAL and its dependencies through vcpkg and can
take a while; later builds reuse the local binary cache. CMake outputs go under
`out\b`, separate from the legacy MSBuild output.

The `.slnx` and `.vcxproj` projects remain available during the build-system
transition. To use the legacy MSBuild build, bootstrap vcpkg and run:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
```

Visual Studio 2026 can open the repository folder as a CMake project. Select
the `vs2026-x64` configure preset and the matching `vs2026-x64-debug` build
preset. See [docs/BUILDING.md](docs/BUILDING.md) for Visual Studio, CLion,
command-line, and legacy MSBuild setup.

### Run the C++ tests

CTest runs the CMake-built test cases:

```powershell
ctest --preset x64-debug
```

CTest discovers both native suites when ActiveX is enabled. Core tests remain
available when configuring with `-DMWG_BUILD_ACTIVEX=OFF`.

For the legacy MSBuild build, run its test executables from the repository root:

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe
bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe
```

Visual Studio's CMake project exposes discovered tests in Test Explorer. The
legacy `.slnx` tests also remain available through Test Explorer. For a legacy
Release build, run
`bin\Release\x64\MapWinGIS.Core.Tests.exe` and
`bin\Release\x64\MapWinGIS.ActiveX.Tests.exe`.

For additional configurations, dependency features, and troubleshooting, see
[docs/BUILDING.md](docs/BUILDING.md).

The CMake build can also include the native `translate` sample and targets for
building and running the C# ActiveX test host. Configure with
`-DMWG_BUILD_SAMPLES=ON` to enable them:

```powershell
# Configure with samples and build or launch the C# test host
cmake --preset x64-debug -DMWG_BUILD_SAMPLES=ON
cmake --build --preset x64-debug --target run_activex_host
```

See the CMake section in the build guide for target names and additional commands.

CI Release artifacts use the `full-drivers` feature. Their tests require
HDF5, NetCDF, KML, SpatiaLite, and PostgreSQL support.
Pull requests build and test Debug and full-driver Release for x64 using both
CMake and MSBuild, and also run CMake Win32 Debug. Pushes to `main` include the
full Win32 Debug and Release matrix. Each CI build has a 180-minute timeout.

After vector geometry edits, call `VectorDataset::Flush()` before closing to
persist exact Shapefile and GeoPackage bounds. The flush batches stored-extent
scans across multiple edits.

## Layout

| Path | Contents |
|---|---|
| `src/MapWinGIS.Core/` | The core engine. Public headers in `include/mapwingis/` |
| `tests/MapWinGIS.Core.Tests/` | Core C++ unit tests (GoogleTest) |
| `tests/MapWinGIS.ActiveX.Tests/` | ActiveX and COM-hosting C++ unit tests (GoogleTest) |
| `samples/Cpp/` | Buildable native C++ examples |
| `samples/CSharp/ActiveXTestHost/` | WinForms host for manual and automated ActiveX testing |
| `build/` | Shared MSBuild props, vcpkg triplets and scripts |
| `docs/` | Build, architecture, coding, GDAL integration, porting and testing documentation |

See [GDAL integration](docs/gdal-integration.md) for GDAL setup, API examples,
progress handling, migration notes, and limitations.

## License

Mozilla Public License 2.0. See [LICENSE](LICENSE).
