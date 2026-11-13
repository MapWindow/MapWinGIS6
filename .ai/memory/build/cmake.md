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

The `cmake-parity` job fails (`-FailOnLostFlags`) when a translation unit is
compiled by MSBuild but missing from the CMake target — including test
projects. Adding a source file to a `.vcxproj` therefore requires the same
addition to the corresponding `CMakeLists.txt` source list (verified on
PR #71, where `PolygonTriangulationTests.cpp` was listed in
`MapWinGIS.Core.Tests.vcxproj` but not in its CMake target). The parity
report (`out/parity/parity-Debug-x64.json` on the runner) names the lost
TU; the job log only shows the lost count.

## References

- [CMake presets](../../../CMakePresets.json)
- [Root CMake project](../../../CMakeLists.txt)
- [CMake and MSBuild CI jobs](../../../.github/workflows/build.yml)
- [Shared MSBuild properties](../../../Directory.Build.props)
- [Build configuration checks](../../../build/scripts/Test-BuildConfiguration.ps1)
- [Build guide](../../../docs/BUILDING.md#building-with-cmake)
- [Parity comparison](../../../build/scripts/Compare-CompileCommands.ps1)
