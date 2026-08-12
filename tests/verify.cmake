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

zen_ctest_entry_listing("${ZEN_BUILD_DIR}" "" "${ZEN_SELECT}" listing)
zen_check_entry_population("${listing}" "${ZEN_SELECT}" "${ZEN_BUILD_DIR}" "${run_token}" selected)

if(NOT DEFINED selected OR selected STREQUAL "")
    message(FATAL_ERROR
        "verify: the CTest-entry inventory did not answer. This lane does not count its own "
        "tests -- zen_check_entry_population() in tests/check_entry_population.cmake does, "
        "because the count and the declared-versus-registered comparison are one question. "
        "A lane that proceeded without it would be the lane VOLATILE-2a closed: one that "
        "runs whatever is left and calls the smaller number green.")
endif()
message(STATUS "verify: ${selected} CTest entries selected (${selection})")

# ---- guard 2: run them, and let CTest refuse a zero too --------------------------
#
# ZEN_ENTRY_INVENTORY_RUN is this run announcing itself to the entries inside it. CTest hands
# its own environment to every test, so the `population` entry sees it and knows to demand
# the receipt guard 1 leaves.

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "ZEN_ENTRY_INVENTORY_RUN=${run_token}"
            ${CMAKE_CTEST_COMMAND} --test-dir "${ZEN_BUILD_DIR}"
            --no-tests=error --output-on-failure ${select_args} ${ZEN_CTEST_ARGS}
    RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "verify: FAILED (ctest exit ${run_rc}) over ${selected} selected entries")
endif()

message(STATUS "verify: PASSED -- ${selected} CTest entries selected and executed (${selection})")
