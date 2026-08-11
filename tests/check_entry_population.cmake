# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE CTEST-ENTRY INVENTORY (POP-01, VOLATILE-2a).
#
# One question: are the CTest entries this configuration registered exactly the ones the
# repository declared it would register? Not how many -- WHICH.
#
# It defines one function and executes nothing on its own. tests/verify.cmake is the only
# caller, and that is deliberate: a check that notices deleted CTest entries cannot itself
# be a CTest entry, because deleting its registration would delete the question along with
# the answer. The contract and the reasoning are in tests/entry_population.txt.
#
# TWO MANIFESTS, ONE EXPECTATION, NO DUPLICATED FACT:
#
#   suite_population.txt    the doctest suites: name + gate (+ a case floor this check
#                           never reads -- floors belong to the `population` entry)
#   entry_population.txt    the entries that are not suites: name + gate
#
# The union, resolved against the gates this configuration actually has, is the expected
# set. Neither manifest is derived from the add_test() calls it judges, which is the whole
# mechanism: a list generated from the registrations cannot notice a registration that is
# gone (POP-01).
#
# WHY A COUNT WOULD NOT HAVE DONE. `EXPECTED_CTEST_COUNT 39` detects a deletion only while
# no unrelated addition compensates, and it can never say which entry vanished, whether one
# was renamed, or whether a gate registered the wrong thing. Named identity answers all
# three; the totals below are printed as derived diagnostics and are never the contract.

cmake_minimum_required(VERSION 3.16)

# Name + gate rows out of a manifest, ignoring any further fields (suite_population.txt's
# third column is a case floor and is emphatically not this check's business). A name may
# carry several rows -- one per gate -- and is expected if ANY of them is active, which is
# the same rule the suite population check applies to the same file.
function(zen_entry_manifest_rows manifest out_names out_gates)
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR
            "entries: the manifest is missing: ${manifest}. Without a written expectation "
            "there is nothing to compare `ctest -N` against, and a comparison against "
            "nothing is satisfied by anything.")
    endif()
    file(STRINGS "${manifest}" lines)
    set(names "")
    set(gates "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        string(REPLACE "\t" " " line "${line}")
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        string(REGEX MATCHALL "[^ ]+" fields "${line}")
        list(LENGTH fields field_count)
        if(field_count LESS 2)
            message(FATAL_ERROR
                "entries: malformed manifest line in ${manifest} (want at least "
                "`<name> <gate>`): ${line}")
        endif()
        list(GET fields 0 name)
        list(GET fields 1 gate)
        list(APPEND names "${name}")
        list(APPEND gates "${gate}")
    endforeach()
    if(names STREQUAL "")
        message(FATAL_ERROR
            "entries: ${manifest} declares nothing. An expectation of nothing is satisfied "
            "by anything (POP-01), so an empty manifest is a failure and not a quiet pass.")
    endif()
    set(${out_names} "${names}" PARENT_SCOPE)
    set(${out_gates} "${gates}" PARENT_SCOPE)
endfunction()

# The names whose gate is active here, de-duplicated, plus the ones no active gate reaches
# (declared absent -- not run, and not passed).
function(zen_entry_resolve names gates active out_present out_absent)
    set(present "")
    set(seen "")
    set(reasons "")
    list(LENGTH names count)
    if(count EQUAL 0)
        return()
    endif()
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        list(GET names ${i} name)
        list(GET gates ${i} gate)
        list(FIND active "${gate}" gate_on)
        if(gate_on EQUAL -1)
            list(FIND seen "${name}" known)
            if(known EQUAL -1)
                list(APPEND reasons "${name} (gate '${gate}' off here)")
            endif()
        else()
            list(FIND present "${name}" known)
            if(known EQUAL -1)
                list(APPEND present "${name}")
            endif()
        endif()
        list(APPEND seen "${name}")
    endforeach()
    # A name with two rows -- one active gate, one inactive -- is PRESENT, not absent.
    set(absent "")
    foreach(reason IN LISTS reasons)
        string(REGEX REPLACE " .*$" "" bare "${reason}")
        list(FIND present "${bare}" is_present)
        if(is_present EQUAL -1)
            list(APPEND absent "${reason}")
        endif()
    endforeach()
    set(${out_present} "${present}" PARENT_SCOPE)
    set(${out_absent} "${absent}" PARENT_SCOPE)
endfunction()

# THE CHECK.
#
#   listing     the raw stdout of `ctest -N`
#   selector    the value of ZEN_SELECT, empty for the full lane
#   build_dir   the configured build tree
#   out_count   receives the number of entries the selector matched
#
# It also owns the two things the lane needs from that listing anyway -- the count, and the
# refusal of a zero -- so that removing the call does not quietly leave a lane that still
# runs. tests/verify.cmake refuses to continue without an answer from here.
function(zen_check_entry_population listing selector build_dir out_count)
    if(NOT listing MATCHES "Total Tests: ([0-9]+)")
        message(FATAL_ERROR
            "entries: could not read a test count out of `ctest -N`.\n${listing}")
    endif()
    set(total "${CMAKE_MATCH_1}")

    if(total EQUAL 0)
        message(FATAL_ERROR
            "entries: the selection matched ZERO registered tests. CTest would have printed "
            "\"No tests were found!!!\" and exited 0; this lane calls that what it is -- a "
            "question that was never asked, not an answer.")
    endif()

    # `ctest -N` prints one `Test #<n>: <name>` line per selected entry. Reading the NAMES
    # is what makes this an inventory rather than a tally, and the count is re-derived from
    # them: a listing this check could only half-read is not evidence about a population.
    string(REPLACE ";" "" scrubbed "${listing}")
    string(REGEX MATCHALL "Test +#[0-9]+: *[^\n\r]*" rows "${scrubbed}")
    set(registered "")
    foreach(row IN LISTS rows)
        if(row MATCHES "Test +#[0-9]+: *(.+)$")
            string(STRIP "${CMAKE_MATCH_1}" name)
            list(APPEND registered "${name}")
        endif()
    endforeach()
    list(LENGTH registered named)
    if(NOT named EQUAL total)
        message(FATAL_ERROR
            "entries: `ctest -N` reported ${total} tests but named ${named} of them. This "
            "check could not read the inventory it is supposed to check, and an inventory "
            "check that cannot see the inventory is not evidence.\n${listing}")
    endif()

    set(${out_count} "${total}" PARENT_SCOPE)

    # A SUBSET RUN IS NOT AN INVENTORY RUN, and it says so rather than passing quietly.
    # Under -DZEN_SELECT the listing is the entries the selector matched, so comparing it
    # against the declared set would fail for the one reason that is not a defect.
    if(NOT selector STREQUAL "")
        message(STATUS
            "entries: NOT CHECKED -- this is a subset run (-R ${selector}). The entry "
            "inventory is a claim about the whole lane; run without ZEN_SELECT to make it.")
        return()
    endif()

    # ---- which configuration is this? ------------------------------------------------
    #
    # A DESCRIPTION written by the build at configure time, from the same variable that
    # feeds the `population` entry's gate list one line away, so the two cannot come to
    # disagree about which gates are on. It never relaxes the manifests: it only says which
    # of their rows apply here.
    set(gates_file "${build_dir}/zen-test-gates.cmake")
    if(NOT EXISTS "${gates_file}")
        message(FATAL_ERROR
            "entries: '${build_dir}' has no zen-test-gates.cmake. That file is written at "
            "configure time by tests/CMakeLists.txt and says which gates this configuration "
            "has. Without it there is no way to tell a legitimately absent entry from a "
            "deleted one, so there is no honest inventory question to ask. Reconfigure.")
    endif()
    include("${gates_file}")
    if(NOT DEFINED ZEN_ACTIVE_GATES OR ZEN_ACTIVE_GATES STREQUAL "")
        message(FATAL_ERROR
            "entries: ${gates_file} declares no active gates. Every supported configuration "
            "has at least `portable`; an empty gate set would deactivate every row of both "
            "manifests and leave nothing to expect.")
    endif()
    list(FIND ZEN_ACTIVE_GATES portable has_portable)
    if(has_portable EQUAL -1)
        message(FATAL_ERROR
            "entries: ${gates_file} does not list the `portable` gate. That gate is on in "
            "every supported configuration by definition, so its absence means the "
            "description is wrong rather than that the configuration is unusual -- and a "
            "wrong description makes every missing entry look declared-absent.")
    endif()

    # ---- the expected set: two manifests, one union ----------------------------------

    zen_entry_manifest_rows("${CMAKE_CURRENT_LIST_DIR}/suite_population.txt"
                            suite_names suite_gates)
    zen_entry_manifest_rows("${CMAKE_CURRENT_LIST_DIR}/entry_population.txt"
                            other_names other_gates)

    # A suite must not also be declared as a non-suite entry: one name, one owner, and the
    # union would otherwise hide which file is answering for it.
    foreach(name IN LISTS other_names)
        list(FIND suite_names "${name}" clash)
        if(NOT clash EQUAL -1)
            message(FATAL_ERROR
                "entries: '${name}' is declared in BOTH suite_population.txt and "
                "entry_population.txt. A doctest suite's name and gate belong to the suite "
                "manifest, beside its floor; the entry manifest is for entries that are not "
                "suites. Two owners for one fact is how the two come to disagree.")
        endif()
    endforeach()

    zen_entry_resolve("${suite_names}" "${suite_gates}" "${ZEN_ACTIVE_GATES}"
                      suite_expected suite_absent)
    zen_entry_resolve("${other_names}" "${other_gates}" "${ZEN_ACTIVE_GATES}"
                      other_expected other_absent)

    # Joined through `foreach(IN LISTS ...)`, which skips empty elements, rather than by
    # concatenating two variables either of which may be empty: that leaves an empty element
    # behind, and an empty element prints a DECLARED ABSENT heading over nothing -- a report
    # claiming absences a reader would then go looking for.
    set(expected "")
    foreach(item IN LISTS suite_expected other_expected)
        list(APPEND expected "${item}")
    endforeach()
    set(declared_absent "")
    foreach(item IN LISTS suite_absent other_absent)
        list(APPEND declared_absent "${item}")
    endforeach()
    list(LENGTH declared_absent absent_count)

    # ---- the comparison, both directions ---------------------------------------------

    set(problems "")
    set(sorted_expected ${expected})
    set(sorted_registered ${registered})
    list(SORT sorted_expected)
    list(SORT sorted_registered)

    foreach(name IN LISTS sorted_expected)
        list(FIND sorted_registered "${name}" idx)
        if(idx EQUAL -1)
            string(CONCAT msg
                "MISSING: CTest entry '${name}' is declared for an active gate but is NOT "
                "registered in this build -- deleted, renamed, or gated out of a "
                "configuration that is supposed to have it. The lane would otherwise have "
                "run everything that remained and reported green at a smaller number.")
            list(APPEND problems "${msg}")
        endif()
    endforeach()
    foreach(name IN LISTS sorted_registered)
        list(FIND sorted_expected "${name}" idx)
        if(idx EQUAL -1)
            string(CONCAT msg
                "UNDECLARED: CTest entry '${name}' is registered in this build but no active "
                "row declares it. Declare it -- a suite with its gate and floor in "
                "suite_population.txt, anything else with its gate in entry_population.txt -- "
                "so the inventory stays the COMPLETE contract rather than a partial one.")
            list(APPEND problems "${msg}")
        endif()
    endforeach()

    # ---- the report -------------------------------------------------------------------

    list(LENGTH expected expected_count)
    list(LENGTH suite_expected suite_count)
    list(LENGTH other_expected other_count)
    message(STATUS "entries: gates active: ${ZEN_ACTIVE_GATES}")
    message(STATUS "entries: expected ${expected_count} "
                   "(${suite_count} declared suites + ${other_count} non-suite checks), "
                   "registered ${total}")
    if(absent_count GREATER 0)
        message(STATUS "entries: DECLARED ABSENT in this configuration (not run, and not passed):")
        foreach(absent IN LISTS declared_absent)
            message("  ${absent}")
        endforeach()
    endif()

    if(NOT problems STREQUAL "")
        set(text "")
        foreach(problem IN LISTS problems)
            string(APPEND text "  - ${problem}\n")
        endforeach()
        message(FATAL_ERROR
            "entries: the registered CTest inventory does not match the declared one.\n${text}"
            "  (contracts: tests/suite_population.txt + tests/entry_population.txt)")
    endif()

    message(STATUS
        "entries: OK -- every declared CTest entry is registered, and every registered "
        "entry is declared")
endfunction()
