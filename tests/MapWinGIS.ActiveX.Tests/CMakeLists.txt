# SPDX-License-Identifier: MPL-2.0
#
# Native GoogleTest suite for the ActiveX implementation and COM control.

find_package(GDAL CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

set(MWG_ACTIVEX_TEST_HEADERS
    OpenGlVectorLayerRendererTestAccess.h
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/TestData.h"
)

if(NOT MWG_ACTIVEX_SHARED_SOURCES)
    message(FATAL_ERROR
        "MWG_ACTIVEX_SHARED_SOURCES is empty; the ActiveX subdirectory must be added before the tests.")
endif()

set(MWG_ACTIVEX_TEST_SOURCES
    ActiveXActivationTests.cpp
    ComConversionTests.cpp
    FrameBufferTests.cpp
    GdalOperationDispatcherTests.cpp
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/Main.cpp"
    RenderingBaselineTests.cpp
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/TestData.cpp"
    VectorLayerRendererTests.cpp
    "${PROJECT_SOURCE_DIR}/src/MapWinGIS.ActiveX/FrameBuffer.cpp"
    ${MWG_ACTIVEX_SHARED_SOURCES}
)

add_executable(MapWinGIS.ActiveX.Tests)
target_sources(MapWinGIS.ActiveX.Tests PRIVATE
    ${MWG_ACTIVEX_TEST_HEADERS}
    ${MWG_ACTIVEX_TEST_SOURCES}
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    OpenGlVectorLayerRendererTestAccess.h
    ActiveXActivationTests.cpp
    ComConversionTests.cpp
    FrameBufferTests.cpp
    GdalOperationDispatcherTests.cpp
    RenderingBaselineTests.cpp
    VectorLayerRendererTests.cpp
)
source_group("Shared Core Test Sources" FILES
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/Main.cpp"
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/TestData.cpp"
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests/TestData.h"
)
source_group("ActiveX Implementation Sources" FILES
    "${PROJECT_SOURCE_DIR}/src/MapWinGIS.ActiveX/FrameBuffer.cpp"
    ${MWG_ACTIVEX_SHARED_SOURCES}
)

target_include_directories(MapWinGIS.ActiveX.Tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/MapWinGIS.ActiveX"
    "${PROJECT_SOURCE_DIR}/tests/MapWinGIS.Core.Tests"
)

target_compile_definitions(MapWinGIS.ActiveX.Tests PRIVATE
    MWG_ENABLE_RENDERER_TEST_HOOKS
)

target_link_libraries(MapWinGIS.ActiveX.Tests PRIVATE
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

mwg_apply_common_options(MapWinGIS.ActiveX.Tests)
target_link_options(MapWinGIS.ActiveX.Tests PRIVATE /SUBSYSTEM:CONSOLE)
set_target_properties(MapWinGIS.ActiveX.Tests PROPERTIES FOLDER "tests")

add_dependencies(MapWinGIS.ActiveX.Tests
    MapWinGIS.ActiveX
    MapWinGIS.RuntimeData
)
mwg_deploy_runtime_dlls(MapWinGIS.ActiveX.Tests)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/test-results")
gtest_discover_tests(MapWinGIS.ActiveX.Tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
    TEST_FILTER "*-ActiveXHosting.ResizesRepaintsAndRecreatesControl"
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
            -Configuration "$<CONFIG>"
            -Platform "${MWG_PLATFORM}"
            -BinDirectory "${MWG_RUNTIME_DIR}"
            -TestExecutable "MapWinGIS.ActiveX.Tests.exe"
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
