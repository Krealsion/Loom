# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `message_journey` entry: the record examples/message_journey.cpp prints (each journey under
# `== <name> ==`) is the one docs/guides/message-journey.md shows (under `<!-- journey: <name> -->`,
# in the ```text block after it), compared both ways.
#   cmake -DZEN_JOURNEY_EXE=<message_journey> -DZEN_JOURNEY_PAGE=<page> -P tests/message_journey.cmake

cmake_minimum_required(VERSION 3.16)

foreach(v ZEN_JOURNEY_EXE ZEN_JOURNEY_PAGE)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "message_journey: pass -D${v}=<path>")
    endif()
endforeach()

# A text as a list of lines. The characters a CMake list reads as its own are held aside first
# and given back by zen_journey_restore, so a line keeps what it says.
string(ASCII 1 ZEN_SEMI)
string(ASCII 2 ZEN_OPEN)
string(ASCII 3 ZEN_CLOSE)
function(zen_journey_lines text out)
    string(REPLACE "\r" "" t "${text}")
    string(REPLACE ";" "${ZEN_SEMI}" t "${t}")
    string(REPLACE "[" "${ZEN_OPEN}" t "${t}")
    string(REPLACE "]" "${ZEN_CLOSE}" t "${t}")
    string(REPLACE "\n" ";" t "${t}")
    set(${out} "${t}" PARENT_SCOPE)
endfunction()
function(zen_journey_restore text out)
    string(REPLACE "${ZEN_SEMI}" ";" t "${text}")
    string(REPLACE "${ZEN_OPEN}" "[" t "${t}")
    string(REPLACE "${ZEN_CLOSE}" "]" t "${t}")
    set(${out} "${t}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND "${ZEN_JOURNEY_EXE}"
                OUTPUT_VARIABLE printed
                ERROR_VARIABLE errors
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "message_journey: the example exited ${rc}: a journey did not reach its "
                        "end.\n${printed}${errors}")
endif()

# ---- what the program printed, journey by journey ---------------------------------------
zen_journey_lines("${printed}" lines)
set(printed_names "")
set(current "")
foreach(line IN LISTS lines)
    if(line MATCHES "^== ([a-z]+) ==$")
        set(current "${CMAKE_MATCH_1}")
        list(APPEND printed_names "${current}")
        set(printed_${current} "")
    elseif(NOT current STREQUAL "" AND NOT line STREQUAL "")
        string(APPEND printed_${current} "${line}\n")
    endif()
endforeach()
if(printed_names STREQUAL "")
    message(FATAL_ERROR "message_journey: the example printed no journey\n${printed}")
endif()

# ---- what the page shows, block by block -------------------------------------------------
file(READ "${ZEN_JOURNEY_PAGE}" page)
zen_journey_lines("${page}" lines)
set(shown_names "")
set(pending "")
set(capturing "")
foreach(line IN LISTS lines)
    if(NOT capturing STREQUAL "")
        if(line STREQUAL "```")
            set(capturing "")
        else()
            string(APPEND shown_${capturing} "${line}\n")
        endif()
    elseif(line MATCHES "^<!-- journey: ([a-z]+) -->$")
        set(pending "${CMAKE_MATCH_1}")
    elseif(NOT pending STREQUAL "" AND line STREQUAL "```text")
        set(capturing "${pending}")
        set(pending "")
        list(APPEND shown_names "${capturing}")
        set(shown_${capturing} "")
    elseif(NOT pending STREQUAL "" AND NOT line STREQUAL "")
        message(FATAL_ERROR "message_journey: the page names journey '${pending}' and no "
                            "```text block follows it")
    endif()
endforeach()

# ---- the two, compared both ways ---------------------------------------------------------
set(failures 0)
foreach(name IN LISTS printed_names)
    if(NOT name IN_LIST shown_names)
        message(SEND_ERROR "message_journey: the example prints journey '${name}' and the page "
                           "shows no block for it")
        math(EXPR failures "${failures} + 1")
    elseif(NOT printed_${name} STREQUAL shown_${name})
        zen_journey_restore("${printed_${name}}" want)
        zen_journey_restore("${shown_${name}}" got)
        message(SEND_ERROR "message_journey: the page's '${name}' block is not what the example "
                           "prints.\n--- the example prints:\n${want}--- the page shows:\n${got}")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()
foreach(name IN LISTS shown_names)
    if(NOT name IN_LIST printed_names)
        message(SEND_ERROR "message_journey: the page shows journey '${name}' and the example "
                           "no longer prints it")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()
if(failures GREATER 0)
    message(FATAL_ERROR "message_journey: ${failures} journey(s) differ")
endif()
list(LENGTH printed_names count)
message(STATUS "message_journey: the page shows every journey the example prints (${count})")
