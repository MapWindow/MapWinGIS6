#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "unofficial::Lerc::Lerc" for configuration "Release"
set_property(TARGET unofficial::Lerc::Lerc APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(unofficial::Lerc::Lerc PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/Lerc.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/Lerc.dll"
  )

list(APPEND _cmake_import_check_targets unofficial::Lerc::Lerc )
list(APPEND _cmake_import_check_files_for_unofficial::Lerc::Lerc "${_IMPORT_PREFIX}/lib/Lerc.lib" "${_IMPORT_PREFIX}/bin/Lerc.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
