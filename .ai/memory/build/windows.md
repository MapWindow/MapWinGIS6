# Windows build environment

The repository's supported native configurations are `Debug` and `Release`
for `x64` and `Win32`. The solution is an MSBuild `.slnx` solution and the
project toolset is MSVC v145. The default Windows SDK is 10.0.28000.0; it can
be overridden with `MwgWindowsSdk`.

The ActiveX project statically links ATL. MFC is not required. The C# ActiveX
host requires the .NET 10 SDK and an interactive Windows desktop for its
automated UI and pixel checks.

## CI scope

`.github/workflows/build.yml` runs both CMake and MSBuild jobs. Pull requests
and pushes to `development` build and test Debug and Release x64; pushes to
`main` additionally build both configurations for Win32. The CMake job uses
the Visual Studio 18 2026 generator, while a separate Ninja job checks compile
flags and warnings against the MSBuild baseline. CTest excludes tests labelled
`interactive`; the C# host checks require a desktop and are not part of CI.
The runner overrides the default SDK with `10.0` because `windows-2025` does
not include the repository's pinned SDK. Each build job has a 180-minute
timeout.

## References

- [Build requirements and commands](../../../docs/BUILDING.md#prerequisites)
- [CI workflow](../../../.github/workflows/build.yml)
- [CMake presets](../../../CMakePresets.json)
- [Shared toolset and SDK configuration](../../../Directory.Build.props)
- [ActiveX project](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.vcxproj)
