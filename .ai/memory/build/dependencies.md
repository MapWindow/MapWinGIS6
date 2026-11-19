# Dependency and runtime details

Dependencies are declared in `vcpkg.json`; GDAL and GEOS are production
dependencies, and GoogleTest is supplied by the `tests` feature. The optional
`full-drivers` feature adds a larger set of GDAL drivers. The manifest baseline
and GDAL minimum version must be considered together when updating GDAL.

GDAL 3.12 renamed the OGR geometry accessors to snake_case (`get_Area()`,
`get_Length()`); the old camelCase names such as `getArea()` are removed.
`get_Area()` is not on the `OGRGeometry` base at all (only on `OGRCurve` and
`OGRSurface`), and a disjoint polygon `Intersection()` returns `POLYGON EMPTY`,
which still reports dimension 2 — check `IsEmpty()` before using `getDimension()`
or a surface cast on intersection results.

MSBuild resolves vcpkg from the `MwgVcpkgRoot` property, `MWG_VCPKG_ROOT`,
the sibling `..\vcpkg` directory, then `VCPKG_ROOT`. It installs to a
platform-specific tree because vcpkg can remove another triplet from a shared
install directory.

In a git worktree under `.worktrees\<name>`, the sibling `..\vcpkg` resolves
inside `.worktrees`, so set `MWG_VCPKG_ROOT` to the real vcpkg root before
configuring. On the primary development machine `VCPKG_ROOT` points to an
unrelated vcpkg checkout and must not be used.

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

GitHub Actions vcpkg cache keys must also distinguish the requested feature set.
On PR #80, Debug and Release initially shared a key; the Debug job saved its
smaller package cache first, and the Release job could not replace it with the
`full-drivers` packages. `main` is the default branch and cannot restore
`development`-scoped caches, so it must save its own.

An exact cache-key hit does not mean vcpkg reuses the packages. Package ABIs
include vcpkg's own scripts, and an unpinned vcpkg clone (vcpkg master moved
past `builtin-baseline`) changed every ABI: run `43765171463` hit all caches
but rebuilt GDAL and its dependencies for 30-86 minutes per job, and the
save-on-miss-only rule never refreshed the stale caches. CI therefore runs
`bootstrap-vcpkg.ps1 -Baseline` (vcpkg at the manifest baseline, full clone
because vcpkg reads older port versions from git history; a depth-1 clone fails
to load `gdal@3.12.4`). Separate per-leg dependency jobs save caches under unique
`<prefix><run_id>-<run_attempt>` keys. The sync script prunes unused archives
only after a successful install; after a failed install it retains all
archives, including packages not reached in the install plan. Pull requests
save only when vcpkg added packages; pushes also refresh stale-prefix caches.
An archive is named by the SHA-256 of `share/<port>/vcpkg_abi_info.txt` in the
install tree (verified locally). Each consumer waits only for its matching
platform and configuration dependency job; Release jobs use `-release`
triplets.

The MSBuild parity step must not rebuild on its own: `Compare-CompileCommands.ps1`
without `-MSBuildLog` rebuilds with the default features, which in a
full-driver Release job reinstalled GDAL without `full-drivers` (+25 minutes)
and left the tests and artifacts on the reduced driver set. CI passes the build
step's detailed log instead. See
[`build.yml`](../../../.github/workflows/build.yml).

## References

- [Dependency manifest](../../../vcpkg.json)
- [Shared vcpkg configuration](../../../Directory.Build.props)
- [Feature stamp and runtime-data deployment](../../../Directory.Build.targets)
- [CI binary-cache pruning](../../../build/scripts/Sync-VcpkgBinaryCache.ps1)
- [Build and dependency guide](../../../docs/BUILDING.md#dependencies)
- [GDAL version requirement](../../../docs/gdal-version.md)
