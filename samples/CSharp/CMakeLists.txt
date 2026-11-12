# SPDX-License-Identifier: MPL-2.0
#
# .NET ActiveX host targets. CMake builds the native dependencies separately.

find_program(MWG_DOTNET_EXECUTABLE NAMES dotnet)
if(NOT MWG_DOTNET_EXECUTABLE)
    message(STATUS "Skipping ActiveX test-host targets: dotnet was not found.")
    return()
endif()

find_program(MWG_PWSH_EXECUTABLE NAMES pwsh)
if(NOT MWG_PWSH_EXECUTABLE)
    message(FATAL_ERROR
        "PowerShell 7 (pwsh) is required for the ActiveX test-host run targets.")
endif()

set(_mwg_host_project "${CMAKE_CURRENT_SOURCE_DIR}/ActiveXTestHost/ActiveXTestHost.csproj")
set(_mwg_host_launcher "${CMAKE_CURRENT_SOURCE_DIR}/ActiveXTestHost/Run-ActiveXTestHost.ps1")

add_custom_target(build_activex_host
    COMMAND "${MWG_DOTNET_EXECUTABLE}" build "${_mwg_host_project}"
        --configuration "$<CONFIG>"
        "-p:Platform=${MWG_PLATFORM}"
        "-p:OutputPath=${MWG_RUNTIME_DIR}/"
        -p:MwgCMakeBuild=true
        -p:BuildProjectReferences=false
    DEPENDS MapWinGIS.ActiveX MapWinGIS.RuntimeData
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ActiveXTestHost"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run_activex_host
    COMMAND "${MWG_PWSH_EXECUTABLE}" -NoProfile -File "${_mwg_host_launcher}"
        -Configuration "$<CONFIG>"
        -Platform "${MWG_PLATFORM}"
        -BinDirectory "${MWG_RUNTIME_DIR}"
    DEPENDS build_activex_host
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run_activex_host_automated
    COMMAND "${MWG_PWSH_EXECUTABLE}" -NoProfile -File "${_mwg_host_launcher}"
        -Configuration "$<CONFIG>"
        -Platform "${MWG_PLATFORM}"
        -BinDirectory "${MWG_RUNTIME_DIR}"
        -Automated
    DEPENDS build_activex_host
    USES_TERMINAL
    VERBATIM
)
