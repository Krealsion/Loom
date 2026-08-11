# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The population verifier (POP-01 / POP-02, R2F-D). CTest runs this as the `population`
# test; it is also the one place that reads tests/suite_population.txt.
#
# It answers the question a green run cannot answer for itself: *did the population this
# project claims to verify actually exist in the binary that was built?* It executes no
# test cases -- it takes an inventory. `--list-test-suites` and `--count` are doctest
# QUERY modes, so nothing runs and the whole check costs a fraction of a second.
#
# Usage (all arguments required except ZEN_SDL_TESTS_EXE):
#   cmake -DZEN_TESTS_EXE=<path> -DZEN_SDL_TESTS_EXE=<path-or-empty>
#         -DZEN_MANIFEST=<path> -DZEN_GATES=portable,kernel,posix
#         -P check_population.cmake
#
# ZEN_GATES is COMMA-separated on purpose: a semicolon list would be split into separate
# arguments by the command line before this script ever saw it.

cmake_minimum_required(VERSION 3.16)

foreach(required IN ITEMS ZEN_TESTS_EXE ZEN_MANIFEST ZEN_GATES)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check_population.cmake: -D${required}=... is required")
    endif()
endforeach()
if(NOT DEFINED ZEN_SDL_TESTS_EXE)
    set(ZEN_SDL_TESTS_EXE "")
endif()

string(REPLACE "," ";" active_gates "${ZEN_GATES}")

# ---- asking the binary what it actually contains ---------------------------------

function(zen_run_query exe out_var)
    execute_process(COMMAND "${exe}" ${ARGN}
                    OUTPUT_VARIABLE captured
                    ERROR_VARIABLE  errors
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "population: `${exe} ${ARGN}` failed (exit ${rc}). A test binary that cannot "
            "even be asked for its inventory has no population to report.\n${captured}${errors}")
    endif()
    string(REPLACE "\r" "" captured "${captured}")
    set(${out_var} "${captured}" PARENT_SCOPE)
endfunction()

# The suites the built binary really has. doctest prints them one per line between two
# rules of '=', with its own [doctest] lines around them.
function(zen_suite_inventory exe out_var)
    zen_run_query("${exe}" raw --list-test-suites)
    string(REPLACE "\n" ";" lines "${raw}")
    set(found "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        if(line MATCHES "^\\[doctest\\]")
            continue()
        endif()
        if(line MATCHES "^=+$")
            continue()
        endif()
        list(APPEND found "${line}")
    endforeach()
    list(SORT found)
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

# How many cases a named suite selects. 0 is a legitimate answer here (this is a query,
# not a run) -- and it is exactly the answer the inventory check exists to catch.
function(zen_suite_case_count exe suite out_var)
    zen_run_query("${exe}" raw "--test-suite=${suite}" --count)
    if(NOT raw MATCHES "filters: ([0-9]+)")
        message(FATAL_ERROR
            "population: could not read a case count out of `${exe} --test-suite=${suite} "
            "--count`. doctest's --count output shape changed; this check must be repaired "
            "rather than removed.\n${raw}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# ---- the manifest ----------------------------------------------------------------

if(NOT EXISTS "${ZEN_MANIFEST}")
    message(FATAL_ERROR "population: the suite manifest is missing: ${ZEN_MANIFEST}")
endif()

# ---- reverse custody: this entry keeps the entry inventory invoked (VOLATILE-2a) --
#
# tests/verify.cmake carries the CTest-ENTRY inventory -- the check that notices when an
# add_test() disappears, THIS registration included. It cannot be a CTest entry itself,
# because an entry that has been deleted cannot complain about its own deletion, so nothing
# in a build would notice if the line that runs it were removed from the lane.
#
# So the two guard each other. That inventory keeps this entry registered; this entry, which
# is registered and therefore runs, requires the lane to still call it. Neither can be
# removed alone without the other saying so.
#
# It is a source-text tripwire, and that is the honest shape here rather than a weakness:
# no behaviour of a build can observe whether the LANE still asks a question, so the claim
# can only be checked by reading the file that makes it. Defense in depth, with its limit
# stated -- two coordinated deletions still escape, because a file cannot be defended
# against an edit to itself.
get_filename_component(zen_tests_dir "${ZEN_MANIFEST}" DIRECTORY)
set(zen_lane "${zen_tests_dir}/verify.cmake")
if(NOT EXISTS "${zen_lane}")
    message(FATAL_ERROR
        "population: the official lane ${zen_lane} is missing. It is what runs the "
        "CTest-entry inventory that keeps this very entry from being deleted silently, and "
        "AGENTS.md names it as the lane a result is quoted from.")
endif()
file(READ "${zen_lane}" zen_lane_text)
if(NOT zen_lane_text MATCHES "zen_check_entry_population\\(")
    message(FATAL_ERROR
        "population: ${zen_lane} no longer calls zen_check_entry_population(), so the "
        "official lane has stopped checking WHICH CTest entries it registered -- it would "
        "run whatever is left and report green at a smaller number. That is exactly the "
        "hole VOLATILE-2a closed, and this entry exists partly to keep it closed. Restore "
        "the call in the lane, or -- if the inventory has genuinely moved -- move this "
        "tripwire with it rather than deleting it.")
endif()

file(STRINGS "${ZEN_MANIFEST}" manifest_lines)

set(expected_main "")     # suites this configuration must have in zen-tests
set(expected_sdl "")      # ...and in zen-sdl-tests
set(declared_absent "")   # suites no active gate reaches: absent BY DECLARATION
set(problems "")
set(manifest_suites "")   # every suite named, in file order

# A suite may carry MORE THAN ONE row, and its floor is the sum of the rows whose gate is
# active. That is how a conditional subpopulation INSIDE a suite gets modelled explicitly
# rather than hidden under a slack floor: a suite that is smaller on Windows says so with a
# second row instead of buying portability with a floor that has downward slack on Linux.
# The worked examples, with their values, are in suite_population.txt beside the rows they
# describe -- one place, so the numbers cannot drift apart from the contract they explain.
# A suite is present if any of its rows' gates are active.
foreach(line IN LISTS manifest_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(REPLACE "\t" " " line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCHALL "[^ ]+" fields "${line}")
    list(LENGTH fields field_count)
    if(NOT field_count EQUAL 3)
        message(FATAL_ERROR
            "population: malformed manifest line (want `<suite> <gate> <min-cases>`): ${line}")
    endif()
    list(GET fields 0 suite)
    list(GET fields 1 gate)
    list(GET fields 2 minimum)
    if(NOT minimum MATCHES "^[0-9]+$")
        message(FATAL_ERROR "population: `${suite}` has a non-numeric floor: ${minimum}")
    endif()

    list(FIND manifest_suites "${suite}" known)
    if(known EQUAL -1)
        list(APPEND manifest_suites "${suite}")
        set(floor_${suite} 0)
        set(present_${suite} 0)
        set(binary_${suite} "main")
        set(rows_${suite} "")
        set(formula_${suite} "")
    endif()
    list(APPEND rows_${suite} "${gate}")

    list(FIND active_gates "${gate}" gate_active)
    if(gate_active EQUAL -1)
        continue()
    endif()
    math(EXPR floor_${suite} "${floor_${suite}} + ${minimum}")
    set(present_${suite} 1)
    if(gate STREQUAL "sdl")
        set(binary_${suite} "sdl")
    endif()
    if(formula_${suite} STREQUAL "")
        set(formula_${suite} "${minimum} ${gate}")
    else()
        set(formula_${suite} "${formula_${suite}} + ${minimum} ${gate}")
    endif()
endforeach()

foreach(suite IN LISTS manifest_suites)
    if(present_${suite})
        if(binary_${suite} STREQUAL "sdl")
            list(APPEND expected_sdl "${suite}")
        else()
            list(APPEND expected_main "${suite}")
        endif()
    else()
        string(REPLACE ";" "/" why "${rows_${suite}}")
        list(APPEND declared_absent "${suite} (gate '${why}' off here)")
    endif()
endforeach()

# ---- the comparison --------------------------------------------------------------

# One binary's worth of contract: the declared set must equal the built set exactly,
# and every declared suite must clear its floor.
function(zen_check_binary label exe expected out_problems out_report)
    set(local_problems "")
    set(local_report "")

    if(exe STREQUAL "")
        if(NOT expected STREQUAL "")
            string(CONCAT msg
                "${label}: suites [${expected}] are declared for an active gate, but no "
                "${label} binary was handed to the population check.")
            list(APPEND local_problems "${msg}")
        endif()
        set(${out_problems} "${local_problems}" PARENT_SCOPE)
        set(${out_report} "" PARENT_SCOPE)
        return()
    endif()

    zen_suite_inventory("${exe}" actual)
    set(sorted_expected ${expected})
    list(SORT sorted_expected)

    foreach(suite IN LISTS sorted_expected)
        list(FIND actual "${suite}" idx)
        if(idx EQUAL -1)
            string(CONCAT msg
                "${label}: suite '${suite}' is declared in the manifest but is NOT in the built "
                "binary -- renamed, deleted, or compiled out. Missing tests are absence of "
                "evidence, never successful evidence.")
            list(APPEND local_problems "${msg}")
        endif()
    endforeach()
    foreach(suite IN LISTS actual)
        list(FIND sorted_expected "${suite}" idx)
        if(idx EQUAL -1)
            string(CONCAT msg
                "${label}: suite '${suite}' exists in the built binary but is NOT declared in the "
                "manifest. Add it (with a gate and a floor) so the inventory stays a contract "
                "rather than a description.")
            list(APPEND local_problems "${msg}")
        endif()
    endforeach()

    foreach(suite IN LISTS sorted_expected)
        list(FIND actual "${suite}" idx)
        if(NOT idx EQUAL -1)
            zen_suite_case_count("${exe}" "${suite}" count)
            set(minimum "${floor_${suite}}")
            if(count LESS minimum)
                string(CONCAT msg
                    "${label}: suite '${suite}' selected ${count} cases, below its floor of "
                    "${minimum}. Cases were removed or compiled out.")
                list(APPEND local_problems "${msg}")
            endif()
            string(APPEND local_report
                   "  ${suite}: ${count} cases (floor ${minimum} = ${formula_${suite}})\n")
        endif()
    endforeach()

    set(${out_problems} "${local_problems}" PARENT_SCOPE)
    set(${out_report} "${local_report}" PARENT_SCOPE)
endfunction()

zen_check_binary("zen-tests" "${ZEN_TESTS_EXE}" "${expected_main}" main_problems main_report)
zen_check_binary("zen-sdl-tests" "${ZEN_SDL_TESTS_EXE}" "${expected_sdl}" sdl_problems sdl_report)
list(APPEND problems ${main_problems} ${sdl_problems})

# ---- the report ------------------------------------------------------------------

message(STATUS "population: gates active: ${ZEN_GATES}")
message(STATUS "population: zen-tests")
if(NOT main_report STREQUAL "")
    message("${main_report}")
endif()
if(NOT sdl_report STREQUAL "")
    message(STATUS "population: zen-sdl-tests")
    message("${sdl_report}")
endif()
if(NOT declared_absent STREQUAL "")
    message(STATUS "population: DECLARED ABSENT in this configuration (not run, and not passed):")
    foreach(entry IN LISTS declared_absent)
        message("  ${entry}")
    endforeach()
endif()

if(NOT problems STREQUAL "")
    set(text "")
    foreach(problem IN LISTS problems)
        string(APPEND text "  - ${problem}\n")
    endforeach()
    message(FATAL_ERROR
        "population: the verified population does not match the declared one.\n${text}"
        "  (contract: ${ZEN_MANIFEST})")
endif()

list(LENGTH expected_main main_count)
list(LENGTH expected_sdl sdl_count)
math(EXPR total_suites "${main_count} + ${sdl_count}")
message(STATUS "population: OK -- ${total_suites} declared suites all present and above their floors")
