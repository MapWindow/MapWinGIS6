# SPDX-License-Identifier: MPL-2.0
#
# Native GoogleTest suite for the Core API.

find_package(GDAL CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# vcpkg marks GoogleTest headers as system, conflicting with /external:W4.
set_property(TARGET GTest::gtest PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

set(MWG_CORE_TEST_HEADERS
    TestData.h
)

set(MWG_CORE_TEST_SOURCES
    GdalAlgorithmRunnerTests.cpp
    GdalEnvironmentTests.cpp
    GdalModernCapabilitiesTests.cpp
    GdalRuntimeTests.cpp
    GdalTypedOptionsTests.cpp
    GdalUtilsTests.cpp
    Main.cpp
    MapRenderSceneTests.cpp
    MapTests.cpp
    MapViewTests.cpp
    PolygonTriangulationTests.cpp
    TestData.cpp
    VectorDatasetTests.cpp
    VectorLayerTests.cpp
    VectorRenderSceneTests.cpp
    ViewportTests.cpp
)

add_executable(MapWinGIS.Core.Tests)
target_sources(MapWinGIS.Core.Tests PRIVATE
    ${MWG_CORE_TEST_HEADERS}
    ${MWG_CORE_TEST_SOURCES}
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${MWG_CORE_TEST_HEADERS}
    ${MWG_CORE_TEST_SOURCES}
)

target_link_libraries(MapWinGIS.Core.Tests PRIVATE
    MapWinGIS::Core
    GDAL::GDAL
    GTest::gtest
)

if("full-drivers" IN_LIST VCPKG_MANIFEST_FEATURES)
    target_compile_definitions(MapWinGIS.Core.Tests PRIVATE MWG_TEST_FULL_DRIVERS)
endif()

mwg_apply_common_options(MapWinGIS.Core.Tests)
target_link_options(MapWinGIS.Core.Tests PRIVATE /SUBSYSTEM:CONSOLE)
set_target_properties(MapWinGIS.Core.Tests PROPERTIES FOLDER "tests")

add_dependencies(MapWinGIS.Core.Tests MapWinGIS.RuntimeData)
mwg_deploy_runtime_dlls(MapWinGIS.Core.Tests)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/test-results")
gtest_discover_tests(MapWinGIS.Core.Tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
    WORKING_DIRECTORY "${MWG_RUNTIME_DIR}"
    XML_OUTPUT_DIR "${PROJECT_BINARY_DIR}/test-results"
)
