# CMake build

The root `CMakeLists.txt` defines the standalone native build, with x64 and
Win32 Debug/Release presets in `CMakePresets.json`. The presets support Ninja
and the Visual Studio 18 2026 generator; CMake output remains under `out/b`,
separate from MSBuild's `bin` output.

CI keeps the legacy MSBuild workflow and runs the CMake build in parallel.
`cmake-parity` retains the Ninja-based compiler-flag and warning comparison;
the `cmake` job builds and tests the Visual Studio generator presets.
For plain-shell Visual Studio-generator configure, MIDL is found under the
registered Windows Kits root using the SDK version selected by CMake.
Generated Visual Studio projects set `WindowsTargetPlatformVersion` before
importing `Directory.Build.props`; its legacy SDK default must not override
that selection, including in CMake's `VCTargetsPath` probe.

On the primary development machine `cmake` is not on PATH; it ships with
Visual Studio 18 Community under `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
The `x64-debug`/`x64-release` Ninja presets also require `cl.exe` on PATH
(a Developer prompt). From a plain shell, use the `vs2026-x64` configure
preset and the `vs2026-x64-debug`/`vs2026-x64-release` build and test presets;
the Visual Studio generator locates the v145 toolset itself.

The `cmake-parity` job fails (`-FailOnLostFlags`) when a translation unit is
compiled by MSBuild but missing from the CMake target — including test
projects. Adding a source file to a `.vcxproj` therefore requires the same
addition to the corresponding `CMakeLists.txt` source list (verified on
PR #71, where `PolygonTriangulationTests.cpp` was listed in
`MapWinGIS.Core.Tests.vcxproj` but not in its CMake target). The parity
report (`out/parity/parity-Debug-x64.json` on the runner) names the lost
TU; the job log only shows the lost count.

`MapWinGIS.RuntimeData` tracks every deployed GDAL and PROJ file as a build
output. Its sync manifest lets the build repair missing files and prune files
removed from vcpkg while preserving unrelated files in the runtime directory.
See `MwgRuntimeDeploy.cmake`, `MwgSyncRuntimeData.cmake`, and
`Test-CMakeRuntimeDataRepair.ps1`.

## References

- [CMake presets](../../../CMakePresets.json)
- [Root CMake project](../../../CMakeLists.txt)
- [CMake and MSBuild CI jobs](../../../.github/workflows/build.yml)
- [Runtime data target](../../../cmake/MwgRuntimeDeploy.cmake)
- [Runtime data sync](../../../cmake/MwgSyncRuntimeData.cmake)
- [Runtime data repair check](../../../build/scripts/Test-CMakeRuntimeDataRepair.ps1)
- [Shared MSBuild properties](../../../Directory.Build.props)
- [Build configuration checks](../../../build/scripts/Test-BuildConfiguration.ps1)
- [Build guide](../../../docs/BUILDING.md#building-with-cmake)
- [Parity comparison](../../../build/scripts/Compare-CompileCommands.ps1)
