# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# `loom_wasm_weave(<target> OUTPUT <file.wasm> SOURCES <source>... [INCLUDE_DIRECTORIES <dir>...]
# [DEFINITIONS <name[=value]>...])` builds a weave as a WebAssembly module: its sources, the glue
# beside this file and Loom's core and switchboard, compiled for wasm32 by the wasi-sdk
# LOOM_WASI_SDK names (custom commands: it is not this project's compiler) and linked as a reactor.
# docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

if(COMMAND loom_wasm_weave)
    return()
endif()

set(LOOM_WASI_SDK "" CACHE PATH
    "An unpacked wasi-sdk release, to build weaves as WebAssembly modules (loom_wasm_weave)")

# Where this file and the glue sit, and the Loom sources a module carries.
set(_LOOM_WASM_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
get_filename_component(_LOOM_WASM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_LOOM_WASM_ROOT "${_LOOM_WASM_ROOT}" CACHE INTERNAL "")

# The flags of every wasm32 compile: the target, C++20 with WebAssembly's exception form, and
# the warning set Loom's own builds keep.
function(_loom_wasm_flags out)
    set(${out}
        --target=wasm32-wasip1 "--sysroot=${LOOM_WASI_SDK}/share/wasi-sysroot"
        -std=c++20 -O2 -g0 -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror
        "-I${_LOOM_WASM_ROOT}/include" "-I${_LOOM_WASM_ROOT}/src"
        PARENT_SCOPE)
endfunction()

# One wasm32 object from one source, with its dependency file; its path in `out`.
function(_loom_wasm_object out source object_dir extra_flags)
    get_filename_component(_name "${source}" NAME_WE)
    string(MD5 _key "${source};${extra_flags}")
    string(SUBSTRING "${_key}" 0 8 _key)
    set(_object "${object_dir}/${_name}-${_key}.o")
    _loom_wasm_flags(_flags)
    add_custom_command(
        OUTPUT "${_object}"
        COMMAND "${LOOM_WASI_SDK}/bin/clang++" ${_flags} ${extra_flags}
                -MD -MF "${_object}.d" -c "${source}" -o "${_object}"
        DEPENDS "${source}"
        DEPFILE "${_object}.d"
        COMMENT "wasm32: ${_name}"
        VERBATIM)
    set(${out} "${_object}" PARENT_SCOPE)
endfunction()

# Loom's core and switchboard for wasm32, once a build: the archive every module links.
function(_loom_wasm_core out)
    get_property(_archive GLOBAL PROPERTY _LOOM_WASM_CORE)
    if(_archive)
        set(${out} "${_archive}" PARENT_SCOPE)
        return()
    endif()
    set(_dir "${CMAKE_BINARY_DIR}/loom-wasm32")
    set(_objects "")
    foreach(_source IN ITEMS
            src/schema.cpp src/value.cpp src/gate.cpp src/registry.cpp src/serialize.cpp
            src/content_id.cpp src/detail/json.cpp src/detail/base64.cpp src/detail/binary.cpp
            src/switchboard/switchboard.cpp)
        _loom_wasm_object(_object "${_LOOM_WASM_ROOT}/${_source}" "${_dir}" "")
        list(APPEND _objects "${_object}")
    endforeach()
    set(_archive "${_dir}/libloom-wasm32.a")
    add_custom_command(
        OUTPUT "${_archive}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${_archive}"
        COMMAND "${LOOM_WASI_SDK}/bin/llvm-ar" rcs "${_archive}" ${_objects}
        DEPENDS ${_objects}
        COMMENT "wasm32: Loom's core and switchboard"
        VERBATIM)
    set_property(GLOBAL PROPERTY _LOOM_WASM_CORE "${_archive}")
    set(${out} "${_archive}" PARENT_SCOPE)
endfunction()

function(loom_wasm_weave target)
    cmake_parse_arguments(PARSE_ARGV 1 _w "" "OUTPUT" "SOURCES;INCLUDE_DIRECTORIES;DEFINITIONS")
    if(NOT LOOM_WASI_SDK OR NOT EXISTS "${LOOM_WASI_SDK}/bin/clang++")
        message(FATAL_ERROR
            "loom_wasm_weave(${target}): LOOM_WASI_SDK names no wasi-sdk ('${LOOM_WASI_SDK}'); "
            "unpack a wasi-sdk release and name its directory (docs/guides/tools.md)")
    endif()
    if(NOT _w_OUTPUT OR NOT _w_SOURCES)
        message(FATAL_ERROR "loom_wasm_weave(${target}): OUTPUT and SOURCES are required")
    endif()
    set(_extra "")
    foreach(_dir IN LISTS _w_INCLUDE_DIRECTORIES)
        list(APPEND _extra "-I${_dir}")
    endforeach()
    foreach(_def IN LISTS _w_DEFINITIONS)
        list(APPEND _extra "-D${_def}")
    endforeach()
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}.wasm32")
    set(_objects "")
    foreach(_source IN LISTS _w_SOURCES ITEMS "${_LOOM_WASM_DIR}/loom-wasm-glue.cpp")
        get_filename_component(_source "${_source}" ABSOLUTE)
        _loom_wasm_object(_object "${_source}" "${_dir}" "${_extra}")
        list(APPEND _objects "${_object}")
    endforeach()
    _loom_wasm_core(_core)
    get_filename_component(_output "${_w_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(_output_dir "${_output}" DIRECTORY)
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
        COMMAND "${LOOM_WASI_SDK}/bin/clang++" --target=wasm32-wasip1
                "--sysroot=${LOOM_WASI_SDK}/share/wasi-sysroot" -O2 -fwasm-exceptions
                -mexec-model=reactor ${_objects} "${_core}" -lunwind -o "${_output}"
        DEPENDS ${_objects} "${_core}"
        COMMENT "wasm32: linking ${target}"
        VERBATIM)
    add_custom_target(${target} ALL DEPENDS "${_output}")
    set_target_properties(${target} PROPERTIES LOOM_WASM_MODULE "${_output}")
endfunction()
