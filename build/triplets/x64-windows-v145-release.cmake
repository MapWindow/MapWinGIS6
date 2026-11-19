set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Pinned so that the dependencies are built with the same toolset as MapWinGIS6
# even when an older Visual Studio is also installed.
set(VCPKG_PLATFORM_TOOLSET v145)

# Release-only variant of x64-windows-v145 for Release-only builds such as CI.
# Skipping the debug libraries roughly halves the dependency build time; Debug
# builds cannot use this triplet.
set(VCPKG_BUILD_TYPE release)
