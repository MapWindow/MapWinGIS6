#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qhull::qhullcpp" for configuration "Debug"
set_property(TARGET Qhull::qhullcpp APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Qhull::qhullcpp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/qhullcpp_d.lib"
  )

list(APPEND _cmake_import_check_targets Qhull::qhullcpp )
list(APPEND _cmake_import_check_files_for_Qhull::qhullcpp "${_IMPORT_PREFIX}/debug/lib/qhullcpp_d.lib" )

# Import target "Qhull::qhull_r" for configuration "Debug"
set_property(TARGET Qhull::qhull_r APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Qhull::qhull_r PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/qhull_rd.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/qhull_rd.dll"
  )

list(APPEND _cmake_import_check_targets Qhull::qhull_r )
list(APPEND _cmake_import_check_files_for_Qhull::qhull_r "${_IMPORT_PREFIX}/debug/lib/qhull_rd.lib" "${_IMPORT_PREFIX}/debug/bin/qhull_rd.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
