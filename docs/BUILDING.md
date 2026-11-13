# Building MapWinGIS6

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| Visual Studio 2026 | 18.10 or later | Workload **Desktop development with C++** |
| CMake | 3.28 or later | Use the Visual Studio 2026-bundled version for its generator |
| Ninja | Current | Required by the Ninja presets and CLion profiles |
| MSVC toolset | v145 (14.51 or later) | Installed with the workload |
| Windows SDK | 10.0.28000.0 | Override with `/p:MwgWindowsSdk=10.0` to use the newest installed instead |
| ATL | C++ ATL for latest v145 build tools | Install for each architecture used (x64 and/or x86); MFC is not required |
| Git | any recent version | Needed to clone vcpkg |
| .NET SDK | 10 | Required for the C# ActiveX test host |
| PowerShell | 7 (`pwsh`) | Required by the bootstrap and host scripts |
| Disk space | about 15 GB | vcpkg sources, build trees and binaries |

The ActiveX project uses ATL and statically links it.

## First build with CMake

```powershell
# From a VS 2026 Developer PowerShell: bootstrap, configure, build, and test x64 Debug.
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

The first configure installs GDAL and its dependencies and can take a while;
later builds reuse the vcpkg binary cache. See [Building with CMake](#building-with-cmake)
for other presets, Visual Studio, and CLion.

## Legacy MSBuild build

The `.slnx` and `.vcxproj` files remain supported during the CMake transition
and are built in parallel in CI. After bootstrapping vcpkg, use:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
```

Or open `MapWinGIS6.slnx` in Visual Studio. The legacy build writes to
`bin\<Configuration>\<Platform>`; CMake writes under `out\b`, so their outputs
do not overwrite one another. If `msbuild` is not on `PATH`, see
[Finding MSBuild](#finding-msbuild-for-the-legacy-build).

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
| Output | MSBuild: `bin\<Configuration>\<Platform>\`; CMake build tree: `out\b\<preset directory>\` (runtime files under `bin\`) |
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
Release for x64 with both CMake and MSBuild. Pushes to `main` also cover Win32.
Each job has a 180-minute timeout.

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

## Running the legacy MSBuild tests

These commands run the MSBuild test executables:

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

CMake is the primary native build. The `.slnx` and `.vcxproj` files remain
supported during the transition, and CI validates both build systems. The
audit of the MSBuild settings and the decisions followed by CMake are in
[CMAKE-MIGRATION.md](CMAKE-MIGRATION.md).

To measure flag and binary parity, `build\scripts\Compare-CompileCommands.ps1`
records a detailed MSBuild log and writes a normalised report to `out\parity\`.
It finds MSBuild through `MWG_MSBUILD`, `PATH`, or `vswhere`, including Visual
Studio installations outside the default `C:\Program Files` location.

### Finding MSBuild for the legacy build

MSBuild is not normally on `PATH` in a plain PowerShell or CLion terminal. Use
a Visual Studio Developer PowerShell, set `MWG_MSBUILD` for the parity script,
or locate the executable with `vswhere`:

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild was not found. Install the Visual Studio MSBuild component.' }
& $msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
```

The parity script checks `MWG_MSBUILD`, then `PATH`, then `vswhere`. Do not
hardcode a Visual Studio installation path in repository files.

### Building with CMake

The CMake build covers `MapWinGIS.Core.dll`, `MapWinGIS.ocx`, and the Core
GoogleTest suite. It does not yet build the ActiveX-specific test executable;
run the legacy ActiveX tests until CMake registers that suite. Set
`MWG_BUILD_SAMPLES=ON` to add the `translate` sample and, when .NET and
PowerShell 7 are installed, targets to build and launch the C# ActiveX test
host. The OCX needs the **C++ ATL for latest v145 build tools** component for
each architecture you build. CMake 3.28 or later is required; use the Visual
Studio 2026-bundled CMake for the Visual Studio 18 2026 generator. Run the
vcpkg bootstrap first. Ninja presets need an MSVC environment: use a Visual
Studio toolchain in CLion or configure from a VS 2026 Developer PowerShell.
With the Ninja generator, MIDL also needs `cl.exe` on `PATH`.

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

The root `CMakeLists.txt` resolves the vcpkg root before `project()` in this
order: `MWG_VCPKG_ROOT` (cache variable or environment variable), the sibling
`..\vcpkg` directory, then `VCPKG_ROOT`. Set `MWG_VCPKG_ROOT` to override the
location. A `CMAKE_TOOLCHAIN_FILE` supplied by a preset or command line takes
precedence over automatic resolution. If no root is found, run the bootstrap
script above.

The CMake build uses a separate vcpkg install tree per platform under
`vcpkg_installed\cmake-x64` or `vcpkg_installed\cmake-Win32`. Debug and Release
share the platform tree; it is separate from the MSBuild install. vcpkg
reconfigures the tree using `VCPKG_MANIFEST_FEATURES` on every CMake configure.
Feature changes take effect after reconfiguring. In CLion, use **Tools | CMake
| Reset Cache and Reload Project** if a cached preset value is stale.

Sample and C# host targets are optional. Enable them when configuring:

```powershell
cmake --preset x64-debug -DMWG_BUILD_SAMPLES=ON
```

Then build individual targets:

```powershell
cmake --build --preset x64-debug --target MapWinGIS.Core
cmake --build --preset x64-debug --target MapWinGIS.ActiveX
cmake --build --preset x64-debug --target MapWinGIS.Samples.Translate
cmake --build --preset x64-debug --target build_activex_host
cmake --build --preset x64-debug --target run_activex_host
cmake --build --preset x64-debug --target run_activex_host_automated
```

The sample executable is `out\b\x64d\bin\translate.exe`. The C# host targets
need .NET 10; the two run targets also need PowerShell 7 and an interactive
desktop session.

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
- CMake uses its own install tree, `vcpkg_installed\cmake-<Platform>`, shared
  by Debug and Release and separate from the MSBuild tree; the binary cache
  makes it cheap.
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

| CMake setting | Default / unset behavior | MSBuild equivalent |
|---|---|---|
| `MWG_WINDOWS_SDK` | `10.0.28000.0` | `MwgWindowsSdk` (`10.0` selects newest) |
| `MWG_VCPKG_ROOT` | resolved | `MwgVcpkgRoot` |
| `VCPKG_MANIFEST_FEATURES` | `tests` | `MwgVcpkgFeatures` |
| `MWG_ENABLE_ASAN` | `OFF` | `MwgEnableAsan` |
| `MWG_DEPLOY_RUNTIME_DATA` | `ON` | `MwgDeployRuntimeData` |
| `MWG_BUILD_TESTS` | `ON` | — (adds the `tests` feature) |
| `MWG_BUILD_ACTIVEX` | `ON` | — (set `OFF` for a Core-only build) |
| `MWG_BUILD_SAMPLES` | unset (false in `if()`) | — |

`MWG_BUILD_SAMPLES` is read by CMake but is not declared with `option()`.
Passing `-DMWG_BUILD_SAMPLES=ON` sets it for that configure; because there is
no declared option, it has no default cache entry and typos are not checked.

With the CMake Visual Studio generator, `MWG_WINDOWS_SDK=10.0` selects the
newest installed SDK. With Ninja, `10.0` accepts the SDK already selected by
`vcvars`; Ninja cannot select or pin an SDK.

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

### Using CMake in CLion

1. Install CLion with CMake and Ninja available. Bootstrap vcpkg and run one
   command-line configure before the first CLion reload:

   ```powershell
   pwsh -File build\scripts\bootstrap-vcpkg.ps1
   cmake --preset x64-debug
   ```

   This starts the first GDAL build in a visible command window instead of
   leaving CLion apparently idle during its reload.
2. Open the repository root folder in CLion as a CMake project. Select the
   root `CMakeLists.txt`; do not open `.mscppls\compile_commands.json` as the
   project. That database is produced by the legacy MSBuild tooling and is not
   the CMake project.
3. In **Settings | Build, Execution, Deployment | Toolchains**, add a Visual
   Studio toolchain for each architecture. Use `amd64` for x64 and `x86` or
   `amd64_x86` for Win32. The toolchain must provide the MSVC compiler
   environment and `cl.exe` on `PATH` for MIDL.
4. In **Settings | Build, Execution, Deployment | CMake**, use the configure
   profiles imported from `CMakePresets.json`. Assign the matching Visual
   Studio toolchain to each profile. The preset's `architecture` field informs
   Visual Studio but does not select CLion's toolchain architecture. Disable
   CLion's extra default profiles, such as `RelWithDebInfo` and `MinSizeRel`;
   only Debug and Release are supported.
5. Reload the project and select the profile for the desired architecture and
   configuration. For x86, use `x86-debug` or `x86-release` with the matching
   x86 toolchain.

Build `MapWinGIS.Core`, `MapWinGIS.ActiveX`, and `MapWinGIS.Core.Tests` from
CLion's CMake targets. To expose the sample and C# host targets, add
`-DMWG_BUILD_SAMPLES=ON` to the **CMake options** field in the selected profile
(**Settings | Build, Execution, Deployment | CMake**). CLion's GoogleTest
integration can run, filter, and debug individual cases. The C# host can be
built and launched through `build_activex_host` and `run_activex_host`;
these targets automatically ensure `MapWinGIS.ActiveX` and runtime data
dependencies are up to date. CLion's native debugger does not debug the
managed C# code. See [Testing](TESTING.md#cmake-and-clion) for test-running
details.

### Using CMake in Visual Studio

The CMake presets support Visual Studio 2026's **Open Folder** workflow. First
run the vcpkg bootstrap command above, then open the repository root folder in
Visual Studio and select `vs2026-x64` or `vs2026-x86` as the configure preset.
Select Debug or Release with the matching build preset, such as
`vs2026-x64-debug`. Build targets from the CMake Targets view and run discovered
GoogleTest cases from Test Explorer. The `MapWinGIS.Core.Tests` executable can
also be selected for debugging. The `MapWinGIS.Samples.Translate` target is
available when configured with `-DMWG_BUILD_SAMPLES=ON`.

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

**CMake reports a stale preset value or missing manifest feature**
Reconfigure after changing `VCPKG_MANIFEST_FEATURES` or another cached value.
In CLion, select **Tools | CMake | Reset Cache and Reload Project**.

**vcpkg fails because a build path is too long**
Use the short preset build directories under `out\b`. If Windows still rejects
long paths, enable the Windows `LongPathsEnabled` policy and restart the
terminal before configuring again.

**MIDL cannot find `cl.exe` or CLion fails to detect the Visual Studio toolset**
Ninja needs an MSVC environment. Assign the matching Visual Studio toolchain
to the CLion profile, or configure from a VS 2026 Developer PowerShell. If
Visual Studio is installed on a non-standard drive or path (for example,
`D:\Program Files\Microsoft Visual Studio\18\Community`), ensure the Visual
Studio toolchain in **Settings | Build, Execution, Deployment | Toolchains**
points directly to that installation directory rather than relying on automatic
discovery. Ensure `cl.exe` is on `PATH` before rerunning CMake.

**ATL headers or libraries are missing**
Install **C++ ATL for latest v145 build tools** in Visual Studio Installer for
each target architecture. The x86 ATL component is needed for Win32 builds;
the x64 component alone is not enough.

**Ninja reports a Windows SDK mismatch**
Ninja takes the SDK from the vcvars environment and cannot pin it. Set the
Visual Studio toolchain's Windows SDK version to `MWG_WINDOWS_SDK`, or use
`-DMWG_WINDOWS_SDK=10.0` to accept the SDK already selected by vcvars. For the
Visual Studio generator, `10.0` selects the newest installed SDK.

**The C# ActiveX test host cannot find the OCX or executable**
The CMake `run_activex_host` targets pass the runtime directory automatically.
When launching the C# host script directly, pass that directory with
`-BinDirectory`, for example `-BinDirectory out\b\x64d\bin`. This directory
override does not enable native ActiveX hosting verification: that check
requires the legacy `MapWinGIS.ActiveX.Tests.exe` and its hosting test.
