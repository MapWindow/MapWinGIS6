# Building MapWinGIS6

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| Visual Studio 2026 | 18.10 or later | Workload **Desktop development with C++** |
| MSVC toolset | v145 (14.51 or later) | Installed with the workload |
| Windows SDK | 10.0.28000.0 | Override with `/p:MwgWindowsSdk=10.0` to use the newest installed instead |
| Git | any recent version | Needed to clone vcpkg |
| Disk space | about 15 GB | vcpkg sources, build trees and binaries |

The ActiveX project uses ATL and statically links it. MFC is not required.
The C# test host requires the .NET 10 SDK and the Windows Forms desktop
runtime.

## First build

```powershell
# 1. Get vcpkg. Clones into ..\vcpkg next to this repository by default.
pwsh -File build\scripts\bootstrap-vcpkg.ps1

# 2. Build. The first build compiles GDAL and its dependencies from source and
#    takes a while; later builds reuse the vcpkg binary cache.
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
```

Or just open `MapWinGIS6.slnx` in Visual Studio after step 1 and build.

### ActiveX activation

The solution builds `MapWinGIS.ocx` beside `MapWinGIS.Core.dll`. To verify
COM activation without machine-wide registration, run the activation check
from a PowerShell process that matches the target architecture:

```powershell
pwsh -File tests\MapWinGIS.ActiveX.Tests\Verify-ActiveXActivation.ps1 -Configuration Debug -Platform x64
```

The script registers the control for the current user, creates it through its
ProgID, checks the automation boundary, and removes the per-user registration.
It checks both per-user registry views first and refuses to run if the control's
CLSID or either ProgID is already registered, so it cannot overwrite another
build's registration. It also embeds the control in an ATL OLE host, resizes
the host, repaints, and recreates the control. Use 32-bit PowerShell with
`-Platform Win32`.

### C# ActiveX test host

Build the solution for the same architecture as the native control. The host
targets x64 explicitly by default and also defines a Win32/x86 configuration
for use when the native Win32 build is available. Its executable is placed
beside the OCX, core DLL, native dependencies, and GDAL/PROJ runtime data.
The host uses `AxHost` directly with the control's CLSID and an embedded
application manifest that declares the OCX class and type library. This
registration-free COM activation needs no generated interop assembly,
wrapper-generation tool, `regsvr32`, or registry changes.

Run the launcher from an interactive Windows desktop session. It starts the
host beside the native files; the manifest activates that OCX for the host
process only. The launcher does not read, add, overwrite, or remove COM
registration.

Manual mode starts with a generated two-polygon layer. Use `Open` to select
vector datasets, or drag files onto the host. Each load adds an entry to the
layer list with its name, feature count, stable ID, and file path tooltip.
Only the first layer of each dataset is loaded. Available formats depend on
the GDAL drivers in the build.

Use `Clear` to remove all layers. `Zoom +` and `Zoom -` change the zoom,
`Map` fits all layers, and `Layer` fits the selected layer. The arrow
buttons pan by the `Pan units` value in dataset coordinates.
The status strip shows core initialization, total layer and feature counts,
the rendering backend, and rendering status or operation errors.
`Recreate` restores the startup layer, and `Repaint` requests a redraw.

```powershell
# Visible manual host. Close the window to release the control.
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64

# Visible automated hosting checks; writes JSON and returns a nonzero code on failure.
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

For Win32, use `-Platform Win32` after building the matching native binaries.
Automated checks need Windows and an interactive desktop session. The host
creates a temporary two-polygon GeoJSON dataset, loads it through the control,
and verifies visible polygon fills and the background gap after repaint and
control recreation. It also loads two layers, checks their IDs and feature
counts, zooms to each layer, pans, and clears the layers.
It can activate the OCX without a registration. The
separate native activation script above retains its own temporary
registration-based test.

### Validating both native architectures

Build and run both native test executables for each platform. Use `-restore` when
alternating architectures so the matching vcpkg triplet and managed host
assets are restored:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug\x64\MapWinGIS.Core.Tests.exe
bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe

msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=Win32 -m
bin\Debug\Win32\MapWinGIS.Core.Tests.exe
bin\Debug\Win32\MapWinGIS.ActiveX.Tests.exe
```

The native activation check uses a PowerShell process with the same bitness as
the target. The registration-free C# host also needs matching native binaries.
Its pixel assertions need an interactive desktop. Debug and Release builds,
registration-based activation, and the C# host checks have been verified on
both architectures.

Changing `MwgVcpkgFeatures` (for example adding `full-drivers`) reinstalls the
vcpkg packages on the next build. After each successful install, the build
records the installed features next to vcpkg's stamp file in the shared
`vcpkg_installed\<Platform>\` tree. When a later build of any configuration
requests different features, it deletes the stamp, so vcpkg reinstalls the
tree. For example, Release with `full-drivers`, then Debug without it, then
Release with it again reinstalls the tree each time.

### Where vcpkg is found

`Directory.Build.props` and `bootstrap-vcpkg.ps1` look in this order:

1. `/p:MwgVcpkgRoot=<path>` on the MSBuild command line
2. the `MWG_VCPKG_ROOT` environment variable
3. `..\vcpkg` next to the repository
4. the `VCPKG_ROOT` environment variable

The build quotes the overlay-triplet path when it invokes vcpkg, so checkout
paths can contain spaces.

## Configurations and platforms

| | |
|---|---|
| Configurations | `Debug` (`/MDd`, `_DEBUG`), `Release` (`/MD`, `/O2`) |
| Platforms | `x64`, `Win32` |
| Triplets | `x64-windows-v145`, `x86-windows-v145` (in `build/triplets`, pinned to v145) |
| Output | `bin\<Configuration>\<Platform>\` |
| Intermediates | `obj\<Project>\<Configuration>\<Platform>\` |

Nothing is written outside the repository and no environment variables are
changed. The build does not register anything with COM.

## Dependencies

All dependencies come from vcpkg; see `vcpkg.json`. There is no manual SDK
download and no `support\` folder.

Manifest features:

| Feature | Contents | When |
|---|---|---|
| (default) | GDAL with the recommended drivers plus the small permissive codecs | Everyday development |
| `tests` | GoogleTest | Enabled by default through `MwgVcpkgFeatures` |
| `full-drivers` | Adds HDF5, NetCDF, KML, SpatiaLite and PostgreSQL to GDAL | Release packages. Much slower to build |

```powershell
msbuild MapWinGIS6.slnx -restore -p:Platform=x64 -p:Configuration=Release "-p:MwgVcpkgFeatures=tests%3Bfull-drivers"
```

`%3B` passes a literal semicolon through MSBuild's property parser.
CI uses this feature set for Release artifacts. Core tests built with
`full-drivers` require HDF5, NetCDF, LIBKML, PostgreSQL, and SpatiaLite support.
The SpatiaLite test creates a spatial database without an external server.
Pull requests and pushes to `development` build and test Debug and full-driver
Release for x64. Pushes to `main` also cover Win32. Each job has a
180-minute timeout.

### Runtime data

vcpkg installs `gdal-data` and `proj.db` under
`vcpkg_installed\<Platform>\<triplet>\share`. Each platform has its own install
tree because vcpkg removes other triplets from a shared tree, which would break
alternating Win32 and x64 builds.
The Core project's target in `Directory.Build.targets` copies them into the
output folder as `gdal-data\` and `proj\`. Other projects reference Core and
do not copy the shared data. This prevents competing writes during parallel
solution builds. `GdalEnvironment` points GDAL and PROJ at these folders.
No `GDAL_DATA` or `PROJ_LIB` environment variable is needed or set.

### Binary cache

vcpkg caches built packages in `%LOCALAPPDATA%\vcpkg\archives` by default, so a
rebuild or a second platform is fast. To share a cache across machines, set
`VCPKG_BINARY_SOURCES`, for example:

```powershell
$env:VCPKG_BINARY_SOURCES = 'clear;files,\\server\share\vcpkg-cache,readwrite'
```

## Running the tests

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe
bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe
```

Or use Test Explorer in Visual Studio, which picks up the GoogleTest adapter
automatically.

For CI, write a JUnit-style report:

```powershell
bin\Release\x64\MapWinGIS.Core.Tests.exe --gtest_output=xml:core-testresults.xml
bin\Release\x64\MapWinGIS.ActiveX.Tests.exe --gtest_output=xml:activex-testresults.xml
```

## Extra build switches

| Property | Default | Effect |
|---|---|---|
| `MwgWindowsSdk` | `10.0.28000.0` | Windows SDK version. `10.0` means "newest installed" |
| `MwgVcpkgRoot` | resolved (see above) | vcpkg location |
| `MwgVcpkgFeatures` | `tests` | Semicolon-separated manifest features |
| `MwgEnableAsan` | `false` | Adds `/fsanitize=address` |
| `MwgDeployRuntimeData` | `true` for Core, `false` otherwise | Copies `gdal-data` and `proj` to the output folder |

## CMake migration

CMake is being introduced as the primary native build. Until it is complete,
the MSBuild solution above stays the supported build. The audit of the current
MSBuild settings and the decisions the CMake build follows are in
[CMAKE-MIGRATION.md](CMAKE-MIGRATION.md).

To measure flag and binary parity, `build\scripts\Compare-CompileCommands.ps1`
records a detailed MSBuild log and writes a normalised report to `out\parity\`.
It finds MSBuild through `MWG_MSBUILD`, `PATH` or `vswhere`, so it also works
when Visual Studio is not on `PATH` or not installed on `C:`.

### Building with CMake

The CMake build covers `MapWinGIS.Core.dll`, `MapWinGIS.ocx`, the native
GoogleTest suite, and the `translate` sample. When `dotnet` and PowerShell 7
are installed, it also provides targets to build and launch the C# ActiveX
test host. The OCX needs the **C++ ATL for latest v145 build tools** component
for each architecture you build. For Ninja, `midl.exe` needs `cl.exe` on
`PATH`, so use a Visual Studio toolchain (CLion) or a Developer PowerShell.
The Visual Studio generator uses the selected SDK's MIDL compiler and the
compiler environment supplied by MSBuild. CMake 3.28 or later is required;
use the CMake version bundled with Visual Studio 2026 for its Visual Studio 18
2026 generator. Both CMake and Ninja ship with Visual Studio 2026 and CLion.
Run the bootstrap script first. For Ninja presets, configure from a VS 2026
Developer PowerShell or any shell after `vcvarsall.bat x64`:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug --output-on-failure
```

Build the sample or C# host by target:

```powershell
cmake --build --preset x64-debug --target MapWinGIS.Samples.Translate
cmake --build --preset x64-debug --target build_activex_host
cmake --build --preset x64-debug --target run_activex_host
cmake --build --preset x64-debug --target run_activex_host_automated
```

The last two targets launch the host from an interactive Windows session; the
automated target runs the visible host checks. CMake directs the native sample,
host, OCX, Core DLL and deployed runtime dependencies into the selected build
tree's `bin` folder. This keeps them separate from the MSBuild output.

For a Core-only build that does not require MIDL or ATL, configure with
`-DMWG_BUILD_ACTIVEX=OFF`.

| Preset | Generator | Build directory |
|---|---|---|
| `x64-debug`, `x64-release` | Ninja, x64 | `out\b\x64d`, `out\b\x64r` |
| `x86-debug`, `x86-release` | Ninja, Win32 (vcvars `x86` or `amd64_x86`) | `out\b\x86d`, `out\b\x86r` |
| `vs2026-x64`, `vs2026-x86` | Visual Studio 18 2026, `-T v145` (build presets `vs2026-x64-debug`, ...) | `out\b\vs-x64`, `out\b\vs-x86` |

- Output goes to `<build directory>\bin` (`bin\<Config>` for the Visual Studio
  presets) and import libraries to `lib`, never to the MSBuild `bin\` folder, so
  both builds can be used side by side. MIDL output (`MapWinGIS.ActiveX_i.h`,
  `_i.c`, `.tlb`) goes to `<build directory>\src\MapWinGIS.ActiveX\midl`; the
  `.tlb` is also copied beside the OCX.
- To register the CMake-built control for the current user:
  `regsvr32 /n /i:user out\b\x64d\bin\MapWinGIS.ocx` (use
  `%WINDIR%\SysWOW64\regsvr32.exe` for Win32; add `/u` to unregister).
- The vcpkg root is found in the same order as above. CMake uses its own install
  tree, `vcpkg_installed\cmake-<Platform>`, shared by Debug and Release; the
  binary cache makes it cheap.
- Manifest features come from `VCPKG_MANIFEST_FEATURES` (presets: `tests`). A
  change takes effect on the next configure; in CLion use
  **Tools | CMake | Reset Cache and Reload Project** when a cached value is stale.
- Only `Debug` and `Release` are supported. Other `CMAKE_BUILD_TYPE` values,
  such as CLion's default `RelWithDebInfo`, are rejected.
- Ninja takes the toolset and Windows SDK from the vcvars environment and
  cannot pin them; configure warns when the SDK differs from `MWG_WINDOWS_SDK`.
  The Visual Studio presets pin both.
- The presets' `architecture` only informs Visual Studio. In CLion, give each
  preset profile a Visual Studio toolchain with the matching architecture
  (`amd64` for x64, `x86` or `amd64_x86` for x86); a mismatch stops configure
  with an explanation.

| CMake option | Default | MSBuild equivalent |
|---|---|---|
| `MWG_WINDOWS_SDK` | `10.0.28000.0` | `MwgWindowsSdk` (`10.0` = newest) |
| `MWG_VCPKG_ROOT` | resolved | `MwgVcpkgRoot` |
| `VCPKG_MANIFEST_FEATURES` | `tests` | `MwgVcpkgFeatures` |
| `MWG_ENABLE_ASAN` | `OFF` | `MwgEnableAsan` |
| `MWG_DEPLOY_RUNTIME_DATA` | `ON` | `MwgDeployRuntimeData` |
| `MWG_BUILD_TESTS` | `ON` | — (adds the `tests` feature) |
| `MWG_BUILD_ACTIVEX` | `ON` | — (set `OFF` for a Core-only build) |

Machine-specific settings belong in an uncommitted `CMakeUserPresets.json`
that inherits from the presets, for example:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "my-x64-debug", "inherits": "x64-debug", "cacheVariables": { "MWG_WINDOWS_SDK": "10.0" } }
  ]
}
```

### Using CMake in Visual Studio

The CMake presets support Visual Studio 2026's **Open Folder** workflow. First
run the vcpkg bootstrap command above, then open the repository root folder in
Visual Studio and select `vs2026-x64` or `vs2026-x86` as the configure preset.
Select Debug or Release with the matching build preset, such as
`vs2026-x64-debug`. Build targets from the CMake Targets view and run discovered
GoogleTest cases from Test Explorer. The `MapWinGIS.Core.Tests` and
`MapWinGIS.Samples.Translate` executables can also be selected for debugging.

The `.slnx` and `.vcxproj` files remain supported during this transition.
MSBuild writes to `bin\<Configuration>\<Platform>`, while CMake writes under
`out\b`; builds from the two systems do not overwrite each other's output.
Do not remove the legacy project files until CMake parity has been demonstrated
and the migration decision gate in [CMAKE-MIGRATION.md](CMAKE-MIGRATION.md) is
reviewed.

## Troubleshooting

**`Invalid triplet: x64-windows-v145`**
vcpkg was run without the overlay triplets. Build through MSBuild, or pass
`--overlay-triplets=build\triplets` when calling vcpkg directly.

**`gdal-data is missing`, reported by a test**
The dependencies are not installed yet, or `MwgDeployRuntimeData` was turned
off. Rebuild after running the bootstrap script.

**vcpkg picks the wrong compiler**
The triplets pin `VCPKG_PLATFORM_TOOLSET v145`. If an older Visual Studio is
still selected, check that the VS 2026 C++ workload is installed and delete
`vcpkg_installed\` before rebuilding.
