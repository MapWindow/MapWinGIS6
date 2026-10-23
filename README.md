# MapWinGIS6

MapWinGIS6 is the next major version of [MapWinGIS](https://github.com/MapWindow/MapWinGIS),
the open-source mapping engine behind MapWindow.

MapWinGIS 5 is one large MFC/ATL project that builds `MapWinGIS.ocx`. Version 6
splits that into two layers:

- **`MapWinGIS.Core.dll`** — the mapping engine as a normal C++ DLL, with no
  COM, ATL or MFC. It can be used and tested on its own.
- **`MapWinGIS.ocx`** — a thin ActiveX/COM wrapper over the core, keeping the
  existing API so that current C#, VB.NET, WPF, Delphi and VB6 applications
  keep working.

MapWinGIS 5.x stays available and remains the reference implementation during
the migration.

## Status

Early. The core currently includes GDAL setup and OGR-backed vector dataset
and layer access, including feature and field reading and updates. Available
formats depend on the GDAL drivers enabled in the build; tests cover shapefiles
and GeoPackage. The ActiveX layer has not been added yet.

## Build and test

### Prerequisites

- Windows
- Visual Studio 2026 (18.10 or later) with the **Desktop development with C++** workload, including MSVC v145 and Windows SDK 10.0.28000.0
- Git and PowerShell 7 (`pwsh`)
- About 15 GB of free disk space for dependencies and build outputs

### First build

From the repository root, bootstrap vcpkg and build the solution:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
msbuild MapWinGIS6.slnx -p:Configuration=Debug -p:Platform=x64 -m
```

The bootstrap script clones and initializes vcpkg in a sibling `vcpkg` folder
unless an existing vcpkg installation is configured. The first build uses
vcpkg to compile GDAL and its dependencies, so it can take a while; subsequent
builds use the local binary cache. No dependency downloads or environment
configuration are needed by hand.

Alternatively, after bootstrapping vcpkg, open `MapWinGIS6.slnx` in Visual
Studio and build the `Debug | x64` configuration.

### Run the C++ tests

The solution build includes the GoogleTest project by default. After building,
run the test executable from the repository root:

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe
```

You can also run the tests through Visual Studio's Test Explorer. For the
Release configuration, build with `-p:Configuration=Release` and run
`bin\Release\x64\MapWinGIS.Core.Tests.exe`.

For additional configurations, dependency features, and troubleshooting, see
[docs/BUILDING.md](docs/BUILDING.md).

## Layout

| Path | Contents |
|---|---|
| `src/MapWinGIS.Core/` | The core engine. Public headers in `include/mapwingis/` |
| `tests/MapWinGIS.Core.Tests/` | C++ unit tests (GoogleTest) |
| `build/` | Shared MSBuild props, vcpkg triplets and scripts |
| `docs/` | Build, architecture, coding, porting and testing documentation |

## License

Mozilla Public License 2.0. See [LICENSE](LICENSE).
