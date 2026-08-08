# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE INSTALLED-PACKAGE WITNESS DRIVER (MSVC-0).
#
#   cmake -DZEN_PREFIX=<install prefix> -DZEN_WORK=<scratch dir> [-DZEN_CONFIG=Debug]
#         [-DZEN_CMAKE_ARGS=...] -P tests/package/run.cmake
#
# Configures, builds and runs tests/package as a STRANGER against an installed Loom, and
# asks the produced artifact what it actually exports. It is the one command CI runs and
# the one a human runs, so the hosted lane and a local check cannot come to mean
# different things.
#
# WHY THE EXPORT TABLE IS ASKED SEPARATELY FROM "IT LOADED". A weave that loads proves
# the symbol was findable by SOME spelling the loader accepted. The host looks the name
# up as the literal string "zen_weave_abi" (src/kernel/kernel.cpp), so the artifact must
# carry exactly that -- not `?zen_weave_abi@@YAPEBUZenWeaveAbi@@XZ`, not
# `_zen_weave_abi@0`. Those would be a different ABI wearing the same source.
#
# Absence of an inspection tool is a FAILURE, never a skip: a proof that cannot run has
# not passed. Same rule tests/check_weave_contract.cmake keeps for nm.

foreach(v ZEN_PREFIX ZEN_WORK)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "package witness: -D${v}=... is required")
    endif()
endforeach()
if(NOT DEFINED ZEN_CONFIG OR ZEN_CONFIG STREQUAL "")
    set(ZEN_CONFIG Debug)
endif()

get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

function(zen_run label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "package witness: ${label} FAILED (exit ${rc})")
    endif()
    message(STATUS "package witness: ${label} ok")
endfunction()

# ---- configure / build, with NOTHING added to the consumer's flags ------------------
#
# ZEN_CMAKE_ARGS carries only what selects a TOOLCHAIN (compiler, generator program) --
# never a compile option. If a compile option is ever needed here, the package has
# stopped carrying its own requirements and this witness is supposed to go red.
#
# The generator is CHOSEN, not assumed: -DZEN_GENERATOR=... wins, else Ninja when it is
# actually on PATH, else CMake's platform default. Hard-coding Ninja would make this
# witness unrunnable on an ordinary Makefiles host -- and a proof a consumer cannot run
# is not much of a proof.
set(gen_args "")
if(DEFINED ZEN_GENERATOR AND NOT ZEN_GENERATOR STREQUAL "")
    set(gen_args -G "${ZEN_GENERATOR}")
else()
    find_program(ZEN_NINJA ninja)
    if(ZEN_NINJA)
        set(gen_args -G Ninja)
    endif()
endif()

zen_run("configure" ${CMAKE_COMMAND} -S "${here}" -B "${ZEN_WORK}"
        ${gen_args}
        "-DCMAKE_BUILD_TYPE=${ZEN_CONFIG}"
        "-DCMAKE_PREFIX_PATH=${ZEN_PREFIX}"
        ${ZEN_CMAKE_ARGS})
zen_run("build" ${CMAKE_COMMAND} --build "${ZEN_WORK}" --config "${ZEN_CONFIG}")

# ---- 1. the public macro surface ----------------------------------------------------
find_program(ZEN_WITNESS_MACROS witness-macros PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_MACROS)
    message(FATAL_ERROR "package witness: witness-macros was not produced in ${ZEN_WORK}")
endif()
zen_run("public macro surface" "${ZEN_WITNESS_MACROS}")

# ---- the dynamic-weave half, only where the install has a kernel ---------------------
file(GLOB weave_artifact
     "${ZEN_WORK}/*witness-weave*.dll" "${ZEN_WORK}/*witness-weave*.so"
     "${ZEN_WORK}/${ZEN_CONFIG}/*witness-weave*.dll")
if(NOT weave_artifact)
    message(STATUS
        "package witness: no weave artifact -- this install has no loom::kernel, so the "
        "dynamic-weave half is DECLARED ABSENT rather than passed. The macro surface "
        "above did run.")
    return()
endif()
list(GET weave_artifact 0 weave)

# ---- 2. what does the image ACTUALLY export? ----------------------------------------
#
# Tooling follows the platform; the question does not.
set(exports "")
if(WIN32)
    find_program(ZEN_DUMPBIN dumpbin)
    find_program(ZEN_OBJDUMP objdump)
    if(ZEN_DUMPBIN)
        execute_process(COMMAND "${ZEN_DUMPBIN}" /exports "${weave}"
                        OUTPUT_VARIABLE exports RESULT_VARIABLE rc ERROR_QUIET)
        set(tool "dumpbin /exports")
    elseif(ZEN_OBJDUMP)
        execute_process(COMMAND "${ZEN_OBJDUMP}" -p "${weave}"
                        OUTPUT_VARIABLE exports RESULT_VARIABLE rc ERROR_QUIET)
        set(tool "objdump -p")
    else()
        message(FATAL_ERROR
            "package witness: no PE export inspector (dumpbin or objdump) on PATH, so what "
            "${weave} exports cannot be read. The exported symbol is the whole thing the host "
            "needs; this fails rather than skips.")
    endif()
else()
    find_program(ZEN_NM nm)
    if(NOT ZEN_NM)
        message(FATAL_ERROR
            "package witness: no nm on PATH, so what ${weave} exports cannot be read. This "
            "fails rather than skips.")
    endif()
    execute_process(COMMAND "${ZEN_NM}" -D --defined-only "${weave}"
                    OUTPUT_VARIABLE exports RESULT_VARIABLE rc ERROR_QUIET)
    set(tool "nm -D --defined-only")
endif()
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "package witness: ${tool} failed on ${weave} (exit ${rc})")
endif()

# The EXACT spelling, on a word boundary, so a decorated neighbour cannot satisfy it.
if(NOT exports MATCHES "(^|[^A-Za-z0-9_?@])zen_weave_abi([^A-Za-z0-9_@]|$)")
    message(FATAL_ERROR
        "package witness: ${weave} does not export the exact symbol `zen_weave_abi`.\n"
        "The Kernel looks this name up as a literal string, so an artifact that exports a "
        "decorated variant instead is not loadable no matter how it compiled.\n"
        "--- ${tool} ---\n${exports}")
endif()
# And it must not ALSO be present in a mangled form, which would mean the C linkage broke
# and something else happens to be spelled correctly.
if(exports MATCHES "\\?zen_weave_abi@")
    message(FATAL_ERROR
        "package witness: ${weave} exports a C++-MANGLED zen_weave_abi. The entry point lost "
        "its C language linkage.\n--- ${tool} ---\n${exports}")
endif()
message(STATUS "package witness: ${weave} exports exactly `zen_weave_abi` (via ${tool}) ok")

# ---- 3. the real Kernel: load, exercise both directions, unload ----------------------
find_program(ZEN_WITNESS_HOST witness-host PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_HOST)
    message(FATAL_ERROR "package witness: witness-host was not produced in ${ZEN_WORK}")
endif()
zen_run("real Kernel load/exercise/unload" "${ZEN_WITNESS_HOST}")

message(STATUS "package witness: PASSED -- installed package, macro surface, exact export, live weave")
