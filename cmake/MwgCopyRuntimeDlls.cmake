# SPDX-License-Identifier: MPL-2.0
#
# Copy runtime dependencies when TARGET_RUNTIME_DLLS is non-empty.

if(NOT DEFINED MWG_RUNTIME_DLL_LIST_FILE OR NOT EXISTS "${MWG_RUNTIME_DLL_LIST_FILE}")
    message(FATAL_ERROR
        "Runtime DLL list file was not generated: '${MWG_RUNTIME_DLL_LIST_FILE}'.")
endif()

file(STRINGS "${MWG_RUNTIME_DLL_LIST_FILE}" _runtime_dlls)
if(NOT _runtime_dlls)
    return()
endif()

foreach(_dll IN LISTS _runtime_dlls)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${MWG_RUNTIME_DIR}"
        RESULT_VARIABLE _copy_result
        ERROR_VARIABLE _copy_error
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "Failed to deploy runtime DLL '${_dll}': ${_copy_error}")
    endif()
endforeach()
