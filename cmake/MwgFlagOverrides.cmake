# SPDX-License-Identifier: MPL-2.0
#
# CMAKE_USER_MAKE_RULES_OVERRIDE file: runs inside project(), after CMake's
# MSVC platform module and before the CMAKE_<LANG>_FLAGS* cache entries are
# created.
#
# CMake's MSVC defaults (/DWIN32 /D_WINDOWS /EHsc, /Ob0 /Od /RTC1, /O2 /Ob2
# /DNDEBUG) do not match the MSBuild build, which defines neither WIN32 nor
# _WINDOWS. Every compile option comes from mwg_apply_common_options()
# instead (cmake/MwgCompilerOptions.cmake), so the targets carry exactly the
# flags documented in docs/CMAKE-MIGRATION.md and nothing leaks to consumers.
# Linker defaults (/machine, /debug /INCREMENTAL, /INCREMENTAL:NO) are kept.

foreach(_mwg_lang IN ITEMS C CXX)
    set(CMAKE_${_mwg_lang}_FLAGS_INIT "")
    set(CMAKE_${_mwg_lang}_FLAGS_DEBUG_INIT "")
    set(CMAKE_${_mwg_lang}_FLAGS_RELEASE_INIT "")
endforeach()
unset(_mwg_lang)
