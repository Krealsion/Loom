# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE OFFICIAL LOCAL VERIFICATION LANE (POP-01 / POP-03, R2F-D).
#
# Run this, not a bare `ctest`, when a result is going to be quoted as evidence:
#
#   cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake
#   cmake -DZEN_BUILD_DIR=build -DZEN_SELECT="^policy$" -P tests/verify.cmake
#
# On a MULTI-CONFIGURATION build tree (Ninja Multi-Config, Visual Studio, Xcode) the
# configuration has to be named, because the tree holds several of them:
#
#   cmake -DZEN_BUILD_DIR=build-mc -DZEN_BUILD_CONFIG=Debug -P tests/verify.cmake
#
# It exists because CTest's own default is to treat "I selected zero tests" as success:
#
#   $ ctest -R "^no_such_suite$"
#   No tests were found!!!
#   $ echo $?
#   0
#
# A verification path that answers 0 to a question it never asked is the same lie as a
# suite that passes having run nothing (F-2), one layer up. This wrapper closes it twice
# over, on purpose -- the two guards fail independently:
#
#   1. it asks `ctest -N` how many entries the selector matched, and refuses a zero;
#   2. it passes --no-tests=error, so CTest itself refuses a zero as well.
#
# CMake's own semantics are untouched: this is a project-owned lane, not a patch to CTest.
#
# It also refuses to run under the OS-enforcement opt-out. `ZEN_ALLOW_UNENFORCEABLE=1`
# has a real portability purpose (a host that genuinely cannot enforce), but a run made
# under it is NOT evidence that the enforcement population executed -- so the lane whose
# whole job is to produce quotable evidence will not produce it in that mode. Run a bare
# `ctest` for that; it will say NON-ENFORCEMENT MODE in its own output.
#
# R2F-D deliberately does NOT automate this (F-21 / CI is R2F-G). First make the
# measurement truthful; then automate the truthful measurement.

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

# ---- which configuration is being verified? (QR-0) -------------------------------
#
# A MULTI-CONFIGURATION build tree holds Debug, Release and RelWithDebInfo side by side, and
# CTest has to be told which one: without `-C` it reports every test `Not Run` -- 29 of 29 on
# the tree this was measured against, while `ctest -C Debug` on the same tree passed 29 of 29.
# A SINGLE-CONFIGURATION tree has exactly one, and asking a caller to name it would be asking
# them to repeat what the tree already says.
#
# The build tree answers which kind it is. CMAKE_CONFIGURATION_TYPES is written into the cache
# by multi-config generators and by no others; CMAKE_BUILD_TYPE is the single-config record.
# This lane reads that and NEVER picks a configuration on a caller's behalf -- not Debug, not
# the most recently built, not the first in the list. A verifier that chose would be minting
# evidence about a configuration nobody asked about, which is the same class of lie as
# answering a question that was never asked.

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

# ---- ONE configuration authority (QR-1) ------------------------------------------
#
# ZEN_CTEST_ARGS is appended to the ctest command below, after the configuration this lane
# validated. CTest takes the LAST configuration argument it is given -- measured on CTest
# 4.1.0: `-C Debug -C Release` runs Release, `-C Release -C Debug` runs Debug -- so a
# configuration smuggled in here would silently replace the validated one. Measured before
# this guard existed: `-DZEN_BUILD_CONFIG=Debug -DZEN_CTEST_ARGS=-C;Release` reported
# `PASSED ... in configuration Debug` at exit 0 while every one of the 29 entries ran out of
# tests/Release/. The lane named one configuration and proved another.
#
# Argument ORDER is not the fix. Putting ours last would work only until somebody appended
# something after it, and it would leave two things able to choose while only one of them is
# checked. The rule is that there is one authority, so a second one is refused rather than
# out-ranked -- including when the two agree, because a lane that happens to be right is not
# the same as a lane that cannot be wrong.
#
# The spellings are CTest's own, read out of `ctest --help` rather than guessed:
#
#   -C <cfg>              -C is the ONLY option in ctest's -C namespace, so anything
#                         starting with -C is a configuration argument (the joined -CDebug
#                         is rejected by ctest itself today; refusing it costs nothing and
#                         does not depend on that staying true)
#   --build-config <cfg>
#   --build-config=<cfg>
#
# `--build-config-sample` is a DIFFERENT flag (it belongs to --build-and-test) and is
# deliberately not caught: this refuses configuration selection, not a flag namespace.

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
#
# A FULL run stamps itself with a token, exports it to the ctest invocation at the bottom of
# this file, and clears any earlier inventory receipt out of the build tree. The CTest-entry
# inventory below writes a receipt for this token once it has actually taken the inventory;
# the `population` entry, running inside that ctest invocation, refuses to pass a run that
# carries the token and has no matching receipt (VOLATILE-B1).
#
# That is deliberately not beside the call it watches. A lane cannot check itself -- the
# first attempt tried, by having the `population` entry grep this file for the inventory's
# NAME, and the sentence you are reading would have satisfied it. What the entry can check is
# what this lane actually DID, and a run that stopped taking the inventory does no work to
# leave behind.
#
# A SUBSET RUN OWES NO RECEIPT: it deliberately does not assert the whole-lane identity
# contract (see below), so it stamps no token and the entry, if the subset includes it, says
# the lane's half was not asked rather than failing for a receipt this run never owed.

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
#
# WHY THIS RUNS HERE AS WELL AS IN AN ENTRY (VOLATILE-2a, VOLATILE-B1). The suite manifest
# has always pinned which doctest suites exist. Nothing pinned the CTest entries that are NOT
# suites -- the population checks, the empty-population witnesses, the weave-artifact checks,
# the documentation-link check, the aggregate runner -- so deleting one `add_test` left the
# lane registering one fewer entry, running everything that remained, and reporting green at
# a smaller number. Including for entries whose entire job is to notice absences.
#
# An entry alone could not close that: an entry that has been deleted cannot complain about
# its own deletion, so this door -- outside the population it inventories -- is what notices
# a `population` that is gone. And a lane alone could not close it either, which is why the
# `population` entry now asks the same question independently rather than reading this file.
# Two doors, one implementation, one authored expectation.
#
# It is also on this lane's critical path on purpose: zen_check_entry_population() owns the
# count and the zero-refusal everything below depends on, so removing the call leaves a lane
# with no answer rather than a lane that quietly runs less.

zen_ctest_entry_listing("${ZEN_BUILD_DIR}" "${config_selected}" "${ZEN_SELECT}" listing)
zen_check_entry_population("${listing}" "${ZEN_SELECT}" "${ZEN_BUILD_DIR}" "${run_token}" selected)

if(NOT DEFINED selected OR selected STREQUAL "")
    message(FATAL_ERROR
        "verify: the CTest-entry inventory did not answer. This lane does not count its own "
        "tests -- zen_check_entry_population() in tests/check_entry_population.cmake does, "
        "because the count and the declared-versus-registered comparison are one question. "
        "A lane that proceeded without it would be the lane VOLATILE-2a closed: one that "
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
