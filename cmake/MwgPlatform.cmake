# SPDX-License-Identifier: MPL-2.0
#
# Toolchain and platform validation, run after project().
#
# Sets:
#   MWG_PLATFORM          MSBuild platform name: x64 or Win32
#   MWG_RUNTIME_DIR       runtime output folder of this build (may contain $<CONFIG>)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "MapWinGIS6 builds on Windows only.")
endif()

foreach(_mwg_lang IN ITEMS C CXX)
    if(NOT CMAKE_${_mwg_lang}_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR
            "MapWinGIS6 needs the MSVC compiler (cl.exe); the ${_mwg_lang} compiler is "
            "'${CMAKE_${_mwg_lang}_COMPILER_ID}' (${CMAKE_${_mwg_lang}_COMPILER}).\n"
            "In CLion select a Visual Studio toolchain; from a shell, configure inside a "
            "VS 2026 Developer PowerShell or vcvarsall.bat environment.")
    endif()
endforeach()
unset(_mwg_lang)

# v145 is MSVC 19.50 and later. The toolset name is not available under Ninja,
# so the compiler version is the check that works for every generator.
if(MSVC_VERSION LESS 1950)
    message(FATAL_ERROR
        "MapWinGIS6 needs the MSVC v145 toolset (Visual Studio 2026, MSVC 19.50 or later); "
        "found MSVC ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}).")
endif()
if(CMAKE_GENERATOR_TOOLSET AND NOT CMAKE_GENERATOR_TOOLSET MATCHES "^v145")
    message(FATAL_ERROR "MapWinGIS6 needs the v145 toolset; the generator uses '${CMAKE_GENERATOR_TOOLSET}'.")
endif()

# --- architecture -------------------------------------------------------------
# <Platform> is an MSBuild concept: map the CMake architecture to x64 / Win32.
if(CMAKE_VS_PLATFORM_NAME)
    set(MWG_PLATFORM "${CMAKE_VS_PLATFORM_NAME}")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MWG_PLATFORM "x64")
else()
    set(MWG_PLATFORM "Win32")
endif()

if(MWG_PLATFORM STREQUAL "x64" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_mwg_expected_triplet "x64-windows-v145")
elseif(MWG_PLATFORM STREQUAL "Win32" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_mwg_expected_triplet "x86-windows-v145")
else()
    message(FATAL_ERROR "Unsupported target architecture '${MWG_PLATFORM}' (pointer size ${CMAKE_SIZEOF_VOID_P}).")
endif()

# A mismatch means the CLion toolchain or vcvars environment targets another
# architecture than the preset; linking would fail much later with LNK1112.
if(NOT VCPKG_TARGET_TRIPLET STREQUAL _mwg_expected_triplet)
    message(FATAL_ERROR
        "VCPKG_TARGET_TRIPLET is '${VCPKG_TARGET_TRIPLET}', but the compiler targets ${MWG_PLATFORM} "
        "(expected '${_mwg_expected_triplet}').\n"
        "In CLion, assign this profile a Visual Studio toolchain with the matching architecture "
        "(amd64 for x64 presets, x86 or amd64_x86 for x86 presets), then Reset Cache and Reload.")
endif()
unset(_mwg_expected_triplet)

# --- Windows SDK --------------------------------------------------------------
# Visual Studio generators pin the SDK through CMAKE_SYSTEM_VERSION (set from
# MWG_WINDOWS_SDK before project()). Ninja takes the SDK from the vcvars
# environment and cannot pin it, so a mismatch is only reported.
if(NOT MWG_WINDOWS_SDK STREQUAL "10.0")
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION STREQUAL MWG_WINDOWS_SDK)
            message(FATAL_ERROR
                "Windows SDK ${MWG_WINDOWS_SDK} is not installed (Visual Studio selected "
                "'${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}'). Install it, or configure with "
                "-DMWG_WINDOWS_SDK=10.0 to use the newest installed SDK.")
        endif()
    else()
        set(_mwg_sdk "$ENV{WindowsSDKVersion}")
        string(REGEX REPLACE "[\\/]+$" "" _mwg_sdk "${_mwg_sdk}")
        if(_mwg_sdk STREQUAL "" AND CMAKE_RC_COMPILER MATCHES "Windows Kits/10/bin/([0-9.]+)/")
            set(_mwg_sdk "${CMAKE_MATCH_1}")
        endif()
        if(NOT _mwg_sdk STREQUAL MWG_WINDOWS_SDK)
            message(WARNING
                "The build environment uses Windows SDK '${_mwg_sdk}', not ${MWG_WINDOWS_SDK}. "
                "Ninja cannot pin the SDK; it comes from vcvarsall.bat. Set the Version field of the "
                "CLion Visual Studio toolchain to ${MWG_WINDOWS_SDK} (equivalent to "
                "'vcvarsall.bat <arch> ${MWG_WINDOWS_SDK}'), or set -DMWG_WINDOWS_SDK=10.0 to accept the newest SDK.")
        endif()
        unset(_mwg_sdk)
    endif()
endif()

# --- runtime folder -----------------------------------------------------------
get_property(_mwg_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_mwg_multi_config)
    set(MWG_RUNTIME_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
    set(MWG_RUNTIME_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
unset(_mwg_multi_config)

message(STATUS "MapWinGIS6: ${MWG_PLATFORM}, MSVC ${CMAKE_CXX_COMPILER_VERSION}, triplet ${VCPKG_TARGET_TRIPLET}")
