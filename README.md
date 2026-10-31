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

Early. The core includes GDAL setup, OGR-backed vector dataset and layer
access, viewport-windowed raster display, XYZ tile layers, coordinate
reprojection, and map navigation.
Available formats depend on the GDAL drivers enabled in the build; tests cover
shapefiles and GeoPackage. The ActiveX layer supports COM activation, vector
and raster loading, polygon/line/point rendering, keyboard, touch and mouse
navigation, navigation history, animated fits, and asynchronous generic GDAL
algorithms with progress and cancellation events.

The map uses the first layer's declared coordinate reference system (CRS).
Layers in other declared CRSs are reprojected into it; layers with and without
a CRS cannot be mixed. Raster layers are read for the visible map window at
viewport resolution. XYZ URL templates are supported as Web Mercator tile
layers, with GDAL-managed in-memory tile caching. OpenGL renders supported
simple polygons; GDI clips and draws polygons, lines, points, and rasters.

The core supports pan, center and cursor-anchored zoom, map/layer fitting,
back/forward navigation history, and timed animated fits. ActiveX exposes
these operations, configurable zoom factors, mouse-wheel zoom, drag-to-pan,
arrow-key navigation, and touch pan/pinch gestures. ActiveX animation runs on
the control's owner-thread timer.

Vector loads add layers rather than replace them. The first layer fits the
initial view; later loads preserve it. Call `ZoomToMap` to include new layers
in the visible extent, `MoveLayer` to change drawing order, or `ClearLayers`
to release loaded datasets before switching or refreshing them.

Dropped vector and raster files append as layers and the control fits the
combined visible extent after processing the batch. Unsupported or invalid
files are skipped; valid files in the same drop are still loaded.

## Build and test

### Prerequisites

- Windows
- Visual Studio 2026 (18.10 or later) with the **Desktop development with C++** workload, including MSVC v145 and Windows SDK 10.0.28000.0
- .NET 10 SDK to build the C# ActiveX test host
- Git and PowerShell 7 (`pwsh`)
- About 15 GB of free disk space for dependencies and build outputs

### First build

From the repository root, bootstrap vcpkg and build the solution:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
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
| `samples/Cpp/` | Buildable native C++ examples |
| `build/` | Shared MSBuild props, vcpkg triplets and scripts |
| `docs/` | Build, architecture, coding, GDAL integration, porting and testing documentation |

See [GDAL integration](docs/gdal-integration.md) for GDAL setup, API examples,
progress handling, migration notes, and limitations.

## License

Mozilla Public License 2.0. See [LICENSE](LICENSE).
