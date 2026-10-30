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
