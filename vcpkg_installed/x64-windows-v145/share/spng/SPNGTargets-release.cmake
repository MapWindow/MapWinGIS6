#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "spng::spng" for configuration "Release"
set_property(TARGET spng::spng APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(spng::spng PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/spng.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/spng.dll"
  )

list(APPEND _cmake_import_check_targets spng::spng )
list(APPEND _cmake_import_check_files_for_spng::spng "${_IMPORT_PREFIX}/lib/spng.lib" "${_IMPORT_PREFIX}/bin/spng.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
