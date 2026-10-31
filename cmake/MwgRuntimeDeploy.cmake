# SPDX-License-Identifier: MPL-2.0
#
# Runtime data deployment (MSBuild: MwgDeployGdalRuntimeData).
#
# vcpkg installs the GDAL and PROJ data under share/; GdalEnvironment looks for
# "gdal-data" and "proj" beside the loaded module. The target
# MapWinGIS.RuntimeData copies both into the runtime folder. A stamp file in
# that folder keeps the copy from running on every build: it reruns only when
# a data file changes or the runtime folder was cleaned.

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

    file(GLOB_RECURSE data_files CONFIGURE_DEPENDS "${gdal_dir}/*" "${proj_dir}/*")

    set(stamp "${MWG_RUNTIME_DIR}/.mwg-runtime-data.stamp")
    set(commands
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${gdal_dir}" "${MWG_RUNTIME_DIR}/gdal-data")
    if(EXISTS "${proj_dir}")
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${proj_dir}" "${MWG_RUNTIME_DIR}/proj")
    endif()

    add_custom_command(
        OUTPUT "${stamp}"
        ${commands}
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${data_files}
        COMMENT "Deploying GDAL and PROJ runtime data"
        VERBATIM
    )
    add_custom_target(MapWinGIS.RuntimeData DEPENDS "${stamp}")
endfunction()
