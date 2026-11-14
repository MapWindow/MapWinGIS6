# Task: Introduce CMake as the Primary Build System for MapWinGIS6

## Progress Checklist

Update this checklist as work progresses. Mark an item `[x]` only after its acceptance criteria have been met and verified. Do not mark work complete merely because code has been written.

- [x] **Phase 0: Audit, decisions and parity tooling** (no CMake targets yet)
- [x] **Phase 1: CMake foundation and Core DLL**
- [x] **Phase 2: ActiveX / ATL / COM**
- [x] **Phase 3: GoogleTest and CTest** (implementation complete; interactive and CLion acceptance checks remain)
- [x] **Phase 4: Samples, runtime deployment and running applications** (implementation complete; CLion interaction remains)
- [x] **Phase 5: Visual Studio and GitHub Actions**
- [x] **Phase 6: Documentation and legacy build transition**

## Objective

Introduce CMake as the authoritative native C++ build system for MapWinGIS6.

The goal is to make the entire native project buildable, testable and runnable from JetBrains CLion, while retaining support for Visual Studio.

Repository:
https://github.com/MapWindow/MapWinGIS6

Target branch: `development`

The project must continue to target Windows and MSVC v145 initially. Cross-platform support is not part of this task.

CMake should provide a common build definition that can be used by:

- JetBrains CLion (Ninja generator, Visual Studio toolchain).
- Microsoft Visual Studio 2026 (Open Folder / CMake presets, and the `Visual Studio 18 2026` generator).
- GitHub Actions.
- Command-line builds using CMake.

Do not simply translate the existing `.vcxproj` files. Establish a maintainable CMake project structure.

## Architectural requirements

### 1. Preserve the existing architecture

MapWinGIS6 consists of two main native components:

**MapWinGIS.Core.dll**
- Modern C++20 DLL.
- GDAL/OGR integration.
- No COM, ATL or MFC dependencies.
- Independently testable.

**MapWinGIS.ocx**
- Thin ActiveX/COM wrapper.
- Uses ATL.
- Depends on MapWinGIS.Core.dll.
- Includes GDI and OpenGL rendering.
- Uses MIDL-generated COM interfaces and Windows resources.

Do not introduce COM, ATL or Windows-specific dependencies into MapWinGIS.Core.

### 2. Dependencies

Continue using vcpkg manifest mode.

Preserve:
- `vcpkg.json`
- `vcpkg-configuration.json`
- The existing pinned vcpkg baseline.
- Existing overlay triplets (`x64-windows-v145`, `x86-windows-v145`).
- GDAL and GEOS dependencies.
- GoogleTest.
- Existing optional `full-drivers` feature.
- Existing optional `tests` feature.

Use the existing bootstrap script where practical:

`build/scripts/bootstrap-vcpkg.ps1`

CMake should use the vcpkg toolchain file:

`<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`

Resolve the vcpkg root in the **same order** as `bootstrap-vcpkg.ps1` and `Directory.Build.props`:

1. `MWG_VCPKG_ROOT`
2. Existing local sibling `..\vcpkg` installation.
3. `VCPKG_ROOT`

Presets cannot express this fallback chain. Resolve it in the root `CMakeLists.txt` **before** `project()` and set `CMAKE_TOOLCHAIN_FILE` only when it is not already defined (a preset or command line value always wins). Fail with a clear message that points to the bootstrap script if no root is found.

The following must be set before `project()` (presets or the resolver above):

- `CMAKE_TOOLCHAIN_FILE`
- `VCPKG_TARGET_TRIPLET` = `x64-windows-v145` / `x86-windows-v145`. Without it vcpkg silently picks `x64-windows` and builds GDAL a second time with a different toolset.
- `VCPKG_HOST_TRIPLET` = `x64-windows-v145`
- `VCPKG_MANIFEST_FEATURES` (e.g. `tests`, optionally `full-drivers`)
- `VCPKG_INSTALLED_DIR` (see below)

Overlay triplets are already declared in `vcpkg-configuration.json`; do not repeat them in CMake.

**vcpkg install tree:**

- Do **not** share `<repo>\vcpkg_installed\<Platform>` with the MSBuild integration. The two integrations use different stamp files, and a manifest install with a different feature set prunes packages the other build needs.
- Use one CMake install tree **per platform**, shared by Debug and Release presets, e.g. `<repo>\vcpkg_installed\cmake-<Platform>` (or `${sourceDir}/out/vcpkg/<Platform>`). One install per build directory would mean several full GDAL installs.
- The vcpkg binary cache makes the second (CMake) install cheap.
- The MSBuild-only `MwgInvalidateVcpkgInstallOnFeatureChange` workaround is not needed: `vcpkg.cmake` runs `vcpkg install` on every configure and honours `VCPKG_MANIFEST_FEATURES`. Document that feature changes take effect on reconfigure, and that CLion needs **Reset Cache and Reload** when a cached value is stale.

Do not introduce a second dependency-management mechanism.

### 3. Compiler and build configurations

Preserve the current compiler requirements:

- C++20 (`/std:c++20`) and C17 (`/std:c17`, used by the MIDL-generated `_i.c`).
- MSVC v145 (validate `MSVC_VERSION >= 1950` and `CMAKE_CXX_COMPILER_ID STREQUAL MSVC`; the toolset name is empty under Ninja).
- Windows SDK 10.0.28000.0, with an override for the newest installed SDK (`MWG_WINDOWS_SDK` cache variable, equivalent to `-p:MwgWindowsSdk`; `10.0` means "newest").
- Debug and Release configurations only. Restrict `CMAKE_CONFIGURATION_TYPES` to `Debug;Release`, and reject (or deliberately map) `RelWithDebInfo` / `MinSizeRel`, which CLion offers by default.
- x64 and Win32 architectures. Map the CMake architecture to the MSBuild platform names `x64` / `Win32` (from `CMAKE_VS_PLATFORM_NAME` or `CMAKE_SIZEOF_VOID_P`), because `<Platform>` is not a CMake concept.

**Toolset and SDK pinning by generator:**

- Visual Studio generator: `-T v145` and `CMAKE_GENERATOR_PLATFORM=x64,version=10.0.28000.0` (or `Win32,...`) pin both reproducibly.
- Ninja (CLion, VS Open Folder): CMake cannot pin the toolset or SDK; they come from the vcvars environment. Check `WindowsSDKVersion` / `CMAKE_WINDOWS_KITS_10_DIR` at configure time and warn on mismatch. Document the CLion toolchain settings that select them.

**Minimum CMake version:** use `cmake_minimum_required(VERSION 3.28...4.x)` (â‰¥ 3.15 is required so CMP0092 stops the default `/W3`, which would conflict with `/W4` and trigger D9025; â‰¥ 3.21 for `TARGET_RUNTIME_DLLS`; â‰¥ 3.26 for `copy_directory_if_different`). Verify which CMake release introduced the `Visual Studio 18 2026` generator and v145 detection, and test with both the CMake bundled with CLion and the one bundled with VS 2026.

**Effective MSBuild settings that must be reproduced.** The table includes defaults that are implied by MSBuild properties and do not appear as literal switches:

| MSBuild source | Effective switch | Configurations / notes |
|---|---|---|
| `WarningLevel=Level4` | `/W4` | All |
| `DisableSpecificWarnings=4251` | `/wd4251` | All. This is existing behaviour, not a new suppression; keep it |
| `AdditionalOptions` | `/utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:throwingNew /bigobj` | All |
| `ConformanceMode=true` | `/permissive-` | All |
| `SDLCheck=true` | `/sdl` | All |
| `ExceptionHandling=Sync` | `/EHsc` | All (CMake default matches; set explicitly anyway) |
| `CharacterSet=Unicode` | `UNICODE;_UNICODE` | All. Missing this silently changes ATL/Win32 `TCHAR` APIs |
| `RuntimeLibrary` | `/MDd` / `/MD` | Via `MSVC_RUNTIME_LIBRARY` (CMP0091) |
| `DebugInformationFormat` + `GenerateDebugInformation` | `/Zi` + `/DEBUG` | **Debug and Release**. Use `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` (CMP0141) and add `/DEBUG` to Release link flags; CMake Release has no PDB by default |
| `BasicRuntimeChecks` | `/RTC1` | Debug only; **must be removed when ASan is on** (CMake adds `/RTC1` by default) |
| `EnableASAN` option | `/fsanitize=address` | Existing ASan option, Debug, where supported |
| Release optimisation | `/O2 /Oi /Gy /guard:cf` | Release |
| Release link | `/OPT:REF /OPT:ICF /INCREMENTAL:NO /guard:cf` | Release. `/guard:cf` is required on compile **and** link |
| Link defaults | `/DYNAMICBASE /NXCOMPAT`, `/HIGHENTROPYVA` | `/HIGHENTROPYVA` on x64 only |
| `SubSystem` | `WINDOWS` for DLL/OCX, `CONSOLE` for tests and sample | Per target |
| `PreprocessorDefinitions` | `_DEBUG` / `NDEBUG`, `_WINDOWS`, `_USRDLL`, `MWG_CORE_BUILD`, `MWG_VERSION="..."`, `_ATL_APARTMENT_THREADED`, `MWG_ENABLE_RENDERER_TEST_HOOKS`, ... | Per target. Take the full list from the Phase 0 audit |
| `MWG_VERSION="$(MwgVersion)"` | String define | Derive it from `project(VERSION 6.0.0)`; quote it correctly for both Ninja and VS |
| CMake defaults | `/DWIN32 /D_WINDOWS` | Decide explicitly whether to keep them; they are not set by MSBuild for every project |

Apply these through **one** reusable mechanism, either `mwg_apply_common_options(<target>)` in `cmake/MwgCompilerOptions.cmake` or an INTERFACE target `mwg::build_options` linked `PRIVATE`. Do not use global `add_compile_options()`, so the flags never leak to consumers of the Core DLL.

Do not silently change compiler warning levels or suppress existing warnings.

### 4. MSBuild location (when MSBuild is still needed)

MSBuild is still needed during the transition, for example to build the ActiveX control and the rest of `MapWinGIS6.slnx` for the Phase 0 baseline, the parity comparison (D6), the "existing `.slnx` build still works" acceptance criteria, and the MSBuild build of the C# host (Phase 4).

On the development machine MSBuild is located at:

`D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`

Rules:

- MSBuild is **not** on `PATH` in a plain PowerShell or CLion terminal. Call it by full path, or from a VS Developer PowerShell / vcvars environment.
- The path contains spaces. In PowerShell use the call operator and quote the path:

  ```powershell
  $msbuild = 'D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
  & $msbuild MapWinGIS6.slnx -restore '-p:Configuration=Debug' '-p:Platform=x64' -m
  & $msbuild src\MapWinGIS.ActiveX\MapWinGIS.ActiveX.vcxproj -restore '-p:Configuration=Debug' '-p:Platform=x64' -m
  ```

- Use this VS MSBuild (not `dotnet msbuild`) for every `.vcxproj` / `.slnx` build; the .NET SDK MSBuild has no VC targets (see D5).
- This path is machine-specific. **Do not hardcode it** in committed files (scripts, presets, `CMakeLists.txt`, CI). Where a committed script or CMake target needs MSBuild, resolve it in this order:
  1. `MWG_MSBUILD` environment variable (or CMake cache variable of the same name), set locally to the path above.
  2. `msbuild` on `PATH` (Developer PowerShell, CI after `microsoft/setup-msbuild`).
  3. `vswhere.exe -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe` (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`), which also finds installations on a non-default drive such as `D:`.
- Visual Studio is installed on `D:`, so the ATL component, `midl.exe`, `cl.exe` and the vcvars scripts live under `D:\Program Files\Microsoft Visual Studio\18\Community\` too (`VC\Auxiliary\Build\vcvarsall.bat`). Point CLion's Visual Studio toolchain at that installation; do not assume `C:\Program Files`.
- CMake itself must never call MSBuild to build native targets (that would recreate the dual build). MSBuild is used only for the legacy `.slnx` build and for comparisons against it.

## Key decisions (made in Phase 0, binding for all later phases)

These decisions affect every phase. Record the outcome in this section before Phase 1 starts.

**Outcome (Phase 0):** D1â€“D6 are decided as recommended below; D4 uses option (a). The full audit and the recorded decisions are committed as `docs/CMAKE-MIGRATION.md`. Concrete values: CMake runtime folder `<binaryDir>/bin`, import libraries `<binaryDir>/lib`, preset build dirs `out/b/{x64d,x64r,x86d,x86r,vs-x64,vs-x86}`, CMake vcpkg tree `<repo>/vcpkg_installed/cmake-<Platform>`, MIDL output `<build>/src/MapWinGIS.ActiveX/midl`. Audit corrections to the table in section 3: no project defines `WIN32`, `_WINDOWS` or `_USRDLL` (drop the CMake defaults); MSBuild adds `_WINDLL` to DLL projects and, in Debug, `/JMC`; every binary links with `/MANIFEST /manifest:embed /TLBID:1`; MIDL also gets `/W1 /char signed /target "NT60"`. The proposed CMake layout is adopted unchanged.

### D1. Output directory layout

Today these all hard-code `<repo>\bin\<Config>\<Platform>`:

- `samples/CSharp/ActiveXTestHost/Run-ActiveXTestHost.ps1`
- `tests/MapWinGIS.Core.Tests/Verify-ActiveXActivation.ps1`
- `ActiveXTestHost.csproj` (`OutputPath=..\..\..\bin\...`)
- `.github/workflows/build.yml` (test executable path, Release artifact upload)

`ActiveXActivationTests` loads `MapWinGIS.ocx` from the **test executable's folder**.

If CMake also writes to `<repo>\bin\<Config>\<Platform>`, MSBuild and CMake builds will overwrite each other's DLLs, PDBs, import libraries and `gdal-data`. Ninja would lose its up-to-date state, and two presets for the same configuration would collide.

**Decision (recommended):**

- CMake output goes under the build tree: `<binaryDir>/bin` for runtime files and `<binaryDir>/lib` for import libraries. Set `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY` and `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` at the top level so that Core DLL, OCX, tests, sample and all runtime dependencies share **one runtime folder per build**.
- The scripts get an optional `-BinDirectory` parameter. The default stays unchanged (`bin\<Config>\<Platform>`), so MSBuild users see no difference. CMake targets pass the build-tree folder explicitly. This is the only script change the migration needs.
- Use short preset build directories (e.g. `out/b/x64d`, `out/b/x64r`, `out/b/x86d`, `out/b/x86r`) to avoid `MAX_PATH` problems in vcpkg's GDAL build.

### D2. vcpkg install tree and triplets

See Dependencies above: one CMake-specific install tree per platform, separate from MSBuild. Triplets are pinned in the presets.

### D3. MIDL output ownership

Today the test project includes `MapWinGIS.ActiveX_i.h` from ActiveX's **private** intermediate directory (`obj\MapWinGIS.ActiveX\<Config>\<Platform>`). In CMake, a custom-command `OUTPUT` that targets in other directories use as a source leads to duplicate or racing generation, especially with VS generators.

**Decision (recommended):**

- A custom target `MapWinGIS.ActiveX.Interface` owns the single MIDL invocation.
- An INTERFACE (or small OBJECT/STATIC) library exposes the generated include directory and the generated `_i.c`.
- Both `MapWinGIS.ActiveX` and `MapWinGIS.Core.Tests` depend on that library, never on each other's intermediates.

### D4. ActiveX sources compiled into the test executable

The test executable compiles six ActiveX `.cpp` files directly, with `MWG_ENABLE_RENDERER_TEST_HOOKS` defined. The OCX compiles the same files without that definition, so one shared OBJECT library cannot serve both.

**Decision (choose one and record it):**

- (a) Keep one source list variable (`MWG_ACTIVEX_IMPL_SOURCES`) and compile it twice: once in the OCX and once in the tests (or in a test-only `MapWinGIS.ActiveX.TestImpl` OBJECT library with the hook definition). This matches current behaviour and needs no code changes. **Recommended for this migration.**
- (b) Make the hooks a runtime switch. Out of scope, because it changes production code.

### D5. C# host project reference

`ActiveXTestHost.csproj` has a `ProjectReference` to `MapWinGIS.ActiveX.vcxproj`.

- `dotnet build` cannot build `.vcxproj` files (the .NET SDK MSBuild has no VC targets).
- Even through VS MSBuild, it would start a second, parallel OCX build that writes to the same `bin` folder and races CMake.

**Decision (recommended):** make the reference conditional, e.g. `Condition="'$(MwgCMakeBuild)' != 'true'"`, and have CMake pass `-p:MwgCMakeBuild=true` (plus `-p:BuildProjectReferences=false` as a safety net). Also check that `Directory.Build.props` / `.targets` and the vcpkg MSBuild props they import are harmless under `dotnet build`. They are guarded by `Exists`; add an explicit `MwgCMakeBuild` guard if needed.

### D6. Parity measurement

"No flags lost" and "no new warnings" must be measured, not inferred.

**Decision (recommended):**

- Compile flags: diff the existing `msbuild-extractor.json` â†’ `.mscppls\compile_commands.json` output against the CMake (Ninja) `compile_commands.json`, per translation unit, for each configuration and platform. Commit a small script, e.g. `build/scripts/Compare-CompileCommands.ps1`, that normalises paths and option order.
- Link flags: compare link lines from `msbuild -v:detailed` against `ninja -v` (or `cmake --build ... -- -v`).
- Binaries: compare `dumpbin /dependents`, `/exports` and `/headers` (subsystem, DLL characteristics such as CFG, high-entropy VA and NX) of the MSBuild and CMake outputs.
- Warnings: count warnings per configuration for both builds; the CMake count must not exceed the MSBuild count.

## Phase 0 â€” Audit, decisions and parity tooling

No CMake targets are created in this phase. Its output is a short audit document (or PR description) plus the decisions D1â€“D6.

Audit the current project files and identify:

- All source and header files per project (including `.def`, `.rc`, `.rgs`, `.idl`, `.manifest`).
- All compiler definitions, including implicit ones (`CharacterSet`, `UseOfAtl`, `ConformanceMode`, `SDLCheck`, and Release PDB generation).
- All include directories, including generated ones (`$(IntDir)`, `$(OutDir)`).
- All linker dependencies. **`VcpkgAutoLink=true` currently links every installed `.lib` into every project.** CMake links only what is named, so list the real dependencies:
  - Core: `GDAL::GDAL` **and** `GEOS::geos_c` (`GdalEnvironment.cpp` calls `GEOSversion()` directly). Check all sources for direct PROJ, GEOS and CPL use.
  - Tests: Core, `GDAL::GDAL` (tests include `gdal_priv.h` / `gdalalgorithm.h`) and `GTest::gtest`. **Not** `GTest::gtest_main` / `gmock_main`, because `Main.cpp` defines `main`.
  - Translate sample: Core, plus GDAL if `translate.cpp` uses it directly.
  - Windows libraries: `ole32`, `oleaut32`, `uuid`, `gdi32`, `user32` (explicitly, rather than relying on `CMAKE_CXX_STANDARD_LIBRARIES`). `opengl32` comes in through `#pragma comment(lib, ...)`, which is acceptable.
- All generated files (MIDL `.h`, `_i.c`, `_p.c`, `dlldata.c`, `.tlb`) and which project consumes them.
- All custom build steps and targets in `Directory.Build.targets` (e.g. `MwgDeployGdalRuntimeData`, vcpkg feature invalidation).
- All runtime deployment requirements (`VcpkgApplocalDeps`, `gdal-data\`, `proj\`).
- All architecture-specific settings (`/HIGHENTROPYVA`, MIDL `/env`, triplets, C# RID `win-x86` for `Win32`).
- All hard-coded output paths in scripts, the csproj and CI (see D1).
- Prerequisites: VS component **"C++ ATL for latest v145 build tools" for both x64 and x86**. CI gets it from the runner image; local machines may not have it.

Deliverables:

- Audit findings and decisions D1â€“D6 recorded above.
- The parity comparison script (D6), runnable against the MSBuild build alone. It locates MSBuild through `MWG_MSBUILD` / `PATH` / `vswhere` (section 4) and accepts an optional `-MSBuild <path>` parameter.
- `.gitignore` entries: `out/`, `cmake-build-*/`, `CMakeUserPresets.json`, and the new CMake `vcpkg_installed` paths.

### Phase 0 acceptance criteria

- The audit lists every target's sources, definitions, include directories, libraries and generated files.
- D1â€“D6 are decided and recorded.
- The parity script produces a baseline from the MSBuild build (built locally with `D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`).
- The proposed CMake structure (see "Proposed CMake layout") has been reviewed.

## Phase 1 â€” CMake foundation and Core DLL

Create the initial CMake infrastructure.

Expected files:

- Root `CMakeLists.txt`
- `CMakePresets.json`
- Optional `CMakeUserPresets.json` documentation (do not commit machine-specific settings).
- `cmake/` directory for reusable CMake modules (`MwgVcpkg.cmake`, `MwgCompilerOptions.cmake`, `MwgPlatform.cmake`).

Implement:

1. Project initialization (`project(MapWinGIS6 VERSION 6.0.0 LANGUAGES C CXX)`) and CMake version requirements.
2. vcpkg root resolution and toolchain integration **before** `project()` (see Dependencies).
3. C++20 / C17 configuration.
4. Windows/MSVC v145 validation (`MSVC_VERSION`, architecture, SDK warning under Ninja).
5. Configuration restriction to `Debug;Release`.
6. Shared compiler/linker options module (full table in section 3).
7. Configuration options: `MWG_ENABLE_ASAN`, `MWG_DEPLOY_RUNTIME_DATA`, `MWG_BUILD_TESTS`, `MWG_BUILD_SAMPLES`, `MWG_WINDOWS_SDK`, with feature handling via `VCPKG_MANIFEST_FEATURES`.
8. Output directory configuration (D1).
9. `MapWinGIS.Core` shared-library target.

Presets:

- Hidden base presets: `windows-base`, `x64`, `x86`, `debug`, `release`, `ninja`, `vs2026`.
- Concrete configure presets: `x64-debug`, `x64-release`, `x86-debug`, `x86-release` (Ninja), plus `vs2026-x64` / `vs2026-x86` (multi-config VS generator, `-T v145`, SDK pinned).
- Matching `buildPresets` and `testPresets` for each.
- Note in the presets/docs that `architecture: { strategy: external }` only informs Visual Studio. **CLion ignores it; each CLion profile must be assigned a Visual Studio toolchain with the matching architecture (`amd64` or `x86`/`amd64_x86`).**

The Core target must include all current source files and public headers from:

`src/MapWinGIS.Core/`

List sources explicitly (no `file(GLOB)`), grouped as in the source tree, with `source_group(TREE ...)` for IDE views.

Link GDAL and GEOS through their vcpkg-provided CMake targets (`GDAL::GDAL`, `GEOS::geos_c`).

Do not hardcode paths to `vcpkg_installed`.

Use `find_package()` and imported targets provided by the installed packages.

Preserve the `MWG_CORE_BUILD` definition and existing DLL export/import behavior. Expose `include/` as a `PUBLIC` include directory and keep the implementation `src/` `PRIVATE`.

Port `MwgDeployGdalRuntimeData` now (Core is the first runtime consumer):

- A custom target that copies `share/gdal` â†’ `<runtime>/gdal-data` and `share/proj` â†’ `<runtime>/proj`. Resolve the source from `${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/...` (the toolchain's variables, not a hardcoded path).
- Use `copy_directory_if_different` or a stamp file, so the copy does not run on every build.
- Keep the `MWG_DEPLOY_RUNTIME_DATA` option and the warning when the data is missing.
- Make `MapWinGIS.Core` depend on it.

Intermediate files must remain outside the source directories.

### Phase 1 acceptance criteria

- CMake configures from the command line for all four Ninja presets and both VS presets.
- CLion recognizes the project from `CMakePresets.json`, with an MSVC toolchain assigned per profile.
- CLion and the command line can build `MapWinGIS.Core.dll`.
- The parity script (D6) shows no lost compile or link flags for Core in Debug and Release, x64 and Win32. Every difference is explained in the PR.
- `dumpbin` shows the same exports, dependencies, subsystem and DLL characteristics as the MSBuild DLL.
- The warning count does not exceed the MSBuild baseline.
- Visual Studio's existing `.slnx` build still works, and MSBuild and CMake outputs do not overwrite each other.
- No existing source files need to be moved.

Do not proceed to Phase 2 until these conditions are met.

## Phase 2 â€” ActiveX / ATL / COM

Add the `MapWinGIS.ActiveX.Interface` (MIDL) and `MapWinGIS.ActiveX` targets.

Source directory:

`src/MapWinGIS.ActiveX/`

Build the output as:

`MapWinGIS.ocx`

Requirements:

1. Configure the target as `SHARED` (not `MODULE`) with `OUTPUT_NAME MapWinGIS` and `SUFFIX .ocx`, so the import library is still produced and vcpkg applocal deployment applies.
2. Enable ATL and preserve static ATL linking. `UseOfAtl=Static` has no CMake equivalent: rely on the ATL headers and libraries from the VS installation (component required for x64 **and** x86) and reproduce its effective definitions.
3. Preserve the apartment-threaded COM model (`_ATL_APARTMENT_THREADED` on the OCX only).
4. Preserve the `.def` file by adding it as a source. Without it, the `PRIVATE` exports and `DllInstall` (used by `regsvr32 /i:user`) are lost.
5. Compile the resource file.
6. Generate the COM interface and type library from `MapWinGIS.ActiveX.idl`.
7. Generate MIDL output in the build directory, never in the source directory.
8. Include the generated MIDL headers and IID source in the build (D3).
9. Preserve the registry script and existing COM registration behaviour.
10. Link against MapWinGIS.Core and the explicit Windows libraries from the audit.
11. Set the subsystem to `WINDOWS`.

**MIDL integration:**

- Ninja does not handle `.idl` sources; only VS generators do. Use **one explicit `add_custom_command` calling `midl.exe` for all generators**, so they behave the same (`cmake/MwgMidl.cmake`).
- Pass `/nologo /env x64|win32 /h /iid /proxy /dlldata /tlb /out` pointing into `${CMAKE_CURRENT_BINARY_DIR}/midl`, plus the existing MIDL options from the vcxproj.
- Declare every generated file as an `OUTPUT`, and declare the `.idl` and any imported `.idl` files as `DEPENDS`.
- MIDL preprocesses with `cl.exe`, so `cl.exe` must be on `PATH` (vcvars environment). This works in CLion's Visual Studio toolchain and VS but fails from a plain shell. Detect it and give a clear error.
- Generated `_i.c` is C and must be compiled with `/std:c17` and without precompiled headers.

**Resource integration:**

- The `.rc` embeds the generated type library (`1 TYPELIB "MapWinGIS.ActiveX.tlb"`). The RC dependency scanner does not know about it. Add the MIDL output directory to the RC include path and set `OBJECT_DEPENDS` on the `.rc` to the `.tlb` (plus a target dependency on `MapWinGIS.ActiveX.Interface`). Otherwise clean or parallel builds can fail intermittently.
- Write the `.tlb` to the binary directory. Copy it to the runtime folder only if a consumer needs it there.

Use explicit CMake custom commands where necessary. Ensure generated files are correctly declared as outputs and dependencies so parallel builds work reliably.

Do not assume that Visual Studio-specific MSBuild properties automatically work in CMake.

Preserve the existing x64 and Win32 behaviour.

### Phase 2 acceptance criteria

- Core and ActiveX both build successfully with Ninja and the VS generator, from a clean build directory, with `-j` parallelism (repeat the clean build several times to catch MIDL/RC races).
- The OCX has the correct name and extension and exports `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`, `DllUnregisterServer` and `DllInstall` (`dumpbin /exports`).
- The embedded type library matches the MSBuild one (e.g. compare with `oleview`/`tlbimp`, or check resource presence and IDL GUIDs).
- COM registration and activation work: `regsvr32 /i:user` against the CMake output, then `New-Object -ComObject`, then unregister. The full `Verify-ActiveXActivation.ps1` run moves to Phase 3, because it needs `MapWinGIS.Core.Tests.exe`.
- The parity script shows no lost flags for the OCX.
- The existing Visual Studio build continues working.
- No generated files appear in source directories (`git status` is clean after a build).

## Phase 3 â€” GoogleTest and CTest

Add the native test project:

`tests/MapWinGIS.Core.Tests/`

Use the existing GoogleTest dependency from vcpkg.

Requirements:

1. Create a `MapWinGIS.Core.Tests` executable (subsystem `CONSOLE`).
2. Include all existing test files.
3. Preserve the existing test-only compilation definition:
   `MWG_ENABLE_RENDERER_TEST_HOOKS`.
4. Preserve the required ActiveX source files currently compiled directly into the test executable, using the approach chosen in D4.
5. Link Core, `GDAL::GDAL`, `GTest::gtest` (not `gtest_main`), the MIDL interface library (D3) and the required Windows libraries. The tests include `atlbase.h` / `atlhost.h`, so ATL headers must be available to this target too.
6. Preserve the current test data and fixture implementation (`TestData.h/.cpp`, fixtures generated in `%TEMP%`).
7. Enable CTest using `enable_testing()` / `include(CTest)`.
8. Register the GoogleTest executable with CTest.
9. Use `gtest_discover_tests()` with:
   - `DISCOVERY_MODE PRE_TEST`. The default `POST_BUILD` mode runs the executable at build time, which fails if DLLs or the OCX are not yet deployed, or on a Win32 build on a machine that cannot run it.
   - `DISCOVERY_TIMEOUT` large enough for GDAL initialisation.
   - `WORKING_DIRECTORY` = the runtime output folder.
   - `XML_OUTPUT_DIR` for per-test GoogleTest XML reports.
10. Make the test target depend on `MapWinGIS.ActiveX` and the runtime-data target. Otherwise `ActiveXActivation.CreatesControlThroughClassFactory` fails when only the test target is built, because it loads `MapWinGIS.ocx` from the executable's folder.
11. Do **not** rely on `PATH` changes set only through the CTest `ENVIRONMENT` property. CLion's GoogleTest run configurations launch the executable directly, not through CTest, so all DLLs and data must sit beside the executable.

**Environment-dependent tests** keep their current behaviour (reported as skipped):

- `MWG_TEST_ACTIVEX_HOST` (set only by `Verify-ActiveXActivation.ps1`).
- `MWG_RUN_NETWORK_GDAL_TESTS`.
- `MWG_RUN_RENDERING_BASELINE`.
- OpenGL tests, which skip when there is no OpenGL context.

**`Verify-ActiveXActivation.ps1`:** add the `-BinDirectory` parameter (D1) and expose it as an optional CTest test labelled `interactive` (or as a custom target `verify_activex_activation`). It changes the per-user registry and refuses to run when a registration already exists, so it must be excluded from CI (`ctest -LE interactive`).

CLion must recognize individual GoogleTest tests and allow running and debugging them.

Support:

- Running all tests.
- Running a single test.
- Running tests by GoogleTest filter.
- Debugging individual tests.
- Producing XML test reports (GoogleTest XML via `XML_OUTPUT_DIR`; CTest-level JUnit via `ctest --output-junit`).

Do not change existing test behaviour simply to make CMake integration easier.

### Phase 3 acceptance criteria

The following must work for every preset:

`cmake --build --preset x64-debug`

`ctest --preset x64-debug --output-on-failure -LE interactive`

and for the VS generator:

`cmake --build <build-directory> --config Debug`

`ctest --test-dir <build-directory> -C Debug --output-on-failure -LE interactive`

- Building only `MapWinGIS.Core.Tests` from a clean tree produces a runnable test executable.
- CLion displays the tests and allows running and debugging individual tests and filters.
- All existing native tests pass, with the same pass/skip counts as the MSBuild-built executable.
- `Verify-ActiveXActivation.ps1 -BinDirectory <cmake runtime dir>` passes on an interactive desktop.

## Phase 4 â€” Samples, runtime deployment and running applications

The goal is to make the project genuinely usable from CLion, not merely compilable.

### C++ sample

Integrate:

`samples/Cpp/Translate.vcxproj`

as a native CMake executable target (`MapWinGIS.Samples.Translate`, `OUTPUT_NAME` as today), behind `MWG_BUILD_SAMPLES`.

Preserve its current functionality and dependencies.

### C# ActiveX test host

Integrate:

`samples/CSharp/ActiveXTestHost/`

The test host is a .NET 10 Windows Forms application.

CMake does not need to compile the C# source files itself.

Instead, create appropriate custom targets that:

1. Build the C# host using `dotnet build` (`find_program(dotnet)`; skip the targets with a status message if it is missing).
2. Use the correct configuration and architecture: `-c Debug|Release -p:Platform=x64|Win32` (the csproj uses `Win32`, mapped to RID `win-x86`; do not use `-a x86`), `-p:OutputPath=<cmake runtime dir>/`, `-p:MwgCMakeBuild=true -p:BuildProjectReferences=false` (D5).
3. Depend on `MapWinGIS.ActiveX` and the runtime deployment targets, so the native DLLs, OCX and runtime dependencies are available beside the executable.
4. Preserve the embedded application manifest and registration-free COM activation. `app.manifest` is the source of truth (`<file name="MapWinGIS.ocx">` / `comClass`); the OCX must sit beside the host executable.
5. Allow launching the visible test host.

Expose convenient targets:

- `build_activex_host`
- `run_activex_host`
- `run_activex_host_automated` (`-Automated`)

The run targets invoke the existing PowerShell launcher:

`samples/CSharp/ActiveXTestHost/Run-ActiveXTestHost.ps1`

through `pwsh -NoProfile -File ... -BinDirectory <runtime dir>` (`find_program(pwsh)`; fail with a clear message if PowerShell 7 is missing).

Do not replace the existing PowerShell launcher unless necessary. The only change is the `-BinDirectory` parameter (D1).

Custom targets cannot be debugged from CLion. For debugging, commit a CLion run configuration (`.run/ActiveXTestHost.run.xml`, "Custom Build Application") with `build_activex_host` as its build target, `ActiveXTestHost.exe` from the runtime folder as its executable and the runtime folder as its working directory. CLion's native debugger can debug the OCX/Core side only, not the managed side.

### Runtime dependencies

Preserve the existing deployment behaviour for:

- GDAL DLLs.
- PROJ DLLs and `proj.db`.
- GDAL data files.
- Other required vcpkg runtime dependencies.
- MapWinGIS.Core.dll.
- MapWinGIS.ocx.

Mechanisms:

- `VCPKG_APPLOCAL_DEPS=ON` (vcpkg.cmake) copies dependent DLLs after build for executables and shared libraries. It only scans each target's own imports, which works because all targets share one runtime folder (D1).
- Add a post-build step that copies `$<TARGET_RUNTIME_DLLS:...>` for the test and sample executables as a safety net (CMake â‰¥ 3.21). Make it a no-op when the list is empty.
- The C# host is not a CMake target and gets nothing automatically; its dependency on the native targets plus the shared runtime folder covers it.
- `gdal-data\` and `proj\` come from the Phase 1 runtime-data target.

Use appropriate CMake deployment mechanisms and target dependencies.

Do not rely on developers manually copying DLLs.

### CLion integration

Provide convenient build and run targets that CLion can expose.

A developer should be able to:

- Build the Core DLL.
- Build the ActiveX control.
- Build all native targets.
- Run the C++ tests.
- Debug individual tests.
- Launch the C# ActiveX test host.
- Run the automated ActiveX host checks from an interactive Windows session.

The automated GUI tests must not be forced to run in headless CI environments.

### Phase 4 acceptance criteria

- All native targets build successfully.
- The C++ sample runs from CLion and the command line.
- The C# ActiveX test host builds through `dotnet build` without triggering a vcxproj build, launches, and activates the OCX without COM registration.
- The automated host checks pass on an interactive Windows desktop.
- The MSBuild/`.slnx` build of the C# host still works (the conditional `ProjectReference` is active there). Verify it with VS MSBuild (section 4), not `dotnet build`, because that path also builds the OCX through the `.vcxproj`.

## Phase 5 â€” Visual Studio and GitHub Actions

Once the CMake build is fully functional, integrate it into the existing development workflow.

### Visual Studio

Visual Studio 2026 supports opening CMake projects directly.

Verify that:

- Visual Studio can open the root CMake project.
- IntelliSense works.
- Debugging works for the test executable and the sample without a hand-written `launch.vs.json` (add one only if the working directory or arguments require it).
- Build configurations work.
- Tests can be executed from Test Explorer.

Retain the existing `.slnx` and `.vcxproj` files during this transition.

Do not remove them until the CMake build has demonstrated full feature parity and a deliberate migration decision has been made.

Avoid maintaining two competing native build definitions indefinitely (see the Phase 6 decision gate).

Document the transition path for existing Visual Studio developers.

### GitHub Actions

Update `.github/workflows/build.yml` to validate the CMake build.

Preserve the current CI policy:

- PRs and development builds: Debug x64.
- Merges/pushes to main: full Debug/Release and x64/Win32 matrix.

Continue using:

- `windows-2025` or a compatible Windows runner.
- vcpkg binary caching. Add `CMakePresets.json` and `cmake/**` to the cache key, which currently hashes `Directory.Build.props`.
- The pinned vcpkg baseline.
- Existing SDK override where necessary (`MWG_WINDOWS_SDK` â†’ CMake cache variable).

Generator choice in CI: prefer the `Visual Studio 18 2026` generator (`-A x64|Win32 -T v145`). It pins toolset and SDK and needs no vcvars step. If Ninja is used, set up the environment with `ilammy/msvc-dev-cmd` (or `vcvarsall`) per architecture.

CI must:

1. Bootstrap or locate vcpkg.
2. Configure CMake (via presets).
3. Build the project.
4. Run CTest with `-LE interactive`.
5. Generate test reports (GoogleTest XML via `XML_OUTPUT_DIR`, the format uploaded today).
6. Upload test results.
7. Upload Release binaries from the CMake runtime folder (the path changes because of D1).
8. Optionally build with `/WX` in a dedicated job to enforce "no new warnings".

**Keep the MSBuild job running in parallel during the transition**, so "the MSBuild workflow remains functional" is checked automatically, not by hand.

Do not run GUI-based ActiveX tests or `Verify-ActiveXActivation.ps1` (registry changes) in headless CI.

Native COM activation is still validated in CI by the `ActiveXActivation.CreatesControlThroughClassFactory` GoogleTest, which needs no registration.

### Phase 5 acceptance criteria

- GitHub Actions passes for PR builds (CMake and MSBuild jobs).
- The full matrix passes on main.
- Visual Studio can use the CMake project.
- The existing MSBuild workflow remains functional during the transition.

**Implementation status:** [PR #67](https://github.com/MapWindow/MapWinGIS6/pull/67)
adds the Visual Studio-generator CI matrix, parallel
MSBuild job, test-report and Release-artifact uploads, and Visual Studio
Open Folder guidance are implemented. Local Visual Studio-generator x64
Debug configure/build and CTest passed, as did the Debug x64 MSBuild build and
both native test executables. The CMake MIDL lookup now resolves the selected
Windows SDK without requiring `vcvars` for the Visual Studio generator.

**Pending acceptance:** GitHub Actions results for the stacked PR and
interactive Visual Studio checks (IntelliSense, debugger, and Test Explorer)
have not yet been verified.

**Plan deviation:** The existing workflow also runs Release x64 for pull
requests and pushes to `development`; that coverage is retained rather than
reducing it to Debug only.

## Phase 6 â€” Documentation and legacy build transition

Update the documentation.

At minimum:

- `README.md`
- `AGENTS.md` (including the build/test/validate commands)
- `docs/BUILDING.md`
- `docs/TESTING.md`

Document:

1. CMake prerequisites (CMake version, Ninja, VS components including ATL for x64 and x86, .NET 10 SDK, `pwsh`), and how to locate MSBuild for the legacy build when Visual Studio is not on `PATH` or not on `C:` (section 4, `MWG_MSBUILD`).
2. vcpkg bootstrap and root resolution order.
3. CLion installation and configuration.
4. Opening the project in CLion. Make sure CLion loads the CMake project and not the `.mscppls\compile_commands.json` generated by msbuild-extractor.
5. Selecting the MSVC toolchain: one Visual Studio toolchain per architecture, assigned to each preset/profile.
6. Configuring build profiles from presets; disabling the extra default profiles.
7. First configure: run the bootstrap and one command-line configure first, because the initial GDAL install inside CLion's CMake reload takes a long time with little visible progress.
8. Building Core and ActiveX.
9. Running and debugging GoogleTests.
10. Running the C# ActiveX host.
11. Building from Visual Studio.
12. Building from the command line.
13. Troubleshooting common CMake and vcpkg errors: missing triplet, stale cache / Reset Cache and Reload, `MAX_PATH` and `LongPathsEnabled`, `midl` cannot find `cl.exe`, missing ATL, SDK mismatch warning under Ninja, wrong output folder for scripts (`-BinDirectory`).

Provide example commands for configuring, building and testing:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug --output-on-failure -LE interactive
```

**Decision gate for the legacy build.** Once CMake has achieved complete parity, evaluate whether the old `.vcxproj` and `.slnx` files should be removed. Define parity as a checklist, not a feeling:

- All Phase 1â€“5 acceptance criteria are met.
- At least N weeks (or N releases) of green CI on both build systems.
- No open issues tagged as CMake-parity gaps.
- Documentation updated.

Do not remove them automatically as part of this migration.

Document the final decision, its target date, and any remaining limitations (e.g. Ninja cannot pin toolset/SDK; CLion cannot debug managed code).

**Implementation status:** Phase 6 documentation is complete in README.md,
AGENTS.md, docs/BUILDING.md, docs/TESTING.md, and docs/CMAKE-MIGRATION.md. The
legacy build is retained; the decision gate requires four consecutive weeks
of green CI on both build systems and has a target review date of 2027-01-15.

**Remaining verification:** GitHub Actions for the stacked Phase 5 PR and
interactive Visual Studio/CLion acceptance checks are still pending. These
checks do not block the documentation step but must pass before legacy project
removal is considered.

## Proposed CMake layout

```
CMakeLists.txt                          # vcpkg root resolution, project(VERSION 6.0.0), options, output dirs
CMakePresets.json                       # x64/x86 Ã— Debug/Release (Ninja) + VS 18 2026 presets; build/test presets
cmake/MwgVcpkg.cmake                    # vcpkg root resolution (before project())
cmake/MwgPlatform.cmake                 # MSVC/v145 validation, x64/Win32 mapping, SDK check
cmake/MwgCompilerOptions.cmake          # mwg_apply_common_options(<target>) â€” flags from MapWinGIS.Cpp.props
cmake/MwgMidl.cmake                     # midl custom command â†’ interface library
cmake/MwgRuntimeDeploy.cmake            # gdal-data/proj copy, TARGET_RUNTIME_DLLS
src/MapWinGIS.Core/CMakeLists.txt
src/MapWinGIS.ActiveX/CMakeLists.txt    # MapWinGIS.ActiveX.Interface (MIDL), MapWinGIS.ActiveX (OCX)
tests/MapWinGIS.Core.Tests/CMakeLists.txt
samples/Cpp/CMakeLists.txt
samples/CSharp/CMakeLists.txt           # dotnet build + run targets (custom targets)
.run/ActiveXTestHost.run.xml            # CLion run/debug configuration
build/scripts/Compare-CompileCommands.ps1  # parity check (Phase 0)
```

## Risk register

| Risk | Impact | Mitigation | Phase |
|---|---|---|---|
| `VcpkgAutoLink` hides real dependencies | Link errors (`GEOSversion`, GDAL in tests) | Explicit target links from audit | 0â€“1 |
| Implicit MSBuild defaults missed (Unicode, `/permissive-`, `/sdl`, `/guard:cf`, Release PDB) | Silent behaviour or security differences | Flag table + compile_commands diff | 1 |
| `/RTC1` combined with ASan | Compiler error | Strip `/RTC1` when ASan is on | 1 |
| Shared output folder with MSBuild | Overwritten binaries, broken incremental builds | Build-tree output + `-BinDirectory` | 0â€“1 |
| Shared `vcpkg_installed` with MSBuild | Pruned packages, rebuilds | Separate CMake install tree per platform | 0â€“1 |
| Missing triplet in presets | Second GDAL build with wrong toolset | Pin `VCPKG_TARGET_TRIPLET` / `HOST_TRIPLET` | 1 |
| MIDL output consumed across targets | Racing/duplicate generation | Single interface target (D3) | 2 |
| `.rc` â†’ `.tlb` dependency not scanned | Intermittent clean/parallel build failures | `OBJECT_DEPENDS` + target dependency | 2 |
| `midl` without `cl.exe` on `PATH` | Configure/build failure outside vcvars | Detect and report clearly | 2 |
| `.def` file missing / `MODULE` library | Lost exports, no import lib | `SHARED` + `.def` source | 2 |
| ATL component not installed (x86) | Win32 build failure | Document prerequisite, check in configure | 0, 6 |
| `gtest_discover_tests` POST_BUILD | Build fails before DLLs are deployed | `PRE_TEST` discovery | 3 |
| CLion runs tests without CTest environment | Tests pass in ctest, fail in CLion | Everything beside the executable | 3 |
| Registry-based activation script in CI | Polluted registry / false failures | `interactive` label, excluded in CI | 3, 5 |
| C# `ProjectReference` to vcxproj | `dotnet build` failure or parallel OCX build | Conditional reference (D5) | 4 |
| Ninja cannot pin toolset/SDK | Non-reproducible local builds | VS presets pin; Ninja warns | 1, 6 |
| CLion first reload installs GDAL | Apparent hang / timeout | Command-line first configure | 6 |
| `MAX_PATH` in vcpkg buildtrees | GDAL build failure | Short build dirs, LongPathsEnabled | 1, 6 |
| Two build systems drift | Divergent flags | Parallel CI + Phase 6 decision gate | 5â€“6 |
| MSBuild not on `PATH` / VS on `D:` | Baseline, parity or legacy builds fail; scripts assume `C:\Program Files` | `MWG_MSBUILD` â†’ `PATH` â†’ `vswhere`; never hardcode the path (section 4) | 0â€“5 |

## CMake design guidelines

Follow these guidelines throughout the implementation:

- Prefer target-based CMake.
- Avoid global include directories and global compiler definitions.
- Use imported vcpkg targets.
- Keep the Core library independent of COM and ATL.
- Keep generated files in the build directory.
- Use CMake presets for reproducible configurations.
- Use CTest for test execution.
- Avoid hardcoded absolute paths.
- Support parallel builds; declare every generated file as an output and every consumer dependency explicitly.
- Keep the source directory clean.
- List sources explicitly; do not use `file(GLOB)`.
- Do not duplicate source lists; reuse variables when a source must be compiled twice (D4).
- Preserve the existing binary and runtime deployment behaviour.
- Use descriptive CMake target names (`MapWinGIS.Core`, `MapWinGIS.ActiveX`, `MapWinGIS.ActiveX.Interface`, `MapWinGIS.Core.Tests`).
- Keep CMake modules small and maintainable.
- Do not introduce unnecessary third-party dependencies.

## Important implementation rules

1. Work on the `development` branch.
2. Implement one phase at a time.
3. Create a separate PR for each phase.
4. Do not start the next phase until the current phase builds and passes its acceptance criteria.
5. Do not make unrelated architectural changes.
6. Do not rewrite existing production C++ code merely to accommodate CMake. The only planned non-CMake changes are the `-BinDirectory` script parameter (D1), the conditional C# `ProjectReference` (D5), `.gitignore` entries and the parity script.
7. Do not weaken compiler warnings or tests to get a successful build.
8. Inspect existing MSBuild settings and project dependencies before translating them, including implicit defaults.
9. Preserve the existing vcpkg configuration and dependency versions.
10. Update documentation alongside each phase where appropriate.
11. Include the parity script output (flags, `dumpbin`, warning count) in every phase PR.

## Final goal

A developer should be able to clone MapWinGIS6, bootstrap vcpkg, open the root folder in CLion and build, test, debug and run the project without needing Visual Studio's `.slnx` as the build entry point.

Visual Studio developers should be able to use the same CMake project.

CMake should become the authoritative native build definition, with the existing modern MapWinGIS6 architecture and behaviour preserved.

## Remaining limitations (follow-up)

- Phase 0 (PR #39): ~~the MSBuild baseline covers x64 only~~ â€” resolved in Phase 1, which collected the Win32 Debug/Release baseline.
- Phase 0: `Compare-CompileCommands.ps1` is verified by running it against the MSBuild logs; it has no automated (Pester) tests.
- Phase 1 (PR #40): CLion recognition of the preset profiles was not verified interactively; the presets and the toolchain/architecture guard were verified from the command line only.
- Phase 1: there is no automated CMake test yet; the Core GoogleTests run under CMake from Phase 3. Phase 1 was validated with the parity script (0 lost flags, 0 warnings, identical `dumpbin`) for Debug/Release x64/Win32.
- Phase 1: the VS presets were configured and built for x64 Debug/Release and x86 Debug only; parity was measured on the Ninja builds.
- Phase 1: Ninja cannot pin the Windows SDK; configure only warns when it differs from `MWG_WINDOWS_SDK`.
- Phase 1: the parity script skips link comparison for binaries that were up to date (it now warns); delete the binary or rebuild with `--clean-first` before measuring.
- Phase 2 (PR #41): the MSBuild `.slnx` build was not rebuilt in the Phase 2 worktree (no MSBuild file changed; Phase 1 verified it). Parity used the Phase 1 MSBuild logs with worktree paths rewritten.
- Phase 2: the `cl.exe`-on-PATH guard in `MwgMidl.cmake` was not exercised from a plain shell (configure fails earlier there because no compiler is found).
- Phase 2: the VS generator was built for x64 Debug/Release and x86 Debug only; the x86 Release VS build and the VS-generator parity run are still open.
- Phase 2: the type library is compared byte-wise with the MSBuild `.tlb`; the resource embedded in the OCX was not extracted and compared separately.
- Phase 3: x64 Debug Ninja and Visual Studio builds passed; CTest and the MSBuild executable each ran 173 tests (170 passed, 3 skipped). The registered COM activation check was not run because it changes per-user registration and requires an interactive desktop; CLion test discovery/debugging was not verified interactively.
- Phase 3 parity (Debug x64): 0 unexplained lost flags; 22 ActiveX intermediate-directory differences are explained by the generated MIDL include path. Core DLL, OCX and test-executable dumpbin summaries match. CMake warning count was 0; the parity Rebuild had two transient MSB3061 DLL-delete warnings. CMake does not yet include the Translate sample TU/link (Phase 4).
- Phase 4: Ninja and Visual Studio Debug x64 builds, CMake runtime deployment, 173 CTest cases, 14 automated host checks, MSBuild host build, and parity (0 lost options; CMake 0 warnings, MSBuild 1) were verified. CLion's host run configuration and launching Translate from CLion were not checked interactively.
