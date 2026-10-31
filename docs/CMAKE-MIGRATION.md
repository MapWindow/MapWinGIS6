# CMake migration: audit and decisions

This document records what the MSBuild build actually does today and the
decisions that the CMake build follows. It is the reference for every CMake
phase: when a CMake target differs from the table here, the difference must be
deliberate and explained in the PR.

The figures below come from the MSBuild baseline (`Debug|x64` and
`Release|x64`, VS 2026 18.11 MSBuild, MSVC 14.51, Windows SDK 10.0.28000.0),
collected with [`build/scripts/Compare-CompileCommands.ps1`](#parity-measurement-d6).

## Targets

| MSBuild project | Output | Type | Planned CMake target |
|---|---|---|---|
| `src/MapWinGIS.Core/MapWinGIS.Core.vcxproj` | `MapWinGIS.Core.dll` (+ `.lib`, `.pdb`) | DLL, subsystem `WINDOWS` | `MapWinGIS.Core` |
| `src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.vcxproj` | `MapWinGIS.ocx` (+ `.lib`, `.pdb`, `MapWinGIS.ActiveX.tlb`) | DLL, subsystem `WINDOWS`, `UseOfAtl=Static` | `MapWinGIS.ActiveX.Interface` (MIDL) + `MapWinGIS.ActiveX` |
| `tests/MapWinGIS.Core.Tests/MapWinGIS.Core.Tests.vcxproj` | `MapWinGIS.Core.Tests.exe` | Console | `MapWinGIS.Core.Tests` |
| `samples/Cpp/Translate.vcxproj` (project name `TranslateSample`) | `translate.exe` | Console | `MapWinGIS.Samples.Translate` (`OUTPUT_NAME translate`) |
| `samples/CSharp/ActiveXTestHost/ActiveXTestHost.csproj` | `ActiveXTestHost.exe` | .NET 10 WinForms | custom targets around `dotnet build` |

All native projects build `Debug` and `Release` for `x64` and `Win32`.

### Sources

**MapWinGIS.Core** — `MWG_CORE_BUILD`; include directories `include` (public)
and `src` (private).

- Public headers: `include/mapwingis/core/{Api,Enums,Error,Extent}.h`,
  `include/mapwingis/gdal/{GdalEnvironment,GdalConfig,GdalDiagnostics,GdalProgress,GdalOperationResult,GdalAlgorithmRunner,GdalTypedOptions,GdalModern,GdalUtils}.h`,
  `include/mapwingis/vector/{FieldInfo,VectorDataset}.h`,
  `include/mapwingis/rendering/VectorRenderScene.h`.
- Private headers: `src/internal/{GdalExecution,GdalProgressAdapterAccess,Utf8,VectorLayerAccess}.h`.
- Sources: `src/gdal/{GdalAlgorithmRunner,GdalEnvironment,GdalModern,GdalOperationResult,GdalRuntime,GdalTypedOptions,GdalUtils}.cpp`,
  `src/vector/VectorDataset.cpp`, `src/rendering/VectorRenderScene.cpp`.
- No public header includes a GDAL, GEOS or Windows header, so GDAL and GEOS
  can be `PRIVATE` dependencies of the DLL.

**MapWinGIS.ActiveX** — `_ATL_APARTMENT_THREADED`; include directories
`src/MapWinGIS.Core/include` and the MIDL output directory (`$(IntDir)`).

- Headers: `ActiveXModule.h`, `ComConversion.h`, `GdalDispatchWindow.h`
  (header-only, not listed in the vcxproj), `GdalEventSource.h`,
  `GdalOperationDispatcher.h`, `GdiVectorLayerRenderer.h`,
  `MapWinGISControl.h`, `OpenGlVectorLayerRenderer.h`, `VectorLayerRenderer.h`,
  `VectorRenderBackend.h`, `ViewTransform.h`, `resource.h`.
- Sources: `ActiveXModule.cpp`, `ComConversion.cpp`,
  `GdalOperationDispatcher.cpp`, `GdalEventSource.cpp`,
  `GdiVectorLayerRenderer.cpp`, `MapWinGISControl.cpp`,
  `OpenGlVectorLayerRenderer.cpp`, `VectorLayerRenderer.cpp`,
  `ViewTransform.cpp`, plus the generated `MapWinGIS.ActiveX_i.c` (compiled as
  C, `/std:c17 /TC`).
- Other inputs: `MapWinGIS.ActiveX.idl` (MIDL), `MapWinGIS.ActiveX.rc`
  (embeds `MapWinGISControl.rgs` as `REGISTRY` and the type library as
  `1 TYPELIB "MapWinGIS.ActiveX.tlb"`), `MapWinGIS.ActiveX.def` (linker
  `/DEF`, exports `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`,
  `DllUnregisterServer`, `DllInstall`).
- No `.manifest` file; the linker embeds its default manifest.

**MapWinGIS.Core.Tests** — `MWG_ENABLE_RENDERER_TEST_HOOKS`; include
directories `src/MapWinGIS.Core/include`, `src/MapWinGIS.ActiveX` and the
ActiveX MIDL output directory `obj/MapWinGIS.ActiveX/<Config>/<Platform>`.

- Headers: `TestData.h`.
- Sources: `ActiveXActivationTests.cpp`, `ComConversionTests.cpp`,
  `GdalAlgorithmRunnerTests.cpp`, `GdalEnvironmentTests.cpp`,
  `GdalModernCapabilitiesTests.cpp`, `GdalOperationDispatcherTests.cpp`,
  `GdalRuntimeTests.cpp`, `GdalTypedOptionsTests.cpp`, `GdalUtilsTests.cpp`,
  `Main.cpp` (defines `main`), `RenderingBaselineTests.cpp`, `TestData.cpp`,
  `VectorDatasetTests.cpp`, `VectorLayerRendererTests.cpp`,
  `VectorLayerTests.cpp`, `VectorRenderSceneTests.cpp`.
- ActiveX sources compiled again with the test hook definition:
  `GdiVectorLayerRenderer.cpp`, `OpenGlVectorLayerRenderer.cpp`,
  `GdalOperationDispatcher.cpp`, `ComConversion.cpp`,
  `VectorLayerRenderer.cpp`, `ViewTransform.cpp`.
- Project references: Core (linked) and ActiveX (build order only,
  `LinkLibraryDependencies=false`), so the OCX is beside the test executable.

**Translate sample** — `samples/Cpp/translate.cpp` (`wmain`); include
directory `src/MapWinGIS.Core/include`. It uses only the Core public API.

## Effective compiler options

Common to every C/C++ translation unit (from `build/props/MapWinGIS.Cpp.props`
and MSBuild defaults):

| Option | Source |
|---|---|
| `/W4 /WX- /wd4251 /external:W4` | `WarningLevel`, `DisableSpecificWarnings`; `/external:W4` is the MSBuild default for external headers |
| `/utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:throwingNew /bigobj` | `AdditionalOptions` |
| `/permissive- /sdl /EHsc /GS /Gd /Gm- /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline` | `ConformanceMode`, `SDLCheck`, `ExceptionHandling`, MSBuild defaults |
| `/std:c++20` (`/TP`), `/std:c17` (`/TC`, `_i.c` only) | `LanguageStandard`, `LanguageStandard_C` |
| `/Zi` | `DebugInformationFormat=ProgramDatabase`, **Debug and Release** |
| `/D UNICODE /D _UNICODE` | `CharacterSet=Unicode` |
| `/D WIN32_LEAN_AND_MEAN /D NOMINMAX /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D NTDDI_VERSION=0x0A00000B /D _CRT_SECURE_NO_WARNINGS /D MWG_VERSION="6.0.0"` | `PreprocessorDefinitions` |
| `/D _WINDLL` | MSBuild adds it for every DLL project (Core, ActiveX) |
| `/diagnostics:column /FC /MP /nologo /errorReport:queue` | Logging and parallelism only; ignored by the parity script |

Per configuration:

| Configuration | Options |
|---|---|
| Debug | `/Od /MDd /RTC1 /JMC /D _DEBUG` (`/RTC1` is dropped when `MwgEnableAsan=true`, which adds `/fsanitize=address`) |
| Release | `/O2 /Oi /Gy /guard:cf /MD /D NDEBUG` |

Per target: `MWG_CORE_BUILD` (Core), `_ATL_APARTMENT_THREADED` (ActiveX),
`MWG_ENABLE_RENDERER_TEST_HOOKS` (tests).

Not defined by any project: `WIN32`, `_WINDOWS`, `_USRDLL`, `_CONSOLE`. The
CMake defaults `/DWIN32 /D_WINDOWS` are therefore removed, so the two builds
see the same preprocessor state. `/JMC` (Just My Code, a debugger aid) is an
MSBuild default for Debug that CMake does not add; CMake reproduces it.

## Effective linker options

| Option | Configurations |
|---|---|
| `/DEBUG` | Debug and Release (PDB for every binary) |
| `/DYNAMICBASE /NXCOMPAT /TLBID:1 /MANIFEST /manifest:embed /MANIFESTUAC:"level='asInvoker' uiAccess='false'"` | All |
| `/HIGHENTROPYVA` | x64 only (`MachineX64` also sets *large address aware*) |
| `/INCREMENTAL` | Debug |
| `/INCREMENTAL:NO /OPT:REF /OPT:ICF /guard:cf` | Release |
| `/SUBSYSTEM:WINDOWS /DLL` | Core, ActiveX |
| `/SUBSYSTEM:CONSOLE` | Tests, sample |
| `/DEF:MapWinGIS.ActiveX.def` and the compiled `.res` | ActiveX |
| `kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32 ole32 oleaut32 uuid odbc32 odbccp32` | All (MSBuild default, same list as `CMAKE_CXX_STANDARD_LIBRARIES`) |
| `<vcpkg>/[debug/]lib/*.lib` | All (`VcpkgAutoLink=true`) |

### Real dependencies

`VcpkgAutoLink` links every installed import library into every project and
hides what each target uses. From the sources and `dumpbin /dependents`:

| Target | Libraries |
|---|---|
| Core | `GDAL::GDAL`, `GEOS::geos_c` (`GdalEnvironment.cpp` includes `geos_c.h` and calls `GEOSversion()`). PROJ is reached only through GDAL/OGR (`OSRSetPROJSearchPaths`). Windows: `kernel32` only. |
| ActiveX | Core, `ole32`, `oleaut32`, `uuid`, `user32`, `gdi32`, `advapi32` (registration), `opengl32` (through `#pragma comment(lib, ...)` in `OpenGlVectorLayerRenderer.cpp`), static ATL. No direct GDAL use. |
| Tests | Core, `GDAL::GDAL` (`gdal_priv.h`, `gdalalgorithm.h`, `vrtdataset.h`, `cpl_error.h`), `GTest::gtest` (not `gtest_main`: `Main.cpp` defines `main`), ATL headers (`atlbase.h`, `atlhost.h`), `ole32`, `oleaut32`, `uuid`, `user32`, `gdi32`, `opengl32`. |
| Sample | Core only. |

`dumpbin` of the MSBuild tests shows `gmock.dll` instead of `gtest.dll`: the
auto-linked `gmock.lib` happens to export the GoogleTest symbols too. With
explicit linking the CMake build depends on `gtest.dll`; that is an expected
difference.

## Generated files

| File | Producer | Consumers | MSBuild location |
|---|---|---|---|
| `MapWinGIS.ActiveX_i.h` | MIDL (`/h`) | ActiveX sources, tests (`ActiveXActivationTests.cpp`) | `obj/MapWinGIS.ActiveX/<Config>/<Platform>` |
| `MapWinGIS.ActiveX_i.c` | MIDL (`/iid`) | Compiled into the OCX | same |
| `MapWinGIS.ActiveX_p.c`, `dlldata.c` | MIDL (`/proxy`, `/dlldata`) | None; MIDL writes no proxy for this IDL | — |
| `MapWinGIS.ActiveX.tlb` | MIDL (`/tlb`) | Embedded by `MapWinGIS.ActiveX.rc` | `bin/<Config>/<Platform>` |

MIDL command line: `midl /W1 /nologo /char signed /env x64|win32 /h ... /iid ...
/proxy ... /tlb ... /target "NT60" MapWinGIS.ActiveX.idl`. The RC compiler
gets `/D UNICODE /D _UNICODE /l 0x0409` and the MIDL and output directories
as include paths.

## Custom build steps

- `MwgDeployGdalRuntimeData` (`Directory.Build.targets`, after `Build`, every
  project unless `MwgDeployRuntimeData=false`): copies
  `<vcpkg>/share/gdal` → `<OutDir>/gdal-data` and `<vcpkg>/share/proj` →
  `<OutDir>/proj`, and warns when the GDAL data is missing.
  `GdalEnvironment` looks for both folders beside the loaded module.
- `MwgInvalidateVcpkgInstallOnFeatureChange` / `MwgRecordVcpkgInstalledFeatures`:
  MSBuild-only workaround for vcpkg's feature-blind stamp file. Not needed in
  CMake: `vcpkg.cmake` runs `vcpkg install` on every configure with
  `VCPKG_MANIFEST_FEATURES`.

## Runtime deployment

- `VcpkgApplocalDeps=true` copies the dependent vcpkg DLLs (GDAL, PROJ, GEOS,
  curl, OpenSSL, SQLite, ...) beside each binary.
- `gdal-data/` and `proj/` (including `proj.db`) come from the custom step
  above.
- All binaries share `bin/<Config>/<Platform>`. `ActiveXActivationTests` loads
  `MapWinGIS.ocx` from the test executable's folder, and the C# host uses
  registration-free COM through `app.manifest`, so the OCX must sit beside it.

## Architecture-specific settings

| Setting | x64 | Win32 |
|---|---|---|
| vcpkg triplet | `x64-windows-v145` | `x86-windows-v145` |
| vcpkg host triplet | `x64-windows-v145` | `x64-windows-v145` |
| MIDL `/env` | `x64` | `win32` |
| `/HIGHENTROPYVA` | yes | no |
| C# `PlatformTarget` / RID | `x64` / `win-x64` | `x86` / `win-x86` |

## Hard-coded output paths

These assume `<repo>/bin/<Config>/<Platform>`:

- `samples/CSharp/ActiveXTestHost/Run-ActiveXTestHost.ps1`
- `tests/MapWinGIS.Core.Tests/Verify-ActiveXActivation.ps1` (OCX and test executable)
- `samples/CSharp/ActiveXTestHost/ActiveXTestHost.csproj` (`OutputPath`)
- `.github/workflows/build.yml` (test executable, Release artifact upload)

## Prerequisites

- Visual Studio 2026 with MSVC v145, Windows SDK 10.0.28000.0 and **C++ ATL
  for latest v145 build tools for x64 and x86** (`atlmfc/lib/x64` and
  `atlmfc/lib/x86`).
- CMake ≥ 3.28. VS 2026 and CLion both bundle CMake 4.3.1, which provides the
  `Visual Studio 18 2026` generator and detects the v145 toolset.
- Ninja (bundled with VS and CLion) for single-config builds.
- MSBuild is only needed for the legacy build and parity comparisons. It is not
  on `PATH` outside a Developer PowerShell; scripts resolve it through
  `MWG_MSBUILD`, then `PATH`, then `vswhere`.

## Decisions

### D1. Output directory layout

CMake writes to its build tree, never to `<repo>/bin`: runtime files to
`<binaryDir>/bin`, import libraries to `<binaryDir>/lib`, set once through
`CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY` and
`CMAKE_ARCHIVE_OUTPUT_DIRECTORY`. All targets and runtime dependencies of one
build share one runtime folder. Preset build directories are short
(`out/b/x64d`, `out/b/x64r`, `out/b/x86d`, `out/b/x86r`, `out/b/vs-x64`,
`out/b/vs-x86`) to stay clear of `MAX_PATH`. The scripts listed above gain an
optional `-BinDirectory` parameter in the phase that first needs it; their
default stays `bin/<Config>/<Platform>`.

### D2. vcpkg install tree and triplets

CMake uses its own install tree per platform,
`<repo>/vcpkg_installed/cmake-<Platform>`, shared by the Debug and Release
presets and separate from the MSBuild tree `<repo>/vcpkg_installed/<Platform>`
(different stamp files; a different feature set would prune packages the other
build needs). The binary cache makes the second install cheap. Presets pin
`VCPKG_TARGET_TRIPLET`, `VCPKG_HOST_TRIPLET=x64-windows-v145` and
`VCPKG_MANIFEST_FEATURES`; overlay triplets stay in `vcpkg-configuration.json`.
The vcpkg root is resolved before `project()` in the same order as
`bootstrap-vcpkg.ps1` (`MWG_VCPKG_ROOT`, sibling `../vcpkg`, `VCPKG_ROOT`),
unless `CMAKE_TOOLCHAIN_FILE` is already set.

### D3. MIDL output ownership

One custom target, `MapWinGIS.ActiveX.Interface`, owns the only MIDL
invocation and writes to `<build>/src/MapWinGIS.ActiveX/midl`. A small library
target exposes that include directory and the generated `_i.c`. The OCX and
the tests both depend on that target, never on each other's intermediates.

### D4. ActiveX sources compiled into the tests

Option (a): one source-list variable for the six shared ActiveX sources,
compiled once into the OCX and once into the test executable with
`MWG_ENABLE_RENDERER_TEST_HOOKS`. This matches the MSBuild build and needs no
production code change. Making the hooks a runtime switch is out of scope.

### D5. C# host project reference

The `ProjectReference` from `ActiveXTestHost.csproj` to
`MapWinGIS.ActiveX.vcxproj` becomes conditional on
`'$(MwgCMakeBuild)' != 'true'`. The CMake custom targets pass
`-p:MwgCMakeBuild=true -p:BuildProjectReferences=false`, so `dotnet build`
never tries to build a `.vcxproj` or races CMake for the OCX. The imports in
`Directory.Build.props/.targets` are guarded by `Exists` and harmless under
`dotnet build`; the csproj already sets `MwgDeployRuntimeData=false`.

### D6. Parity measurement

Parity is measured with `build/scripts/Compare-CompileCommands.ps1`:

- Compile options per translation unit and link options per binary, from the
  detailed MSBuild log and from CMake's `compile_commands.json` plus a verbose
  build log. Paths are normalised (`<repo>`, `<vcpkg_installed>`, `<intdir>`,
  `<bindir>`, `<builddir>`), the vcpkg platform folder is collapsed and option
  order is ignored.
- `dumpbin /dependents /exports /headers` summaries for selected binaries.
- Unique warning counts for both builds; CMake must not exceed MSBuild.

The script runs against the MSBuild build alone (baseline) or with
`-CMakeBuildDir` for a comparison; MSBuild targets that the CMake build does
not have yet are skipped, so each phase can be measured on its own.

```powershell
# Baseline: rebuilds MapWinGIS6.slnx with a detailed log under out\parity
pwsh -File build\scripts\Compare-CompileCommands.ps1 -Configuration Debug -Platform x64 `
    -Binaries 'MapWinGIS.Core.dll,MapWinGIS.ocx,MapWinGIS.Core.Tests.exe,translate.exe'

# Comparison with a CMake Ninja build
cmake --build --preset x64-debug -- -v > out\parity\cmake-x64d.log
pwsh -File build\scripts\Compare-CompileCommands.ps1 -MSBuildLog out\parity\msbuild-Debug-x64.log `
    -CMakeBuildDir out\b\x64d -CMakeBuildLog out\parity\cmake-x64d.log -Binaries MapWinGIS.Core.dll
```

## Baseline

| | Debug x64 | Release x64 |
|---|---|---|
| Translation units / linked binaries | 42 / 4 | 42 / 4 |
| Warnings | 0 | 0 |
| `MapWinGIS.Core.dll` dependents | `gdald`, `geos_c`, CRT | `gdal`, `geos_c`, CRT |
| `MapWinGIS.ocx` exports | 5 (`DllCanUnloadNow`, `DllGetClassObject`, `DllInstall`, `DllRegisterServer`, `DllUnregisterServer`) | 5 |
| DLL characteristics | Dynamic base, NX, high-entropy VA, large address aware | + Control Flow Guard |
| Native tests | 173 run, 170 passed, 3 skipped | — |

The three skipped tests need `MWG_TEST_ACTIVEX_HOST`, `MWG_RUN_NETWORK_GDAL_TESTS`
or `MWG_RUN_RENDERING_BASELINE`. The Win32 baseline was collected in Phase 1
(Debug and Release, 42 TUs / 4 links, 0 warnings).

## Parity status

### MapWinGIS.Core (Phase 1)

Measured for Debug and Release, x64 and Win32 (Ninja presets):

- Compile options: none lost; 0 warnings in both builds.
- Link options: none lost. The remaining differences are explained by the
  parity script (`diff.links.<binary>.Explained`):
  - `/MANIFEST /manifest:embed /MANIFESTUAC:...`: CMake's `vs_link_dll` wrapper
    generates and embeds the manifest with `mt.exe`; the embedded manifest
    resource is byte-identical to the MSBuild one.
  - `/TLBID:1`: only relevant when a type library resource is embedded (OCX).
  - vcpkg `/LIBPATH` and `*.lib` auto-link: replaced by `GDAL::GDAL` and
    `GEOS::geos_c` with full library paths.
  - `odbc32.lib odbccp32.lib`: MSBuild defaults, unused, not in CMake's default
    `CMAKE_CXX_STANDARD_LIBRARIES`.
  - `/version:0.0`: added by CMake; the image version is 0.0 in both builds.
- Added compile options, from the imported targets: `/I<vcpkg>/include/geos`
  (GEOS's interface include directory) and, in Debug, `/DGDAL_DEBUG`
  (`GDAL::GDAL` interface definition for the debug library).
- Win32 needs `/Oy- /analyze-` and, in Release, `/SAFESEH`; MSBuild adds them
  by default for that platform.
- `dumpbin`: identical dependents, exports (260), subsystem and DLL
  characteristics.

### MapWinGIS.ActiveX (Phase 2)

Measured for Debug and Release, x64 and Win32 (Ninja presets):

- Compile and link options: none lost; 0 warnings. Explained differences:
  - The MIDL include directory is `<build>/src/MapWinGIS.ActiveX/midl`
    instead of the intermediate directory (D3); the generated `_i.c` is
    compared by file name.
  - No vcpkg include directory: the OCX has no direct vcpkg dependency.
  - The compiled resource is named `MapWinGIS.ActiveX.rc.res`, and the Core
    import library comes from `target_link_libraries` instead of the project
    reference.
- CMake needs two explicit settings to match MSBuild: `DEFINE_SYMBOL ""` (no
  `MapWinGIS_ActiveX_EXPORTS`; the `.def` file exports the entry points) and
  `/TC` for C sources (CMake passes `/TP` for C++ only).
- `dumpbin`: identical dependents, the five COM exports, subsystem and DLL
  characteristics.
- The generated type library is byte-identical to the MSBuild one, and
  `MapWinGIS.ActiveX_i.h` is identical below its generated header comment.
- MIDL runs through one custom command for all generators
  (`cmake/MwgMidl.cmake`) with the MSBuild options. This IDL produces no
  proxy or `dlldata.c`; MIDL is told to write them into the output directory
  anyway, so nothing can land in the source tree.
- Per-user registration (`regsvr32 /n /i:user`), `New-Object -ComObject
  MapWinGIS6.Control` and unregistration work for the x64 and Win32 OCX.
- Three clean `-j32` Ninja builds in a row succeeded (no MIDL/RC race), and the
  Visual Studio generator builds x64 Debug/Release and x86 Debug.
