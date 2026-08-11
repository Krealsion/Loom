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

# ---- guard 1: the selection must be the one this repository declared -------------
#
# WHY THIS LIVES IN THE LANE AND NOT IN A CTEST ENTRY (VOLATILE-2a). The suite manifest has
# always pinned which doctest suites exist. Nothing pinned the CTest entries that are NOT
# suites -- the population checks, the empty-population witnesses, the weave-artifact
# checks, the documentation-link check, the aggregate runner -- so deleting one `add_test`
# left the lane registering one fewer entry, running everything that remained, and
# reporting green at a smaller number. Including for entries whose entire job is to notice
# absences.
#
# A CTest entry could not have closed that: an entry that has been deleted cannot complain
# about its own deletion. So the inventory runs here, outside the population it counts, for
# the same reason the population check's lane does.
#
# ...AND IT IS ON THIS LANE'S CRITICAL PATH ON PURPOSE. zen_check_entry_population() owns
# the count and the zero-refusal that everything below depends on, so removing the call
# does not leave a lane that still runs -- it leaves a lane with no answer, and the guard
# under it says so. The other half of the lock is in check_population.cmake: the
# `population` entry (which this inventory keeps registered) requires this file to still
# call the function. Neither can be deleted alone without the other saying so.

set(select_args "")
set(selection "everything registered")
if(NOT DEFINED ZEN_SELECT)
    set(ZEN_SELECT "")
endif()
if(NOT ZEN_SELECT STREQUAL "")
    set(select_args -R "${ZEN_SELECT}")
    set(selection "-R ${ZEN_SELECT}")
endif()

execute_process(
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${ZEN_BUILD_DIR}" -N ${select_args}
    OUTPUT_VARIABLE listing RESULT_VARIABLE list_rc)
if(NOT list_rc EQUAL 0)
    message(FATAL_ERROR "verify: could not list the selected tests (exit ${list_rc}).\n${listing}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/check_entry_population.cmake")
zen_check_entry_population("${listing}" "${ZEN_SELECT}" "${ZEN_BUILD_DIR}" selected)

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

execute_process(
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${ZEN_BUILD_DIR}"
            --no-tests=error --output-on-failure ${select_args} ${ZEN_CTEST_ARGS}
    RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "verify: FAILED (ctest exit ${run_rc}) over ${selected} selected entries")
endif()

message(STATUS "verify: PASSED -- ${selected} CTest entries selected and executed (${selection})")
