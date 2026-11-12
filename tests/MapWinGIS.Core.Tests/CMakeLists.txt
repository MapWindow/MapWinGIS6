# SPDX-License-Identifier: MPL-2.0
#
# Native GoogleTest suite for the Core API and ActiveX rendering components.

find_package(GDAL CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# vcpkg marks GoogleTest headers as system, conflicting with /external:W4.
set_property(TARGET GTest::gtest PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

set(MWG_CORE_TEST_HEADERS
    TestData.h
)

set(MWG_CORE_TEST_SOURCES
    ActiveXActivationTests.cpp
    ComConversionTests.cpp
    GdalAlgorithmRunnerTests.cpp
    GdalEnvironmentTests.cpp
    GdalModernCapabilitiesTests.cpp
    GdalOperationDispatcherTests.cpp
    GdalRuntimeTests.cpp
    GdalTypedOptionsTests.cpp
    GdalUtilsTests.cpp
    Main.cpp
    RenderingBaselineTests.cpp
    TestData.cpp
    VectorDatasetTests.cpp
    VectorLayerRendererTests.cpp
    VectorLayerTests.cpp
    VectorRenderSceneTests.cpp
)

add_executable(MapWinGIS.Core.Tests)
target_sources(MapWinGIS.Core.Tests PRIVATE
    ${MWG_CORE_TEST_HEADERS}
    ${MWG_CORE_TEST_SOURCES}
    ${MWG_ACTIVEX_SHARED_SOURCES}
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${MWG_CORE_TEST_HEADERS}
    ${MWG_CORE_TEST_SOURCES}
)
source_group("ActiveX Test Sources" FILES ${MWG_ACTIVEX_SHARED_SOURCES})

target_include_directories(MapWinGIS.Core.Tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/MapWinGIS.ActiveX"
)

target_compile_definitions(MapWinGIS.Core.Tests PRIVATE
    MWG_ENABLE_RENDERER_TEST_HOOKS
)

target_link_libraries(MapWinGIS.Core.Tests PRIVATE
    MapWinGIS::Core
    MapWinGIS.ActiveX.Interface.Generated
    GDAL::GDAL
    GTest::gtest
    ole32
    oleaut32
    uuid
    gdi32
    user32
    opengl32
    psapi
)

mwg_apply_common_options(MapWinGIS.Core.Tests)
target_link_options(MapWinGIS.Core.Tests PRIVATE /SUBSYSTEM:CONSOLE)
set_target_properties(MapWinGIS.Core.Tests PROPERTIES FOLDER "tests")

add_dependencies(MapWinGIS.Core.Tests
    MapWinGIS.ActiveX
    MapWinGIS.RuntimeData
)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/test-results")
gtest_discover_tests(MapWinGIS.Core.Tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
    WORKING_DIRECTORY "${MWG_RUNTIME_DIR}"
    XML_OUTPUT_DIR "${PROJECT_BINARY_DIR}/test-results"
)

if(MWG_PLATFORM STREQUAL "Win32")
    set(_mwg_activation_shell
        "$ENV{WINDIR}/SysWOW64/WindowsPowerShell/v1.0/powershell.exe")
    if(NOT EXISTS "${_mwg_activation_shell}")
        unset(_mwg_activation_shell)
    endif()
else()
    find_program(_mwg_activation_shell NAMES pwsh powershell)
endif()

if(_mwg_activation_shell)
    add_test(NAME Verify-ActiveXActivation
        COMMAND "${_mwg_activation_shell}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/Verify-ActiveXActivation.ps1"
            -Platform "${MWG_PLATFORM}"
            -BinDirectory "${MWG_RUNTIME_DIR}"
    )
    set_tests_properties(Verify-ActiveXActivation PROPERTIES
        LABELS "interactive"
        WORKING_DIRECTORY "${MWG_RUNTIME_DIR}"
    )
else()
    message(STATUS
        "Skipping the interactive ActiveX activation CTest entry: no PowerShell process "
        "matching ${MWG_PLATFORM} was found.")
endif()
