# SPDX-License-Identifier: MPL-2.0
#
# mwg_add_midl_target(<target> IDL <file.idl> [OUTPUT_DIR <dir>] [DEPENDS <file>...])
#
# Runs midl.exe through one explicit custom command for every generator (only
# the Visual Studio generators understand .idl sources), see
# docs/CMAKE-MIGRATION.md (D3). Creates:
#
#   <target>            custom target that owns the MIDL invocation
#   <target>.Generated  INTERFACE library: the MIDL output include directory,
#                       for the OCX and the tests
#
# and sets, in the caller's scope:
#   <target>_HEADER  <name>_i.h
#   <target>_IID     <name>_i.c (C, compile with the consumer)
#   <target>_TLB     <name>.tlb
#   <target>_DIR     output directory
#
# The options match the MSBuild Midl task: /W1 /char signed /env x64|win32
# /target "NT60". This IDL declares no non-automation interfaces, so MIDL
# writes no proxy or dlldata file; they are redirected into the output
# directory anyway and are not declared as outputs.

function(_mwg_find_midl out_var)
    # The MSBuild Midl task uses the SDK's x86 midl.exe; any host build works.
    set(hints "")
    if(CMAKE_RC_COMPILER)
        get_filename_component(rc_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
        get_filename_component(sdk_bin "${rc_dir}" DIRECTORY)
        list(APPEND hints "${rc_dir}" "${sdk_bin}/x64" "${sdk_bin}/x86")
    endif()
    find_program(MWG_MIDL_EXECUTABLE midl HINTS ${hints} DOC "MIDL compiler from the Windows SDK")
    if(NOT MWG_MIDL_EXECUTABLE)
        message(FATAL_ERROR
            "midl.exe was not found. It ships with the Windows SDK ${MWG_WINDOWS_SDK}; "
            "configure from a Visual Studio toolchain (CLion) or a VS 2026 Developer PowerShell.")
    endif()
    set(${out_var} "${MWG_MIDL_EXECUTABLE}" PARENT_SCOPE)
endfunction()

function(mwg_add_midl_target target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "IDL;OUTPUT_DIR" "DEPENDS")
    if(NOT arg_IDL)
        message(FATAL_ERROR "mwg_add_midl_target: IDL is required")
    endif()
    if(NOT arg_OUTPUT_DIR)
        set(arg_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/midl")
    endif()

    _mwg_find_midl(midl)

    # MIDL preprocesses the IDL with cl.exe, which it finds on PATH only. The
    # Ninja generator inherits the vcvars PATH; the Visual Studio generators
    # run custom commands in an MSBuild environment that has it. A plain shell
    # with a cl.exe from CMAKE_C_COMPILER is not enough.
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
        find_program(_mwg_cl_on_path cl NO_CACHE NO_DEFAULT_PATH PATHS ENV PATH)
        if(NOT _mwg_cl_on_path)
            message(FATAL_ERROR
                "cl.exe is not on PATH, but midl.exe needs it to preprocess ${arg_IDL}.\n"
                "Use a Visual Studio toolchain in CLion, or configure and build from a "
                "VS 2026 Developer PowerShell (vcvarsall.bat).")
        endif()
    endif()

    cmake_path(ABSOLUTE_PATH arg_IDL BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE idl)
    # LAST_ONLY: MapWinGIS.ActiveX.idl -> MapWinGIS.ActiveX
    cmake_path(GET idl STEM LAST_ONLY name)
    cmake_path(GET idl PARENT_PATH idl_dir)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(env x64)
    else()
        set(env win32)
    endif()

    set(header "${arg_OUTPUT_DIR}/${name}_i.h")
    set(iid "${arg_OUTPUT_DIR}/${name}_i.c")
    set(tlb "${arg_OUTPUT_DIR}/${name}.tlb")

    add_custom_command(
        OUTPUT "${header}" "${iid}" "${tlb}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${arg_OUTPUT_DIR}"
        COMMAND "${midl}" /W1 /nologo /char signed /env ${env} /target NT60
            /I "${idl_dir}"
            /out "${arg_OUTPUT_DIR}"
            /h "${name}_i.h"
            /iid "${name}_i.c"
            /proxy "${name}_p.c"
            /dlldata dlldata.c
            /tlb "${name}.tlb"
            "${idl}"
        MAIN_DEPENDENCY "${idl}"
        DEPENDS ${arg_DEPENDS}
        WORKING_DIRECTORY "${idl_dir}"
        COMMENT "MIDL ${name}.idl"
        VERBATIM
    )

    add_custom_target(${target} DEPENDS "${header}" "${iid}" "${tlb}" SOURCES "${idl}")
    set_target_properties(${target} PROPERTIES FOLDER "src")

    add_library(${target}.Generated INTERFACE)
    target_include_directories(${target}.Generated INTERFACE "${arg_OUTPUT_DIR}")
    add_dependencies(${target}.Generated ${target})

    set(${target}_HEADER "${header}" PARENT_SCOPE)
    set(${target}_IID "${iid}" PARENT_SCOPE)
    set(${target}_TLB "${tlb}" PARENT_SCOPE)
    set(${target}_DIR "${arg_OUTPUT_DIR}" PARENT_SCOPE)
endfunction()
