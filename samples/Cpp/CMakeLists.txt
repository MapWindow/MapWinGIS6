# SPDX-License-Identifier: MPL-2.0
#
# Native sample applications.

add_executable(MapWinGIS.Samples.Translate translate.cpp)
target_link_libraries(MapWinGIS.Samples.Translate PRIVATE MapWinGIS::Core)

mwg_apply_common_options(MapWinGIS.Samples.Translate)
target_link_options(MapWinGIS.Samples.Translate PRIVATE /SUBSYSTEM:CONSOLE)
set_target_properties(MapWinGIS.Samples.Translate PROPERTIES
    OUTPUT_NAME translate
    FOLDER "samples"
)

mwg_deploy_runtime_dlls(MapWinGIS.Samples.Translate)
