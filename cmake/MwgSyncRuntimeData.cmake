# SPDX-License-Identifier: MPL-2.0

foreach(required_variable IN ITEMS
        MWG_RUNTIME_DIR
        MWG_GDAL_SOURCE_DIR
        MWG_PROJ_SOURCE_DIR
        MWG_CURRENT_MANIFEST
        MWG_STAMP_FILE)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required to deploy runtime data.")
    endif()
endforeach()

if(NOT EXISTS "${MWG_CURRENT_MANIFEST}")
    message(FATAL_ERROR "Runtime-data manifest was not found: ${MWG_CURRENT_MANIFEST}")
endif()

set(installed_manifest "${MWG_RUNTIME_DIR}/.mwg-runtime-data.outputs")
set(previous_paths)
if(EXISTS "${installed_manifest}")
    file(STRINGS "${installed_manifest}" previous_paths)
endif()
file(STRINGS "${MWG_CURRENT_MANIFEST}" current_paths)

foreach(runtime_path IN LISTS previous_paths)
    if(NOT runtime_path IN_LIST current_paths)
        file(REMOVE "${MWG_RUNTIME_DIR}/${runtime_path}")
    endif()
endforeach()

foreach(runtime_path IN LISTS current_paths)
    if(runtime_path MATCHES "^gdal-data/")
        string(REGEX REPLACE "^gdal-data/" "" source_relative_path "${runtime_path}")
        set(source_path "${MWG_GDAL_SOURCE_DIR}/${source_relative_path}")
    elseif(runtime_path MATCHES "^proj/")
        string(REGEX REPLACE "^proj/" "" source_relative_path "${runtime_path}")
        set(source_path "${MWG_PROJ_SOURCE_DIR}/${source_relative_path}")
    else()
        message(FATAL_ERROR "Unexpected runtime-data path: ${runtime_path}")
    endif()

    if(NOT EXISTS "${source_path}")
        message(FATAL_ERROR "Runtime-data source was not found: ${source_path}")
    endif()

    set(destination_path "${MWG_RUNTIME_DIR}/${runtime_path}")
    get_filename_component(destination_directory "${destination_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_directory}")
    file(COPY_FILE "${source_path}" "${destination_path}" ONLY_IF_DIFFERENT)
endforeach()

file(READ "${MWG_CURRENT_MANIFEST}" current_manifest_content)
file(MAKE_DIRECTORY "${MWG_RUNTIME_DIR}")
file(WRITE "${installed_manifest}" "${current_manifest_content}")
file(REMOVE "${MWG_RUNTIME_DIR}/.mwg-runtime-data.inputs")
file(TOUCH "${MWG_STAMP_FILE}")