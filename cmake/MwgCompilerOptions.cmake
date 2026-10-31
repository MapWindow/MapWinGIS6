# SPDX-License-Identifier: MPL-2.0
#
# mwg_apply_common_options(<target>)
#
# The compiler and linker settings of build/props/MapWinGIS.Cpp.props plus the
# MSBuild defaults they rely on, applied PRIVATE to one target so that nothing
# leaks to consumers of MapWinGIS.Core. The effective MSBuild options are listed
# in docs/CMAKE-MIGRATION.md; keep both in sync.

option(MWG_ENABLE_ASAN "Compile with /fsanitize=address (MSBuild: MwgEnableAsan)" OFF)

function(mwg_apply_common_options target)
    get_target_property(type ${target} TYPE)

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        # The MIDL-generated _i.c is C.
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        # PDBs in Debug and Release, as in MSBuild (CMake's default is Debug only).
        MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase
        # MSBuild passes the vcpkg include directory as a plain /I, so warnings
        # in GDAL headers are reported at /W4. CMake would make imported
        # include directories /external:I with /external:W0 and hide them.
        NO_SYSTEM_FROM_IMPORTED ON
    )

    set(c_cxx "$<COMPILE_LANGUAGE:C,CXX>")
    set(debug "$<AND:${c_cxx},$<CONFIG:Debug>>")
    set(release "$<AND:${c_cxx},$<CONFIG:Release>>")

    target_compile_options(${target} PRIVATE
        # WarningLevel, DisableSpecificWarnings. 4251 is safe: one toolset and
        # one CRT for every binary of the product.
        "$<${c_cxx}:/W4;/WX-;/wd4251;/external:W4>"
        # AdditionalOptions
        "$<${c_cxx}:/utf-8;/Zc:__cplusplus;/Zc:preprocessor;/Zc:throwingNew;/bigobj>"
        # ConformanceMode, SDLCheck, ExceptionHandling and MSBuild defaults
        "$<${c_cxx}:/permissive-;/sdl;/EHsc;/GS;/Gd;/Gm-;/fp:precise;/Zc:wchar_t;/Zc:forScope;/Zc:inline>"
        # CMake passes /TP for C++ but not /TC for C (the MIDL _i.c).
        "$<$<COMPILE_LANGUAGE:C>:/TC>"
        "$<${debug}:/Od;/JMC>"
        "$<${release}:/O2;/Oi;/Gy;/guard:cf>"
    )
    if(MWG_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE "$<${c_cxx}:/fsanitize=address>")
    else()
        # BasicRuntimeChecks; incompatible with AddressSanitizer.
        target_compile_options(${target} PRIVATE "$<${debug}:/RTC1>")
    endif()

    target_compile_definitions(${target} PRIVATE
        # CharacterSet=Unicode, also for the resource compiler.
        UNICODE
        _UNICODE
        "$<${c_cxx}:WIN32_LEAN_AND_MEAN;NOMINMAX;_WIN32_WINNT=0x0A00;WINVER=0x0A00;NTDDI_VERSION=0x0A00000B;_CRT_SECURE_NO_WARNINGS>"
        "$<${c_cxx}:MWG_VERSION=\"${PROJECT_VERSION}\">"
        "$<${debug}:_DEBUG>"
        "$<${release}:NDEBUG>"
    )
    if(type STREQUAL "SHARED_LIBRARY")
        # MSBuild defines _WINDLL for every DLL project.
        target_compile_definitions(${target} PRIVATE "$<${c_cxx}:_WINDLL>")
    endif()

    # CMake's defaults already add /machine, /debug /INCREMENTAL (Debug) and
    # /INCREMENTAL:NO (Release).
    target_link_options(${target} PRIVATE
        /DYNAMICBASE
        /NXCOMPAT
        "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/guard:cf>"
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_options(${target} PRIVATE /HIGHENTROPYVA)
    else()
        # MSBuild Win32 defaults: keep frame pointers, no code analysis, and
        # safe exception handler tables in Release.
        target_compile_options(${target} PRIVATE "$<${c_cxx}:/Oy-;/analyze->")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:/SAFESEH>")
    endif()
    if(type STREQUAL "SHARED_LIBRARY")
        target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS)
    endif()
endfunction()
