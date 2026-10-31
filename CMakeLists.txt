# SPDX-License-Identifier: MPL-2.0
#
# MapWinGIS6 native build. Windows and MSVC v145 only.
#
#   pwsh -File build\scripts\bootstrap-vcpkg.ps1
#   cmake --preset x64-debug
#   cmake --build --preset x64-debug
#
# See docs/BUILDING.md and docs/CMAKE-MIGRATION.md.

# 3.15: CMP0092 (no default /W3), 3.21: TARGET_RUNTIME_DLLS,
# 3.26: copy_directory_if_different, 3.28: tested minimum.
cmake_minimum_required(VERSION 3.28...4.3)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- options needed before project() -----------------------------------------
option(MWG_BUILD_TESTS "Build the GoogleTest suite (adds the 'tests' vcpkg feature)" ON)
option(MWG_BUILD_SAMPLES "Build the samples" ON)
set(MWG_WINDOWS_SDK "10.0.28000.0" CACHE STRING
    "Windows SDK version (MSBuild: MwgWindowsSdk). 10.0 means the newest installed SDK")

# --- configurations ----------------------------------------------------------
# Debug and Release only, as in MSBuild. CLion offers RelWithDebInfo and
# MinSizeRel profiles by default; they are rejected instead of being built with
# flags that no other build uses.
if(CMAKE_GENERATOR MATCHES "^Visual Studio|Multi-Config$")
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Debug or Release" FORCE)
    endif()
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR
            "CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}' is not supported; MapWinGIS6 builds Debug and Release only.\n"
            "Use one of the presets in CMakePresets.json (CLion: disable the default profiles and enable the preset profiles).")
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# --- toolchain ---------------------------------------------------------------
include(MwgVcpkg)

# Visual Studio generators select the Windows SDK from CMAKE_SYSTEM_VERSION;
# "10.0" lets them pick the newest one. Ninja uses the vcvars environment.
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOT DEFINED CMAKE_SYSTEM_VERSION)
    set(CMAKE_SYSTEM_VERSION "${MWG_WINDOWS_SDK}" CACHE STRING "Windows SDK selected by the Visual Studio generator")
endif()

set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MwgFlagOverrides.cmake")

project(MapWinGIS6
    VERSION 6.0.0
    DESCRIPTION "Open-source mapping engine behind MapWindow"
    HOMEPAGE_URL "https://github.com/MapWindow/MapWinGIS6"
    LANGUAGES C CXX)

# --- output layout (docs/CMAKE-MIGRATION.md, D1) -----------------------------
# One runtime folder per build tree, separate from MSBuild's bin\<Config>\<Platform>.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

include(MwgPlatform)
include(MwgCompilerOptions)
include(MwgRuntimeDeploy)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

mwg_add_runtime_data_target()

add_subdirectory(src/MapWinGIS.Core)
