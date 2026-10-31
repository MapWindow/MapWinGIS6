# SPDX-License-Identifier: MPL-2.0
#
# MapWinGIS.ocx: thin ActiveX/COM wrapper over MapWinGIS.Core (static ATL,
# apartment threaded).

include(MwgMidl)

mwg_add_midl_target(MapWinGIS.ActiveX.Interface IDL MapWinGIS.ActiveX.idl)

# Compiled into the OCX and, with MWG_ENABLE_RENDERER_TEST_HOOKS, into the
# tests (docs/CMAKE-MIGRATION.md, D4).
set(MWG_ACTIVEX_SHARED_SOURCES
    ComConversion.cpp
    GdalOperationDispatcher.cpp
    GdiVectorLayerRenderer.cpp
    OpenGlVectorLayerRenderer.cpp
    VectorLayerRenderer.cpp
    ViewTransform.cpp
)
list(TRANSFORM MWG_ACTIVEX_SHARED_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE MWG_ACTIVEX_SHARED_SOURCES)
set(MWG_ACTIVEX_SHARED_SOURCES "${MWG_ACTIVEX_SHARED_SOURCES}" PARENT_SCOPE)

set(MWG_ACTIVEX_HEADERS
    ActiveXModule.h
    ComConversion.h
    GdalDispatchWindow.h
    GdalEventSource.h
    GdalOperationDispatcher.h
    GdiVectorLayerRenderer.h
    MapWinGISControl.h
    OpenGlVectorLayerRenderer.h
    VectorLayerRenderer.h
    VectorRenderBackend.h
    ViewTransform.h
    resource.h
)

set(MWG_ACTIVEX_SOURCES
    ActiveXModule.cpp
    GdalEventSource.cpp
    MapWinGISControl.cpp
)

set(MWG_ACTIVEX_RESOURCES
    MapWinGIS.ActiveX.rc
    MapWinGIS.ActiveX.def
    MapWinGISControl.rgs
)

# SHARED, not MODULE: keeps the import library and the runtime-DLL handling.
add_library(MapWinGIS.ActiveX SHARED)

target_sources(MapWinGIS.ActiveX PRIVATE
    ${MWG_ACTIVEX_HEADERS}
    ${MWG_ACTIVEX_SOURCES}
    ${MWG_ACTIVEX_SHARED_SOURCES}
    ${MWG_ACTIVEX_RESOURCES}
    "${MapWinGIS.ActiveX.Interface_HEADER}"
    "${MapWinGIS.ActiveX.Interface_IID}"
)
source_group("Header Files" FILES ${MWG_ACTIVEX_HEADERS})
source_group("Source Files" FILES ${MWG_ACTIVEX_SOURCES} ${MWG_ACTIVEX_SHARED_SOURCES})
source_group("Resource Files" FILES ${MWG_ACTIVEX_RESOURCES})
source_group("Generated Files" FILES
    "${MapWinGIS.ActiveX.Interface_HEADER}"
    "${MapWinGIS.ActiveX.Interface_IID}")

# The .rc embeds the generated type library (1 TYPELIB "MapWinGIS.ActiveX.tlb");
# rc's dependency scanner does not see it.
set_source_files_properties(MapWinGIS.ActiveX.rc PROPERTIES
    OBJECT_DEPENDS "${MapWinGIS.ActiveX.Interface_TLB};${CMAKE_CURRENT_SOURCE_DIR}/MapWinGISControl.rgs")

target_link_libraries(MapWinGIS.ActiveX PRIVATE
    MapWinGIS::Core
    MapWinGIS.ActiveX.Interface.Generated
    # Explicit Windows libraries (audit). ATL is static: atlbase.h links
    # atls.lib through #pragma comment, and OpenGlVectorLayerRenderer.cpp links
    # opengl32.lib the same way.
    ole32 oleaut32 uuid user32 gdi32 advapi32 kernel32
)

target_compile_definitions(MapWinGIS.ActiveX PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:_ATL_APARTMENT_THREADED>")

# MSBuild ResourceCompile: /l 0x0409 and the MIDL output directory on the include path.
target_compile_options(MapWinGIS.ActiveX PRIVATE "$<$<COMPILE_LANGUAGE:RC>:/l0x0409>")

mwg_apply_common_options(MapWinGIS.ActiveX)

set_target_properties(MapWinGIS.ActiveX PROPERTIES
    OUTPUT_NAME MapWinGIS
    SUFFIX ".ocx"
    # The COM entry points are exported through MapWinGIS.ActiveX.def; no
    # <target>_EXPORTS macro, as in MSBuild.
    DEFINE_SYMBOL ""
    FOLDER "src"
)

add_dependencies(MapWinGIS.ActiveX MapWinGIS.ActiveX.Interface)
