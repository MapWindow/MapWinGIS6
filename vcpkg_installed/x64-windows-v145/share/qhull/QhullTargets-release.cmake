#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qhull::qhullcpp" for configuration "Release"
set_property(TARGET Qhull::qhullcpp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Qhull::qhullcpp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/qhullcpp.lib"
  )

list(APPEND _cmake_import_check_targets Qhull::qhullcpp )
list(APPEND _cmake_import_check_files_for_Qhull::qhullcpp "${_IMPORT_PREFIX}/lib/qhullcpp.lib" )

# Import target "Qhull::qhull_r" for configuration "Release"
set_property(TARGET Qhull::qhull_r APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Qhull::qhull_r PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/qhull_r.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/qhull_r.dll"
  )

list(APPEND _cmake_import_check_targets Qhull::qhull_r )
list(APPEND _cmake_import_check_files_for_Qhull::qhull_r "${_IMPORT_PREFIX}/lib/qhull_r.lib" "${_IMPORT_PREFIX}/bin/qhull_r.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
