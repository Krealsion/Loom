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

# ---- guard 1: the selector must actually select something ------------------------

set(select_args "")
set(selection "everything registered")
if(DEFINED ZEN_SELECT AND NOT ZEN_SELECT STREQUAL "")
    set(select_args -R "${ZEN_SELECT}")
    set(selection "-R ${ZEN_SELECT}")
endif()

execute_process(
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${ZEN_BUILD_DIR}" -N ${select_args}
    OUTPUT_VARIABLE listing RESULT_VARIABLE list_rc)
if(NOT list_rc EQUAL 0)
    message(FATAL_ERROR "verify: could not list the selected tests (exit ${list_rc}).\n${listing}")
endif()
if(NOT listing MATCHES "Total Tests: ([0-9]+)")
    message(FATAL_ERROR "verify: could not read a test count out of `ctest -N`.\n${listing}")
endif()
set(selected "${CMAKE_MATCH_1}")
if(selected EQUAL 0)
    message(FATAL_ERROR
        "verify: the selector (${selection}) matched ZERO registered tests. CTest would "
        "have printed \"No tests were found!!!\" and exited 0; this lane calls that what it "
        "is -- a question that was never asked, not an answer. Check the selector, or check "
        "whether the entries it names still exist.")
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
