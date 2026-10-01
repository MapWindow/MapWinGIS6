
include(CMakeFindDependencyMacro)
if(NOT WIN32)
    find_dependency(Threads)
endif()
if("OFF" AND "dynamic" STREQUAL "static")
    find_dependency(ICU COMPONENTS uc i18n)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/unofficial-sqlite3-targets.cmake)
