# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The official verification lane (POP-01, POP-03). Run this, not a bare `ctest`, when a result
# is to be quoted: CTest treats "I selected zero tests" as success (`ctest -R "^none$"` exits 0),
# and this lane refuses a zero twice, independently: by counting `ctest -N`'s selection, and by
# passing --no-tests=error. It refuses to run under the OS-enforcement opt-out
# (ZEN_ALLOW_UNENFORCEABLE=1), whose runs are not enforcement evidence; a bare `ctest` serves them.

#   cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake
#   cmake -DZEN_BUILD_DIR=build -DZEN_SELECT="^policy$" -P tests/verify.cmake
#   cmake -DZEN_BUILD_DIR=build-mc -DZEN_BUILD_CONFIG=Debug -P tests/verify.cmake  (multi-config)

cmake_minimum_required(VERSION 3.18) # --no-tests=error

if(NOT DEFINED ZEN_BUILD_DIR)
    message(FATAL_ERROR "verify: -DZEN_BUILD_DIR=<build dir> is required")
endif()
if(NOT EXISTS "${ZEN_BUILD_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "verify: '${ZEN_BUILD_DIR}' is not a configured CTest build directory "
        "(no CTestTestfile.cmake). Configure and build first.")
endif()
if(NOT EXISTS "${ZEN_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "verify: '${ZEN_BUILD_DIR}' has a CTestTestfile.cmake but no CMakeCache.txt, so it is "
        "a subdirectory of a build tree rather than the top of one. The lane needs the top: "
        "the cache is where the build records how many configurations it has, and a "
        "subdirectory of a multi-configuration tree would look single-configuration from "
        "here. Point ZEN_BUILD_DIR at the directory you configured.")
endif()

# ---- which configuration is being verified? -------------------------------
# A multi-configuration tree holds several, and CTest without `-C` reports every test Not Run; a
# single-configuration tree has one. The tree says which kind it is (CMAKE_CONFIGURATION_TYPES is
# cached by multi-config generators only, CMAKE_BUILD_TYPE by single-config ones), and this lane
# never picks a configuration on a caller's behalf: that would mint evidence nobody asked for.

file(READ "${ZEN_BUILD_DIR}/CMakeCache.txt" cache_text)
string(REPLACE "\r" "" cache_text "${cache_text}")
set(cache_config_types "")
if("\n${cache_text}" MATCHES "\nCMAKE_CONFIGURATION_TYPES:[A-Za-z]+=([^\n]*)")
    set(cache_config_types "${CMAKE_MATCH_1}")
endif()
set(cache_build_type "")
if("\n${cache_text}" MATCHES "\nCMAKE_BUILD_TYPE:[A-Za-z]+=([^\n]*)")
    set(cache_build_type "${CMAKE_MATCH_1}")
endif()
if(NOT DEFINED ZEN_BUILD_CONFIG)
    set(ZEN_BUILD_CONFIG "")
endif()

set(config_args "")      # -C <config>, or nothing at all on a single-config tree
set(config_note "")      # what every result line says it is about
set(config_selected "")  # what the entry inventory is asked about

if(NOT cache_config_types STREQUAL "")
    string(REPLACE ";" ", " config_list "${cache_config_types}")
    if(ZEN_BUILD_CONFIG STREQUAL "")
        message(FATAL_ERROR
            "verify: '${ZEN_BUILD_DIR}' is a MULTI-CONFIGURATION build tree -- it holds "
            "${config_list} side by side -- so the configuration to verify has to be named. "
            "CTest given no configuration reports every test `Not Run`, and this lane will "
            "not choose one for you: a result has to say which configuration it is about.\n"
            "  cmake -DZEN_BUILD_DIR=${ZEN_BUILD_DIR} -DZEN_BUILD_CONFIG=<name> "
            "-P tests/verify.cmake\n"
            "Build that configuration first (`cmake --build ${ZEN_BUILD_DIR} --config "
            "<name>`) -- a configuration that was configured and never built is not evidence "
            "either.")
    endif()
    # Case-insensitively, because CTest's own generated configuration guards are, and then
    # forward the spelling the build tree uses rather than the caller's -- so every line
    # below names the configuration the way the tree does.
    string(TOUPPER "${ZEN_BUILD_CONFIG}" wanted)
    foreach(candidate IN LISTS cache_config_types)
        string(TOUPPER "${candidate}" candidate_upper)
        if(candidate_upper STREQUAL wanted)
            set(config_selected "${candidate}")
        endif()
    endforeach()
    if(config_selected STREQUAL "")
        message(FATAL_ERROR
            "verify: '${ZEN_BUILD_CONFIG}' is not a configuration of '${ZEN_BUILD_DIR}'. That "
            "tree was configured with exactly these: ${config_list} "
            "(CMAKE_CONFIGURATION_TYPES). Running it anyway would select nothing and report "
            "every test `Not Run`, which reads like a broken build rather than a mistyped "
            "argument.")
    endif()
    set(config_args -C "${config_selected}")
    set(config_note " in configuration ${config_selected}")
elseif(NOT ZEN_BUILD_CONFIG STREQUAL "")
    # Single-config tree, and a configuration was named anyway. If it is the one the tree was
    # configured as, that is merely redundant. If it is a different one, the caller believes
    # they are selecting something, and running Debug while they asked for Release is exactly
    # the misreported evidence this lane exists to refuse.
    if(NOT ZEN_BUILD_CONFIG STREQUAL "${cache_build_type}")
        message(FATAL_ERROR
            "verify: -DZEN_BUILD_CONFIG=${ZEN_BUILD_CONFIG} was given, but '${ZEN_BUILD_DIR}' "
            "is a SINGLE-CONFIGURATION build tree configured as '${cache_build_type}' "
            "(CMAKE_BUILD_TYPE). There is nothing here to select, and verifying "
            "'${cache_build_type}' while the caller asked for '${ZEN_BUILD_CONFIG}' would "
            "misname the evidence. Configure a '${ZEN_BUILD_CONFIG}' tree, or drop "
            "ZEN_BUILD_CONFIG -- a single-configuration tree does not need it.")
    endif()
    set(config_note " in configuration ${cache_build_type}")
endif()

# ---- one configuration authority ------------------------------------------
# ZEN_CTEST_ARGS is appended after the configuration this lane validated, and CTest takes the last
# configuration argument it is given, so one passed through it would silently replace the
# validated one. A second authority is refused, even when it agrees. The spellings are CTest's own:
# anything starting with -C (the only option in that namespace), `--build-config <cfg>` and
# `--build-config=<cfg>`; `--build-config-sample` belongs to --build-and-test and is not one.

foreach(passthrough IN LISTS ZEN_CTEST_ARGS)
    if(passthrough MATCHES "^-C"
       OR passthrough STREQUAL "--build-config"
       OR passthrough MATCHES "^--build-config=")
        message(FATAL_ERROR
            "verify: ZEN_CTEST_ARGS carries '${passthrough}', which selects the CTest "
            "configuration. ZEN_BUILD_CONFIG owns that choice: this lane validates it "
            "against the build tree and reports it with every result, and CTest would take "
            "whichever configuration came last. ZEN_CTEST_ARGS is for arguments that do not "
            "change WHICH configured artifact is being verified -- parallelism, timeouts, "
            "output verbosity. Pass the configuration as -DZEN_BUILD_CONFIG=<name> instead. "
            "This is refused even when the two agree: one authority, not two that happen to.")
    endif()
endforeach()

# ---- an opt-out run is not an acceptance run -------------------------------------

set(optout "")
if(DEFINED ENV{ZEN_ALLOW_UNENFORCEABLE} AND "$ENV{ZEN_ALLOW_UNENFORCEABLE}" STREQUAL "1")
    set(optout "ZEN_ALLOW_UNENFORCEABLE=1")
elseif(DEFINED ENV{ZEN_REQUIRE_ENFORCEMENT} AND "$ENV{ZEN_REQUIRE_ENFORCEMENT}" STREQUAL "0")
    set(optout "ZEN_REQUIRE_ENFORCEMENT=0")
endif()
if(NOT optout STREQUAL "")
    message(FATAL_ERROR
        "verify: ${optout} is set, which converts every OS-enforcement proof into a "
        "marked-degraded skip. A run in that mode is NON-ENFORCEMENT MODE: it proves the "
        "rest of the tree, and it proves nothing whatsoever about containment. This lane "
        "will not mint it as evidence. Unset the variable to run the real lane, or run "
        "`ctest` directly and read its NON-ENFORCEMENT MODE banner.")
endif()

# ---- which run is this, and what does it owe? ------------------------------------
# A full run stamps itself with a token, exports it to the ctest invocation below and clears any
# earlier receipt; the entry inventory writes a receipt for this token once it has done the work,
# and the `population` entry refuses a run that carries the token and shows none
# (docs/laws/population-laws.md). A subset run does not assert the whole-lane contract, so it
# stamps no token and owes no receipt.

set(select_args "")
set(selection "everything registered")
if(NOT DEFINED ZEN_SELECT)
    set(ZEN_SELECT "")
endif()
if(NOT ZEN_SELECT STREQUAL "")
    set(select_args -R "${ZEN_SELECT}")
    set(selection "-R ${ZEN_SELECT}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/check_entry_population.cmake")

set(run_token "")
if(ZEN_SELECT STREQUAL "")
    string(TIMESTAMP run_stamp "%Y%m%dT%H%M%S" UTC)
    string(RANDOM LENGTH 16 ALPHABET "0123456789abcdef" run_suffix)
    set(run_token "${run_stamp}-${run_suffix}")
    # A leftover receipt from an earlier run must not be able to answer for this one. The
    # token already makes that so; removing the file as well means freshness does not rest on
    # the uniqueness of a random string alone.
    zen_entry_witness_file("${ZEN_BUILD_DIR}" stale_witness)
    file(REMOVE "${stale_witness}")
endif()

# ---- guard 1: the selection must be the one this repository declared -------------
# Taken here, outside the population it inventories, because a deleted entry cannot complain
# about its own deletion: this door notices a `population` that is gone, and that entry asks the
# same question independently. zen_check_entry_population() owns the count and the zero-refusal
# everything below depends on, so removing the call leaves a lane with no answer rather than one
# that quietly runs less.

zen_ctest_entry_listing("${ZEN_BUILD_DIR}" "${config_selected}" "${ZEN_SELECT}" listing)
zen_check_entry_population("${listing}" "${ZEN_SELECT}" "${ZEN_BUILD_DIR}" "${run_token}" selected)

if(NOT DEFINED selected OR selected STREQUAL "")
    message(FATAL_ERROR
        "verify: the CTest-entry inventory did not answer. This lane does not count its own "
        "tests -- zen_check_entry_population() in tests/check_entry_population.cmake does, "
        "because the count and the declared-versus-registered comparison are one question. "
        "A lane that proceeded without it would be one that "
        "runs whatever is left and calls the smaller number green.")
endif()
message(STATUS "verify: ${selected} CTest entries selected (${selection})${config_note}")

# ---- guard 2: run them, and let CTest refuse a zero too --------------------------
#
# ZEN_ENTRY_INVENTORY_RUN is this run announcing itself to the entries inside it. CTest hands
# its own environment to every test, so the `population` entry sees it and knows to demand
# the receipt guard 1 leaves.

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "ZEN_ENTRY_INVENTORY_RUN=${run_token}"
            ${CMAKE_CTEST_COMMAND} --test-dir "${ZEN_BUILD_DIR}" ${config_args}
            --no-tests=error --output-on-failure ${select_args} ${ZEN_CTEST_ARGS}
    RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR
        "verify: FAILED (ctest exit ${run_rc}) over ${selected} selected entries${config_note}")
endif()

message(STATUS
    "verify: PASSED -- ${selected} CTest entries selected and executed "
    "(${selection})${config_note}")
