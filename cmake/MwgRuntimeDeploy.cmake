# SPDX-License-Identifier: MPL-2.0
#
# Runtime data deployment (MSBuild: MwgDeployGdalRuntimeData).
#
# vcpkg installs the GDAL and PROJ data under share/; GdalEnvironment looks for
# "gdal-data" and "proj" beside the loaded module. The target
# MapWinGIS.RuntimeData copies both into the runtime folder. Each deployed file
# is an output so a missing file is repaired without deleting unchanged data.

option(MWG_DEPLOY_RUNTIME_DATA "Copy gdal-data and proj into the runtime folder (MSBuild: MwgDeployRuntimeData)" ON)

function(mwg_add_runtime_data_target)
    set(share_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    set(gdal_dir "${share_dir}/gdal")
    set(proj_dir "${share_dir}/proj")

    if(NOT MWG_DEPLOY_RUNTIME_DATA)
        add_custom_target(MapWinGIS.RuntimeData)
        return()
    endif()

    if(NOT EXISTS "${gdal_dir}")
        message(WARNING
            "GDAL runtime data was not found in ${gdal_dir}. "
            "Run build\\scripts\\bootstrap-vcpkg.ps1 and reconfigure.")
        add_custom_target(MapWinGIS.RuntimeData)
        return()
    endif()

    file(GLOB_RECURSE gdal_files
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${gdal_dir}/*")
    set(data_files ${gdal_files})
    set(runtime_data_outputs)
    foreach(data_file IN LISTS gdal_files)
        file(RELATIVE_PATH relative_file "${gdal_dir}" "${data_file}")
        list(APPEND runtime_data_outputs
            "${MWG_RUNTIME_DIR}/gdal-data/${relative_file}")
    endforeach()

    if(EXISTS "${proj_dir}")
        file(GLOB_RECURSE proj_files
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES false
            "${proj_dir}/*")
        list(APPEND data_files ${proj_files})
        foreach(data_file IN LISTS proj_files)
            file(RELATIVE_PATH relative_file "${proj_dir}" "${data_file}")
            list(APPEND runtime_data_outputs
                "${MWG_RUNTIME_DIR}/proj/${relative_file}")
        endforeach()
    endif()

    if(NOT data_files)
        add_custom_target(MapWinGIS.RuntimeData)
        return()
    endif()

    list(SORT data_files)

    set(commands
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${gdal_dir}" "${MWG_RUNTIME_DIR}/gdal-data")
    if(EXISTS "${proj_dir}")
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${proj_dir}" "${MWG_RUNTIME_DIR}/proj")
    endif()

    add_custom_command(
        OUTPUT ${runtime_data_outputs}
        ${commands}
        DEPENDS ${data_files}
        COMMENT "Deploying GDAL and PROJ runtime data"
        VERBATIM
    )
    add_custom_target(MapWinGIS.RuntimeData DEPENDS ${runtime_data_outputs})
endfunction()

function(mwg_deploy_runtime_dlls target)
    set(runtime_dll_list_file
        "${PROJECT_BINARY_DIR}/runtime-dlls/${target}-$<CONFIG>.txt")
    file(GENERATE
        OUTPUT "${runtime_dll_list_file}"
        CONTENT "$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,\n>"
    )

    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DMWG_RUNTIME_DLL_LIST_FILE=${runtime_dll_list_file}"
            "-DMWG_RUNTIME_DIR=$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MwgCopyRuntimeDlls.cmake"
        COMMENT "Deploying runtime DLLs for ${target}"
        VERBATIM
    )
endfunction()
