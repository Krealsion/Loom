# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The documentation lane (AGENTS.md): every text check, as CTest runs it, over a change that touches
# no compiled line. It asks tests/check_change_kind.cmake first and passes only when the change is
# documentation-only and every check passed; -DZEN_LANE_KIND=OFF runs the checks alone (CI's job).
#   cmake [-DZEN_BASE=<rev>] [-DZEN_LANE_KIND=OFF] -P tests/documentation_lane.cmake

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/text_checks.cmake")
if(NOT DEFINED ZEN_LANE_KIND)
    set(ZEN_LANE_KIND ON)
endif()

set(compiled "")
if(ZEN_LANE_KIND)
    set(args -DZEN_REPO=${ZEN_REPO})
    if(DEFINED ZEN_BASE)
        list(APPEND args -DZEN_BASE=${ZEN_BASE})
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${args} -P "${CMAKE_CURRENT_LIST_DIR}/check_change_kind.cmake"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
    message("${o}${e}")
    if(NOT rc EQUAL 0 OR NOT "${o}${e}" MATCHES "change-kind: (documentation-only|compiled) --")
        message(FATAL_ERROR "documentation-lane: the change could not be classified (exit ${rc})")
    endif()
    if(CMAKE_MATCH_1 STREQUAL "compiled")
        set(compiled 1)
    endif()
endif()

set(failed "")
foreach(check IN LISTS ZEN_TEXT_CHECKS)
    string(TIMESTAMP t0 "%s")
    execute_process(COMMAND "${CMAKE_COMMAND}" -DZEN_REPO=${ZEN_REPO}
                            -P "${CMAKE_CURRENT_LIST_DIR}/check_${check}.cmake"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
    string(TIMESTAMP t1 "%s")
    math(EXPR s "${t1} - ${t0}")
    if(rc EQUAL 0)
        message(STATUS "documentation-lane: ${check} PASSED (exit 0, ${s} s)")
    else()
        message("${o}${e}")
        message(STATUS "documentation-lane: ${check} FAILED (exit ${rc}, ${s} s)")
        list(APPEND failed "${check}")
    endif()
endforeach()

list(LENGTH ZEN_TEXT_CHECKS total)
if(compiled)
    message(STATUS "documentation-lane: compiled -- this change touches compiled lines, so it is "
                   "the official lane's (tests/verify.cmake) to verify")
endif()
if(failed)
    string(REPLACE ";" ", " shown "${failed}")
    message(FATAL_ERROR "documentation-lane: FAILED -- ${shown} of the ${total} text checks refused")
endif()
if(compiled)
    message(FATAL_ERROR "documentation-lane: the ${total} text checks passed; the change is not documentation-only")
endif()
if(ZEN_LANE_KIND)
    message(STATUS "documentation-lane: PASSED -- the ${total} text checks, over a documentation-only change")
else()
    message(STATUS "documentation-lane: the ${total} text checks PASSED; the change's kind was not asked")
endif()
