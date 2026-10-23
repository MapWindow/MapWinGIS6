set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Pinned so that the dependencies are built with the same toolset as MapWinGIS6
# even when an older Visual Studio is also installed.
set(VCPKG_PLATFORM_TOOLSET v145)
