# Dependency and runtime details

Dependencies are declared in `vcpkg.json`; GDAL and GEOS are production
dependencies, and GoogleTest is supplied by the `tests` feature. The optional
`full-drivers` feature adds a larger set of GDAL drivers. The manifest baseline
and GDAL minimum version must be considered together when updating GDAL.

MSBuild resolves vcpkg from the `MwgVcpkgRoot` property, `MWG_VCPKG_ROOT`,
the sibling `..\vcpkg` directory, then `VCPKG_ROOT`. It installs to a
platform-specific tree because vcpkg can remove another triplet from a shared
install directory.

The build deploys GDAL and PROJ runtime data beside the output binaries.
Only the Core project's target deploys this shared data. Consumer projects
reference Core so parallel solution builds do not copy the same files at once.
`GdalEnvironment` configures those paths; do not set `GDAL_DATA` or
`PROJ_LIB` for the application.

`MwgVcpkgFeatures` is not part of vcpkg's normal manifest stamp inputs. The
repository's `Directory.Build.targets` records the installed feature set beside
the stamp and removes the stamp when requested features differ. This is needed
when switching features such as `tests` and `full-drivers`; otherwise vcpkg
could reuse an install built for another feature set.

## References

- [Dependency manifest](../../../vcpkg.json)
- [Shared vcpkg configuration](../../../Directory.Build.props)
- [Feature stamp and runtime-data deployment](../../../Directory.Build.targets)
- [Build and dependency guide](../../../docs/BUILDING.md#dependencies)
- [GDAL version requirement](../../../docs/gdal-version.md)
