# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE REQUIRED-vs-ACTUAL WEAVE-CONTRACT POPULATION CHECK (POP-05, C3) -- the
# `weave_population` CTest entry.
#
# `weave_contract` proves that every artifact ON the roll has the reload-safe build
# properties. This proves that every artifact that MUST be on the roll is on it. The two
# entries are deliberately separate, and neither subsumes the other: a target can be
# required, present on the roll, and still compiled wrong (weave_contract catches that),
# and a target can quietly leave the roll while still being built and dlopen'ed
# (this catches that, and nothing else did -- COLD-2 finding C-3).
#
# The two inputs come from structurally independent places:
#
#   ZEN_REQUIRED     tests/weave_population.cmake, from the BUILD GRAPH -- target types
#                    and link closures. It never reads the contract roll or the verdict
#                    property, so a mutation that empties the roll leaves this untouched.
#
#   ZEN_CONTRACTED   the manifest generated from LOOM_WEAVE_CONTRACT_TARGETS, i.e. the
#                    roll loom_weave_build_contract() wrote for itself. This is the
#                    EVIDENCE side: what actually opted in.
#
# Green means they agree. Both directions are failures, and they are different failures:
# a required artifact missing from the roll is a live reload-safety hazard; a contracted
# artifact nobody declared required means the two concepts have drifted and one of them is
# now describing something else.

cmake_minimum_required(VERSION 3.16)

foreach(required IN ITEMS ZEN_REQUIRED ZEN_CONTRACTED)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "weave_population: -D${required}=... is required")
    endif()
    if(NOT EXISTS "${${required}}")
        message(FATAL_ERROR
            "weave_population: ${required} manifest is missing: ${${required}}. The check "
            "cannot run, and a check that cannot run has not passed.")
    endif()
endforeach()

# ---- the declared requirement (from the build graph) -------------------------------

file(STRINGS "${ZEN_REQUIRED}" required_rows)

set(required "")        # targets that must carry the contract
set(required_why "")    # ...and why, index-aligned
set(exempt "")          # targets deliberately outside it
set(exempt_why "")

foreach(row IN LISTS required_rows)
    if(row STREQUAL "")
        continue()
    endif()
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields field_count)
    if(NOT field_count EQUAL 3)
        message(FATAL_ERROR "weave_population: malformed required-population row: ${row}")
    endif()
    list(GET fields 0 kind)
    list(GET fields 1 target)
    list(GET fields 2 why)
    if(kind STREQUAL "REQUIRED")
        list(APPEND required "${target}")
        list(APPEND required_why "${why}")
    elseif(kind STREQUAL "EXEMPT")
        list(APPEND exempt "${target}")
        list(APPEND exempt_why "${why}")
    else()
        message(FATAL_ERROR "weave_population: unknown row kind '${kind}' in: ${row}")
    endif()
endforeach()

# ---- the actual roll (what opted in) -----------------------------------------------

file(STRINGS "${ZEN_CONTRACTED}" contract_rows)

set(contracted "")
foreach(row IN LISTS contract_rows)
    if(row STREQUAL "")
        continue()
    endif()
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 target)
    list(APPEND contracted "${target}")
endforeach()

# ---- zero is never a pass, on either side ------------------------------------------

list(LENGTH required required_count)
list(LENGTH contracted contracted_count)

if(required_count EQUAL 0)
    message(FATAL_ERROR
        "weave_population: ZERO artifacts were declared to require the weave build "
        "contract, in a configuration that registered this check because it builds "
        "loadable weaves. The build-graph sweep in tests/weave_population.cmake has "
        "stopped seeing them -- an expectation of nothing is satisfied by anything "
        "(POP-01), so this is a failure and not a quiet pass.")
endif()
if(contracted_count EQUAL 0)
    message(FATAL_ERROR
        "weave_population: the contract roll is EMPTY while ${required_count} artifacts "
        "require it. loom_weave_build_contract() reached nothing at all in this "
        "configuration.")
endif()

# ---- required but not contracted: the C-3 failure ----------------------------------

set(missing "")
math(EXPR last "${required_count} - 1")
foreach(i RANGE ${last})
    list(GET required ${i} target)
    list(GET required_why ${i} why)
    list(FIND contracted "${target}" found)
    if(found EQUAL -1)
        # string(CONCAT) rather than a multi-argument list(APPEND): every wrapped fragment
        # would otherwise become its own list element, and the count reported below would
        # be a count of SENTENCES rather than of artifacts.
        string(CONCAT msg
            "required weave target '${target}' (${why}) did not register the Loom weave "
            "build contract -- loom_weave_build_contract(${target}) was never called for "
            "it, so it is built without whatever this platform needs for dlclose to end "
            "its statics, and nothing else in this lane would have said so")
        list(APPEND missing "${msg}")
    endif()
endforeach()

# ---- contracted but not declared required: drift -----------------------------------

set(unexpected "")
foreach(target IN LISTS contracted)
    list(FIND required "${target}" is_required)
    if(NOT is_required EQUAL -1)
        continue()
    endif()
    list(FIND exempt "${target}" is_exempt)
    if(NOT is_exempt EQUAL -1)
        list(GET exempt_why ${is_exempt} why)
        string(CONCAT msg
            "target '${target}' is EXEMPT from the weave build contract (\"${why}\") and "
            "yet appears on the contract roll. Either the exemption is stale or the "
            "artifact it protects has stopped being what it says it is -- and if this is "
            "the bypass control, the negative control has just stopped being negative")
        list(APPEND unexpected "${msg}")
        continue()
    endif()
    string(CONCAT msg
        "target '${target}' took the weave build contract but is not in the required "
        "population and is not a declared exemption. That is not dangerous by itself, but "
        "the two concepts have diverged: either it is a loadable weave the build-graph "
        "sweep no longer recognises, or the contract is being applied somewhere it means "
        "nothing. Declare it or stop contracting it")
    list(APPEND unexpected "${msg}")
endforeach()

# ---- the report --------------------------------------------------------------------

message(STATUS
    "weave_population: ${required_count} artifacts required to carry the weave build "
    "contract; ${contracted_count} on the roll")

list(LENGTH exempt exempt_count)
if(exempt_count GREATER 0)
    message(STATUS "weave_population: DECLARED EXEMPT (not required, and not silently so):")
    math(EXPR last_exempt "${exempt_count} - 1")
    foreach(i RANGE ${last_exempt})
        list(GET exempt ${i} target)
        list(GET exempt_why ${i} why)
        message("  ${target}: ${why}")
    endforeach()
endif()

if(NOT missing STREQUAL "" OR NOT unexpected STREQUAL "")
    set(text "")
    foreach(problem IN LISTS missing)
        string(APPEND text "  - REQUIRED BUT NOT CONTRACTED: ${problem}\n")
    endforeach()
    foreach(problem IN LISTS unexpected)
        string(APPEND text "  - CONTRACTED BUT NOT DECLARED REQUIRED: ${problem}\n")
    endforeach()
    list(LENGTH missing missing_count)
    list(LENGTH unexpected unexpected_count)
    message(FATAL_ERROR
        "weave_population FAILED: ${missing_count} required artifact(s) are missing from "
        "the weave build-contract roll and ${unexpected_count} contracted artifact(s) were "
        "not declared required.\n${text}"
        "  (required: ${ZEN_REQUIRED})\n"
        "  (roll:     ${ZEN_CONTRACTED})")
endif()

message(STATUS
    "weave_population: OK -- required and contracted populations agree exactly")
