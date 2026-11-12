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

## References

- [CMake presets](../../../CMakePresets.json)
- [Root CMake project](../../../CMakeLists.txt)
- [CMake and MSBuild CI jobs](../../../.github/workflows/build.yml)
- [Build guide](../../../docs/BUILDING.md#building-with-cmake)
- [Parity comparison](../../../build/scripts/Compare-CompileCommands.ps1)
