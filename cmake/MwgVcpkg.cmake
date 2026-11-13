# SPDX-License-Identifier: MPL-2.0
#
# vcpkg integration. Included by the root CMakeLists.txt BEFORE project(), so
# that the toolchain file and the vcpkg variables are in place when project()
# runs vcpkg's manifest install.
#
# Values from a preset or the command line always win; this module only fills
# in what is missing.

# --- vcpkg root ---------------------------------------------------------------
# Same order as build/scripts/bootstrap-vcpkg.ps1 and Directory.Build.props:
#   1. MWG_VCPKG_ROOT (cache variable or environment variable)
#   2. <repo>/../vcpkg
#   3. VCPKG_ROOT
function(_mwg_is_vcpkg_root out_var dir)
    if(EXISTS "${dir}/.vcpkg-root" AND EXISTS "${dir}/scripts/buildsystems/vcpkg.cmake")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_mwg_resolve_vcpkg_root out_var)
    # An explicit MWG_VCPKG_ROOT that is not a vcpkg root is a mistake, not a hint.
    set(explicit "${MWG_VCPKG_ROOT}")
    if(explicit STREQUAL "" AND DEFINED ENV{MWG_VCPKG_ROOT})
        set(explicit "$ENV{MWG_VCPKG_ROOT}")
    endif()
    if(NOT explicit STREQUAL "")
        file(TO_CMAKE_PATH "${explicit}" explicit)
        _mwg_is_vcpkg_root(ok "${explicit}")
        if(NOT ok)
            message(FATAL_ERROR
                "MWG_VCPKG_ROOT points to '${explicit}', which is not a vcpkg root.\n"
                "Run 'pwsh -File build\\scripts\\bootstrap-vcpkg.ps1' or fix MWG_VCPKG_ROOT.")
        endif()
        set(${out_var} "${explicit}" PARENT_SCOPE)
        return()
    endif()

    cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH repo_parent)
    set(candidates "${repo_parent}/vcpkg")
    if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" env_root)
        list(APPEND candidates "${env_root}")
    endif()

    foreach(candidate IN LISTS candidates)
        _mwg_is_vcpkg_root(ok "${candidate}")
        if(ok)
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(FATAL_ERROR
        "No vcpkg installation found (looked at MWG_VCPKG_ROOT, ${repo_parent}/vcpkg and VCPKG_ROOT).\n"
        "Run 'pwsh -File build\\scripts\\bootstrap-vcpkg.ps1' once, or pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake.")
endfunction()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    _mwg_resolve_vcpkg_root(_mwg_vcpkg_root)
    set(CMAKE_TOOLCHAIN_FILE "${_mwg_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file, resolved from MWG_VCPKG_ROOT, ../vcpkg or VCPKG_ROOT")
    message(STATUS "vcpkg root: ${_mwg_vcpkg_root}")
endif()

# --- triplets -----------------------------------------------------------------
# The presets pin the triplets. Without a preset, derive the target triplet from
# the generator platform (Visual Studio) or the vcvars environment (Ninja).
# Without an explicit triplet vcpkg silently picks x64-windows and builds GDAL a
# second time with a different toolset. MwgPlatform.cmake checks after
# project() that the triplet matches the compiler.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "^(Win32|x86)" OR "$ENV{VSCMD_ARG_TGT_ARCH}" STREQUAL "x86")
        set(_mwg_default_triplet "x86-windows-v145")
    else()
        set(_mwg_default_triplet "x64-windows-v145")
    endif()
    set(VCPKG_TARGET_TRIPLET "${_mwg_default_triplet}" CACHE STRING "vcpkg target triplet")
endif()

if(NOT DEFINED VCPKG_HOST_TRIPLET)
    set(VCPKG_HOST_TRIPLET "x64-windows-v145" CACHE STRING "vcpkg host triplet")
endif()

# --- install tree -------------------------------------------------------------
# One CMake install tree per platform, shared by Debug and Release and separate
# from the MSBuild tree (vcpkg_installed/<Platform>), see docs/CMAKE-MIGRATION.md (D2).
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    if(VCPKG_TARGET_TRIPLET MATCHES "^x86-")
        set(_mwg_vcpkg_platform "Win32")
    else()
        set(_mwg_vcpkg_platform "x64")
    endif()
    set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/cmake-${_mwg_vcpkg_platform}"
        CACHE PATH "vcpkg install tree used by the CMake build")
endif()

# --- manifest features --------------------------------------------------------
# vcpkg.cmake runs 'vcpkg install' on every configure with these features, so a
# change takes effect on the next reconfigure (CLion: Reset Cache and Reload
# when a cached value is stale).
if(NOT DEFINED VCPKG_MANIFEST_FEATURES)
    set(_mwg_features "")
    if(MWG_BUILD_TESTS)
        list(APPEND _mwg_features "tests")
    endif()
    set(VCPKG_MANIFEST_FEATURES "${_mwg_features}" CACHE STRING "vcpkg manifest features (tests, full-drivers)")
elseif(MWG_BUILD_TESTS AND NOT "tests" IN_LIST VCPKG_MANIFEST_FEATURES)
    message(STATUS "MWG_BUILD_TESTS is ON: adding the 'tests' vcpkg manifest feature")
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
