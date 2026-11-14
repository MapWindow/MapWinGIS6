# GDAL version

MapWinGIS6 requires GDAL **3.12.4 or later**. The current vcpkg baseline in
`vcpkg.json` resolves GDAL 3.12.4, and the manifest version constraint prevents
selecting an older port. Update both the baseline and the version constraint
together when upgrading GDAL.

`MapWinGIS.Core.dll` uses the GDAL dependency supplied by the repository's
vcpkg manifest. GDAL headers and libraries are not vendored. Runtime data files
are deployed beside the core DLL by the existing build target.

GDAL initialization is process-wide and intentionally has no matching global
shutdown: destroying the driver manager while datasets or other GDAL users are
still alive would invalidate their resources. `GdalInitializer` therefore
registers drivers once and leaves the manager alive until process exit.

The Core module is pinned with `GET_MODULE_HANDLE_EX_FLAG_PIN` before its GDAL
error callback is registered. GDAL retains that callback process-wide, so the
module remains loaded until process termination. The callback's logger state
is also deliberately allocated for process lifetime to avoid static
destruction-order hazards. There is no unload/shutdown API; do not attempt to
unload Core manually while the process is running.

At initialization, `GDALCheckVersion` verifies that the loaded runtime is
compatible with the compile-time major/minor version. The algorithm API also
has a compile-time minimum of GDAL 3.12.4. Applications must deploy a runtime
with the same compatible GDAL ABI as the vcpkg headers and libraries used to
build Core.
