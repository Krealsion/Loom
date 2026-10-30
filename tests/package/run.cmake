# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The installed-package witness's driver: configures, builds and runs tests/package as a stranger
# against an installed Loom, and asks the produced artifact what it exports. CI and a person run
# the same command, so a hosted lane and a local check mean the same thing.
#   cmake -DZEN_PREFIX=<install prefix> -DZEN_WORK=<scratch dir> [-DZEN_CONFIG=Debug]
#         [-DZEN_CMAKE_ARGS=...] -P tests/package/run.cmake

# A weave that loads proves only that some spelling of its entry point was found. The kernel
# looks up the literal "zen_weave_abi" (src/kernel/kernel.cpp), so the export table is asked for
# exactly that, and not a decorated form. A missing inspection tool is a failure, never a skip.

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

# ---- configure / build, with nothing added to the consumer's flags ------------------
# ZEN_CMAKE_ARGS carries only what selects a toolchain, never a compile option: needing one would
# mean the package stopped carrying its requirements. The generator is chosen, not assumed:
# -DZEN_GENERATOR wins, else Ninja when it is on PATH, else CMake's platform default.
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

# ---- 1b. the terminal core, as a second presentation sees it ------------------------
find_program(ZEN_WITNESS_TERMINAL witness-terminal PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_TERMINAL)
    message(FATAL_ERROR
        "package witness: witness-terminal was not produced in ${ZEN_WORK}. The terminal core "
        "is exported precisely so a presentation outside this tree can build one; if it no "
        "longer configures or compiles from the installed package, that claim is false.")
endif()
zen_run("terminal core as a stranger sees it" "${ZEN_WITNESS_TERMINAL}")

# ---- 1c. the history pair, as an application outside this repository reaches it ------
find_program(ZEN_WITNESS_HISTORY witness-history PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_HISTORY)
    message(FATAL_ERROR
        "package witness: witness-history was not produced in ${ZEN_WORK}. The recorder and the "
        "logger are exported precisely so an application outside this tree can hold both; if "
        "they no longer configure or compile from the installed package, that claim is false.")
endif()
zen_run("history pair as a stranger sees it" "${ZEN_WITNESS_HISTORY}")

# ---- 1d. the crossing, as a linking host reaches it ---------------------------------
find_program(ZEN_WITNESS_BRIDGE witness-bridge PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_BRIDGE)
    message(FATAL_ERROR
        "package witness: witness-bridge was not produced in ${ZEN_WORK}. The crossing is "
        "exported precisely so a host outside this tree can link to another; if it no longer "
        "configures or compiles from the installed package, that claim is false.")
endif()
zen_run("the crossing as a linking host sees it" "${ZEN_WITNESS_BRIDGE}")

# ---- 1e. the decode allowance, as a consumer sizing its own data reads it ------------
find_program(ZEN_WITNESS_DECODE witness-decode PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_DECODE)
    message(FATAL_ERROR
        "package witness: witness-decode was not produced in ${ZEN_WORK}. The decode allowance "
        "is declared in an installed header precisely so a consumer can size its own data "
        "against it; if it no longer compiles from the installed package, that claim is false.")
endif()
zen_run("the decode allowance as a consumer reads it" "${ZEN_WITNESS_DECODE}")

# ---- 1f. the deciding half, as a second host keeps its person's decisions ----------------
find_program(ZEN_WITNESS_AUTHORITY witness-authority PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_AUTHORITY)
    message(FATAL_ERROR
        "package witness: witness-authority was not produced in ${ZEN_WORK}. The authority store, "
        "its warden and the `authority` command are exported precisely so a second host can keep "
        "its person's decisions in Loom's store; if they no longer configure or compile from the "
        "installed package, that claim is false.")
endif()
zen_run("a second host's decisions in Loom's authority store" "${ZEN_WITNESS_AUTHORITY}"
        "${ZEN_WORK}/authority")

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

# ---- 2b. on Windows, the weave carries its own C++ runtime --------------------------
# Built through the installed contract, it imports no C++ runtime library (KERN-05): read from its
# import list, which `objdump -p` printed above and `dumpbin /dependents` prints.
if(WIN32)
    if(ZEN_DUMPBIN)
        execute_process(COMMAND "${ZEN_DUMPBIN}" /dependents "${weave}"
                        OUTPUT_VARIABLE imports RESULT_VARIABLE rc ERROR_QUIET)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "package witness: dumpbin /dependents failed on ${weave} (exit ${rc})")
        endif()
    else()
        set(imports "${exports}")
    endif()
    string(TOLOWER "${imports}" imports)
    if(imports MATCHES "(libstdc\\+\\+|libgcc_s|libwinpthread|libc\\+\\+|libunwind|vcruntime|msvcp|ucrtbased)[^ \t\r\n]*\\.dll")
        message(FATAL_ERROR
            "package witness: ${weave} imports its C++ runtime (${CMAKE_MATCH_0}) although it was "
            "built through the installed weave build contract, so the runtime it runs with is "
            "whichever copy the machine or the host holds.\n--- imports ---\n${imports}")
    endif()
    message(STATUS "package witness: ${weave} imports no C++ runtime library ok")
    # A MinGW-w64 install carries what its contract links into every weave to return that
    # runtime's indexes at unload (loom-weave-tls-return.cpp, beside loom-weave.cmake); its wrapped
    # import slots are then in the weave's symbol table.
    file(GLOB tls_return "${ZEN_PREFIX}/*/cmake/loom/loom-weave-tls-return.cpp")
    if(tls_return)
        if(NOT ZEN_OBJDUMP)
            message(FATAL_ERROR
                "package witness: this is a MinGW-w64 install and no objdump is on PATH, so "
                "whether ${weave} returns its runtime's indexes at unload cannot be read. This "
                "fails rather than skips.")
        endif()
        execute_process(COMMAND "${ZEN_OBJDUMP}" -t "${weave}"
                        OUTPUT_VARIABLE symbols RESULT_VARIABLE rc ERROR_QUIET)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "package witness: objdump -t failed on ${weave} (exit ${rc})")
        endif()
        foreach(slot IN ITEMS TlsAlloc TlsFree FlsAlloc FlsFree)
            if(NOT symbols MATCHES " __wrap___imp_${slot}\r?\n")
                message(FATAL_ERROR
                    "package witness: ${weave} was built through the installed weave build "
                    "contract and does not define __wrap___imp_${slot}, so what its runtime takes "
                    "is not returned when it unloads, and a process that reloads it runs out of "
                    "indexes.")
            endif()
        endforeach()
        message(STATUS "package witness: ${weave} returns its runtime's TLS and FLS indexes at "
                       "unload ok")
    endif()
endif()

# ---- 3. the real Kernel: load, exercise both directions, unload ----------------------
find_program(ZEN_WITNESS_HOST witness-host PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(NOT ZEN_WITNESS_HOST)
    message(FATAL_ERROR "package witness: witness-host was not produced in ${ZEN_WORK}")
endif()
zen_run("real Kernel load/exercise/unload" "${ZEN_WITNESS_HOST}")

# ---- 3b. the same weave built as a module by the installed loom_wasm_weave() ----------------
# Where the package builds modules (its loom-wasm.cmake), the boxed host was built when a wasi-sdk
# was named, and says in words that it was not otherwise (tests/package/CMakeLists.txt).
set(zen_boxed_said "")
file(GLOB zen_wasm_package "${ZEN_PREFIX}/*/cmake/loom/loom-wasm.cmake")
find_program(ZEN_WITNESS_HOST_BOX witness-host-box PATHS "${ZEN_WORK}" "${ZEN_WORK}/${ZEN_CONFIG}"
             NO_DEFAULT_PATH)
if(ZEN_WITNESS_HOST_BOX)
    if(NOT zen_wasm_package)
        message(FATAL_ERROR
            "package witness: a boxed host was built against a package that installs no "
            "loom-wasm.cmake")
    endif()
    zen_run("the stranger's weave as a module, built from the package and boxed"
            "${ZEN_WITNESS_HOST_BOX}")
    set(zen_boxed_said ", the same weave boxed")
elseif(zen_wasm_package)
    message(STATUS "package witness: the package builds WebAssembly modules; no wasi-sdk was "
                   "named, so the boxed half is DECLARED ABSENT")
endif()

# ---- 4. on Windows, the installed host runs in the UTF-8 code page ----------------------
# Its manifest makes every narrow path it holds UTF-8 (cmake/loom-code-page.cmake); the installed
# bytes are read, since the installed host is the one a stranger runs.
set(zen_host_said "")
if(CMAKE_HOST_WIN32)
    set(zen_host "${ZEN_PREFIX}/bin/loom-host.exe")
    if(NOT EXISTS "${zen_host}")
        message(FATAL_ERROR "package witness: no installed host at ${zen_host}")
    endif()
    file(STRINGS "${zen_host}" zen_code_page REGEX "activeCodePage[^>]*>UTF-8<")
    if(NOT zen_code_page)
        message(FATAL_ERROR
            "package witness: the installed ${zen_host} carries no UTF-8 active code page, so it "
            "reads every narrow path in the system's code page and cannot name a folder outside it")
    endif()
    message(STATUS "package witness: the installed loom-host runs in the UTF-8 code page ok")
    set(zen_host_said ", installed host in the UTF-8 code page")
endif()

message(STATUS "package witness: PASSED -- installed package, macro surface, a second host's "
               "decisions, exact export, live weave${zen_boxed_said}${zen_host_said}")
