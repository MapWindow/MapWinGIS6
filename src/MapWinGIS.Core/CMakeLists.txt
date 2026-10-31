# SPDX-License-Identifier: MPL-2.0
#
# MapWinGIS.Core.dll: the mapping engine. No COM, ATL or MFC.

find_package(GDAL CONFIG REQUIRED)
find_package(GEOS CONFIG REQUIRED)

set(MWG_CORE_PUBLIC_HEADERS
    include/mapwingis/core/Api.h
    include/mapwingis/core/Enums.h
    include/mapwingis/core/Error.h
    include/mapwingis/core/Extent.h
    include/mapwingis/gdal/GdalAlgorithmRunner.h
    include/mapwingis/gdal/GdalConfig.h
    include/mapwingis/gdal/GdalDiagnostics.h
    include/mapwingis/gdal/GdalEnvironment.h
    include/mapwingis/gdal/GdalModern.h
    include/mapwingis/gdal/GdalOperationResult.h
    include/mapwingis/gdal/GdalProgress.h
    include/mapwingis/gdal/GdalTypedOptions.h
    include/mapwingis/gdal/GdalUtils.h
    include/mapwingis/rendering/VectorRenderScene.h
    include/mapwingis/vector/FieldInfo.h
    include/mapwingis/vector/VectorDataset.h
)

set(MWG_CORE_PRIVATE_HEADERS
    src/internal/GdalExecution.h
    src/internal/GdalProgressAdapterAccess.h
    src/internal/Utf8.h
    src/internal/VectorLayerAccess.h
)

set(MWG_CORE_SOURCES
    src/gdal/GdalAlgorithmRunner.cpp
    src/gdal/GdalEnvironment.cpp
    src/gdal/GdalModern.cpp
    src/gdal/GdalOperationResult.cpp
    src/gdal/GdalRuntime.cpp
    src/gdal/GdalTypedOptions.cpp
    src/gdal/GdalUtils.cpp
    src/rendering/VectorRenderScene.cpp
    src/vector/VectorDataset.cpp
)

add_library(MapWinGIS.Core SHARED)
add_library(MapWinGIS::Core ALIAS MapWinGIS.Core)

target_sources(MapWinGIS.Core PRIVATE
    ${MWG_CORE_PUBLIC_HEADERS}
    ${MWG_CORE_PRIVATE_HEADERS}
    ${MWG_CORE_SOURCES}
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${MWG_CORE_PUBLIC_HEADERS}
    ${MWG_CORE_PRIVATE_HEADERS}
    ${MWG_CORE_SOURCES}
)

target_include_directories(MapWinGIS.Core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# MWG_CORE_BUILD selects __declspec(dllexport) in mapwingis/core/Api.h. Using it
# as the DEFINE_SYMBOL also replaces CMake's default MapWinGIS_Core_EXPORTS.
set_target_properties(MapWinGIS.Core PROPERTIES DEFINE_SYMBOL MWG_CORE_BUILD)

# No public header includes GDAL or GEOS, so both stay private. GEOS is used
# directly: GdalEnvironment::GeosVersion() calls GEOSversion().
target_link_libraries(MapWinGIS.Core PRIVATE GDAL::GDAL GEOS::geos_c)

mwg_apply_common_options(MapWinGIS.Core)

set_target_properties(MapWinGIS.Core PROPERTIES FOLDER "src")

add_dependencies(MapWinGIS.Core MapWinGIS.RuntimeData)
