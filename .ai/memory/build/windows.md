# Windows build environment

The repository's supported native configurations are `Debug` and `Release`
for `x64` and `Win32`. The solution is an MSBuild `.slnx` solution and the
project toolset is MSVC v145. The default Windows SDK is 10.0.28000.0; it can
be overridden with `MwgWindowsSdk`.

The ActiveX project statically links ATL. MFC is not required. The C# ActiveX
host requires the .NET 10 SDK and an interactive Windows desktop for its
automated UI and pixel checks.

## CI scope

`.github/workflows/build.yml` runs Debug and full-driver Release x64 builds
and native tests for pull requests and pushes to `development`.
Each job has a 180-minute timeout. Pushes to `main` additionally build and test
both configurations for `Win32`. The runner overrides the default SDK with `10.0`
because the `windows-2025` image does not include the repository's pinned SDK.
The C# host checks require an interactive desktop and are not part of this CI
workflow.

## References

- [Build requirements and commands](../../../docs/BUILDING.md#prerequisites)
- [CI workflow](../../../.github/workflows/build.yml)
- [Shared toolset and SDK configuration](../../../Directory.Build.props)
- [ActiveX project](../../../src/MapWinGIS.ActiveX/MapWinGIS.ActiveX.vcxproj)
