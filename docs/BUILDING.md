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
pwsh -File tests\MapWinGIS.Core.Tests\Verify-ActiveXActivation.ps1 -Configuration Debug -Platform x64
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
control recreation. It can activate the OCX without a registration. The
separate native activation script above retains its own temporary
registration-based test.

### Validating both native architectures

Build and run the native tests for each platform. Use `-restore` when
alternating architectures so the matching vcpkg triplet and managed host
assets are restored:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug\x64\MapWinGIS.Core.Tests.exe

msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=Win32 -m
bin\Debug\Win32\MapWinGIS.Core.Tests.exe
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
msbuild MapWinGIS6.slnx -restore -p:Platform=x64 -p:Configuration=Release -p:MwgVcpkgFeatures="tests;full-drivers"
```

### Runtime data

vcpkg installs `gdal-data` and `proj.db` under
`vcpkg_installed\<Platform>\<triplet>\share`. Each platform has its own install
tree because vcpkg removes other triplets from a shared tree, which would break
alternating Win32 and x64 builds.
A target in `Directory.Build.targets` copies them into the output folder as
`gdal-data\` and `proj\`, and `GdalEnvironment` points GDAL and PROJ at them.
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
```

Or use Test Explorer in Visual Studio, which picks up the GoogleTest adapter
automatically.

For CI, write a JUnit-style report:

```powershell
bin\Release\x64\MapWinGIS.Core.Tests.exe --gtest_output=xml:testresults.xml
```

## Extra build switches

| Property | Default | Effect |
|---|---|---|
| `MwgWindowsSdk` | `10.0.28000.0` | Windows SDK version. `10.0` means "newest installed" |
| `MwgVcpkgRoot` | resolved (see above) | vcpkg location |
| `MwgVcpkgFeatures` | `tests` | Semicolon-separated manifest features |
| `MwgEnableAsan` | `false` | Adds `/fsanitize=address` |
| `MwgDeployRuntimeData` | `true` | Copies `gdal-data` and `proj` to the output folder |

## CMake migration

CMake is being introduced as the primary native build. Until it is complete,
the MSBuild solution above stays the supported build. The audit of the current
MSBuild settings and the decisions the CMake build follows are in
[CMAKE-MIGRATION.md](CMAKE-MIGRATION.md).

### Building with CMake

The CMake build currently covers `MapWinGIS.Core.dll`; the ActiveX control,
tests and samples follow. It needs CMake 3.28 or later and Ninja; both ship
with Visual Studio 2026 and CLion. Run the bootstrap script first, then
configure from a VS 2026 Developer PowerShell (or any shell after
`vcvarsall.bat x64`):

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
```

| Preset | Generator | Build directory |
|---|---|---|
| `x64-debug`, `x64-release` | Ninja, x64 | `out\b\x64d`, `out\b\x64r` |
| `x86-debug`, `x86-release` | Ninja, Win32 (vcvars `x86` or `amd64_x86`) | `out\b\x86d`, `out\b\x86r` |
| `vs2026-x64`, `vs2026-x86` | Visual Studio 18 2026, `-T v145` (build presets `vs2026-x64-debug`, ...) | `out\b\vs-x64`, `out\b\vs-x86` |

- Output goes to `<build directory>\bin` (`bin\<Config>` for the Visual Studio
  presets) and import libraries to `lib`, never to the MSBuild `bin\` folder, so
  both builds can be used side by side.
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
| `MWG_BUILD_SAMPLES` | `ON` | — |

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

To measure flag and binary parity, `build\scripts\Compare-CompileCommands.ps1`
records a detailed MSBuild log and writes a normalised report to `out\parity\`.
It finds MSBuild through `MWG_MSBUILD`, `PATH` or `vswhere`, so it also works
when Visual Studio is not on `PATH` or not installed on `C:`.

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
