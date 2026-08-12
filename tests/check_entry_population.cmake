# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE CTEST-ENTRY INVENTORY (POP-01, VOLATILE-2a; behavioural mutual custody, VOLATILE-B1).
#
# One question: are the CTest entries this configuration registered exactly the ones the
# repository declared it would register? Not how many -- WHICH.
#
# IT IS ASKED THROUGH TWO INDEPENDENTLY EXECUTED DOORS, both calling the one implementation
# below, so there is one authored expectation rather than two copies of it:
#
#   tests/verify.cmake     the official lane's preflight, before it runs anything
#   the `population` entry inside the run, as part of its own work
#
# ONE DOOR WAS NEVER ENOUGH, and why is the defect VOLATILE-B1 repaired. The inventory cannot
# be only a CTest entry: an entry that has been deleted cannot complain about its own
# deletion. It cannot be only the lane either: a file cannot check itself. The first answer to
# that was a source-text tripwire -- the `population` entry read verify.cmake and required the
# text `zen_check_entry_population(` to appear in it. VOLATILE-COLD removed the call in one
# ordinary edit and the lane still reported PASSED, because the surviving match was a string
# literal inside verify.cmake's own explanatory error message. Prose about a check satisfied
# the guard on the check. Nothing here greps for a name any more.
#
# WHAT REPLACED IT. Each door asks the build itself and compares by name, both directions. On
# top of that the lane leaves a WITNESS of having asked -- a receipt naming this run, this
# build directory, these gates, and the exact expected and registered identity sets it
# validated. The `population` entry measures all of that again for itself and refuses a
# receipt that disagrees with what it just measured, so the receipt cannot become a rubber
# stamp, and refuses a receipt that does not name the run it is part of, so it cannot be a
# leftover. A lane that stopped taking the inventory leaves no receipt for its own run.
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

# The manifests sit beside THIS file, and that has to be captured here rather than read from
# inside a function: CMAKE_CURRENT_LIST_DIR is dynamically scoped to whatever file is being
# processed, so a function that read it would resolve against its CALLER's directory. Both
# callers happen to live in tests/ today, which is exactly how that stays true by accident
# until it does not.
set(ZEN_ENTRY_MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}")

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

# ---- asking the build what it actually registered ---------------------------------
#
# `ctest -N` is the only authority on that: a list generated from the add_test() calls could
# not notice one that is gone (POP-01), and CTestTestfile.cmake re-parsed by hand would be a
# second implementation of CTest free to disagree with it.
#
# IT IS ASKED INDIRECTLY, from a one-line scratch project that does nothing but name the real
# build tree, because EVERY ctest listing mode (`-N`, `--show-only=human`,
# `--show-only=json-v1`) truncates <build>/Testing/Temporary/LastTest.log as a side effect of
# opening its own log. Measured, not assumed: 942 bytes of a run's log became 121 without the
# indirection. The `population` entry asks this question from inside a running lane, and a
# check that destroys the log of the run it is checking is not one anybody wants. The scratch
# project's own Testing/ absorbs it instead.
#
# The scratch file names the build directory and nothing else, so there is no generated CMake
# text being rewritten here and nothing to keep in step with CMake's output format. If the
# indirection ever stopped resolving, it would list ZERO entries -- and zero is refused below,
# loudly, rather than quietly agreeing with an empty expectation.
function(zen_ctest_entry_listing build_dir config selector out_listing)
    if(NOT EXISTS "${build_dir}/CTestTestfile.cmake")
        message(FATAL_ERROR
            "entries: '${build_dir}' is not a configured CTest build directory (no "
            "CTestTestfile.cmake), so there is no registered inventory to read.")
    endif()
    # ABSOLUTE, because the scratch file's subdirs() resolves against the scratch directory
    # and the lane is ordinarily invoked with a relative -DZEN_BUILD_DIR=build.
    get_filename_component(target "${build_dir}" ABSOLUTE)
    set(scratch "${target}/zen-entry-listing")
    file(WRITE "${scratch}/CTestTestfile.cmake" "subdirs(\"${target}\")\n")
    set(args "")
    if(NOT config STREQUAL "")
        list(APPEND args -C "${config}")
    endif()
    if(NOT selector STREQUAL "")
        list(APPEND args -R "${selector}")
    endif()
    execute_process(
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${scratch}" -N ${args}
        OUTPUT_VARIABLE listing RESULT_VARIABLE rc ERROR_VARIABLE errors)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "entries: could not list the CTest entries registered in '${build_dir}' "
            "(exit ${rc}).\n${listing}${errors}")
    endif()
    set(${out_listing} "${listing}" PARENT_SCOPE)
endfunction()

# The NAMES out of a `ctest -N` listing, with the two refusals that make the reading evidence
# rather than an impression: zero selected entries, and a listing this check could only
# half-read.
function(zen_entry_names_from_listing listing label out_names out_total)
    if(NOT listing MATCHES "Total Tests: ([0-9]+)")
        message(FATAL_ERROR
            "${label}: could not read a test count out of `ctest -N`.\n${listing}")
    endif()
    set(total "${CMAKE_MATCH_1}")

    if(total EQUAL 0)
        message(FATAL_ERROR
            "${label}: the selection matched ZERO registered tests. CTest would have printed "
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
            "${label}: `ctest -N` reported ${total} tests but named ${named} of them. This "
            "check could not read the inventory it is supposed to check, and an inventory "
            "check that cannot see the inventory is not evidence.\n${listing}")
    endif()

    set(${out_names} "${registered}" PARENT_SCOPE)
    set(${out_total} "${total}" PARENT_SCOPE)
endfunction()

# ---- THE COMPARISON: one implementation, called by both doors ----------------------
#
#   registered   the names read out of an UNFILTERED `ctest -N`
#   build_dir    the configured build tree
#   label        which door is speaking, so a diagnostic says which one found it
#
# It reports the gates and the expected set back to its caller, because the witness below
# records exactly what was compared and the `population` entry compares that record against
# its own measurement.
function(zen_entry_identity_check registered build_dir label out_gates out_expected)
    # ---- which configuration is this? ------------------------------------------------
    #
    # A DESCRIPTION written by the build at configure time, from the same variable that
    # feeds the `population` entry's gate list one line away, so the two cannot come to
    # disagree about which gates are on. It never relaxes the manifests: it only says which
    # of their rows apply here.
    set(gates_file "${build_dir}/zen-test-gates.cmake")
    if(NOT EXISTS "${gates_file}")
        message(FATAL_ERROR
            "${label}: '${build_dir}' has no zen-test-gates.cmake. That file is written at "
            "configure time by tests/CMakeLists.txt and says which gates this configuration "
            "has. Without it there is no way to tell a legitimately absent entry from a "
            "deleted one, so there is no honest inventory question to ask. Reconfigure.")
    endif()
    include("${gates_file}")
    if(NOT DEFINED ZEN_ACTIVE_GATES OR ZEN_ACTIVE_GATES STREQUAL "")
        message(FATAL_ERROR
            "${label}: ${gates_file} declares no active gates. Every supported configuration "
            "has at least `portable`; an empty gate set would deactivate every row of both "
            "manifests and leave nothing to expect.")
    endif()
    list(FIND ZEN_ACTIVE_GATES portable has_portable)
    if(has_portable EQUAL -1)
        message(FATAL_ERROR
            "${label}: ${gates_file} does not list the `portable` gate. That gate is on in "
            "every supported configuration by definition, so its absence means the "
            "description is wrong rather than that the configuration is unusual -- and a "
            "wrong description makes every missing entry look declared-absent.")
    endif()

    # ---- the expected set: two manifests, one union ----------------------------------

    zen_entry_manifest_rows("${ZEN_ENTRY_MANIFEST_DIR}/suite_population.txt"
                            suite_names suite_gates)
    zen_entry_manifest_rows("${ZEN_ENTRY_MANIFEST_DIR}/entry_population.txt"
                            other_names other_gates)

    # A suite must not also be declared as a non-suite entry: one name, one owner, and the
    # union would otherwise hide which file is answering for it.
    foreach(name IN LISTS other_names)
        list(FIND suite_names "${name}" clash)
        if(NOT clash EQUAL -1)
            message(FATAL_ERROR
                "${label}: '${name}' is declared in BOTH suite_population.txt and "
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
    list(LENGTH registered registered_count)
    list(LENGTH suite_expected suite_count)
    list(LENGTH other_expected other_count)
    message(STATUS "${label}: gates active: ${ZEN_ACTIVE_GATES}")
    message(STATUS "${label}: expected ${expected_count} "
                   "(${suite_count} declared suites + ${other_count} non-suite checks), "
                   "registered ${registered_count}")
    if(absent_count GREATER 0)
        message(STATUS "${label}: DECLARED ABSENT in this configuration (not run, and not passed):")
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
            "${label}: the registered CTest inventory does not match the declared one.\n${text}"
            "  (contracts: tests/suite_population.txt + tests/entry_population.txt)")
    endif()

    message(STATUS
        "${label}: OK -- every declared CTest entry is registered, and every registered "
        "entry is declared")

    set(${out_gates} "${ZEN_ACTIVE_GATES}" PARENT_SCOPE)
    set(${out_expected} "${expected}" PARENT_SCOPE)
endfunction()

# ---- the receipt ------------------------------------------------------------------
#
# The two doors reach the same build tree by different routes -- the lane is ordinarily run
# with a relative -DZEN_BUILD_DIR=build, the entry is registered with an absolute
# CMAKE_BINARY_DIR -- so every path this file records or compares is normalised first.
# Otherwise the receipt would name a directory the entry could not recognise as its own.
function(zen_entry_build_dir build_dir out_abs)
    get_filename_component(absolute "${build_dir}" ABSOLUTE)
    set(${out_abs} "${absolute}" PARENT_SCOPE)
endfunction()

# One path, named in one place, so the two doors cannot come to mean different files.
function(zen_entry_witness_file build_dir out_path)
    zen_entry_build_dir("${build_dir}" absolute)
    set(${out_path} "${absolute}/zen-entry-inventory.witness" PARENT_SCOPE)
endfunction()

# Sorted so the two doors' records of one set are comparable as text rather than only as
# sets -- the entry re-derives the same normal form from its own measurement.
function(zen_entry_normalise names out_text)
    set(sorted ${names})
    list(SORT sorted)
    string(REPLACE ";" " " text "${sorted}")
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

# THE LANE'S DOOR.
#
#   listing     the raw stdout of `ctest -N` for this run's selection
#   selector    the value of ZEN_SELECT, empty for the full lane
#   build_dir   the configured build tree
#   run_token   this lane run's token, exported to the ctest run as
#               ZEN_ENTRY_INVENTORY_RUN -- empty for a subset run
#   out_count   receives the number of entries the selector matched
#
# It also owns the two things the lane needs from that listing anyway -- the count, and the
# refusal of a zero -- so that removing the call does not quietly leave a lane that still
# runs. tests/verify.cmake refuses to continue without an answer from here, and the
# `population` entry refuses to pass without the receipt written at the end of this function.
function(zen_check_entry_population listing selector build_dir run_token out_count)
    zen_entry_build_dir("${build_dir}" build_abs)
    zen_entry_names_from_listing("${listing}" "entries" registered total)
    set(${out_count} "${total}" PARENT_SCOPE)

    # A SUBSET RUN IS NOT AN INVENTORY RUN, and it says so rather than passing quietly.
    # Under -DZEN_SELECT the listing is the entries the selector matched, so comparing it
    # against the declared set would fail for the one reason that is not a defect. The lane
    # exports no run token for such a run either, so the `population` entry -- if the subset
    # happens to include it -- asks its own half of the question and says the lane's half
    # was not asked, rather than demanding a receipt this run never owed.
    if(NOT selector STREQUAL "")
        message(STATUS
            "entries: NOT CHECKED -- this is a subset run (-R ${selector}). The entry "
            "inventory is a claim about the whole lane; run without ZEN_SELECT to make it.")
        return()
    endif()

    zen_entry_identity_check("${registered}" "${build_abs}" "entries" gates expected)

    # ---- the receipt, written only now, having actually done the work ------------------
    #
    # This is what the `population` entry requires of a lane that claims to be the official
    # one. It is not "a file exists": it names THIS run, and it records the two identity sets
    # this call just compared, which the entry re-measures for itself before believing any of
    # it. A lane that stopped taking the inventory never reaches this line, so its run has no
    # receipt and the entry inside it says so.
    zen_entry_witness_file("${build_abs}" witness)
    zen_entry_normalise("${expected}" expected_text)
    zen_entry_normalise("${registered}" registered_text)
    string(REPLACE ";" " " gates_text "${gates}")
    file(WRITE "${witness}"
         "# The CTest-entry inventory's receipt for one official-lane run (VOLATILE-B1).\n"
         "# Written by zen_check_entry_population() in tests/check_entry_population.cmake,\n"
         "# only after the comparison above passed; required by the `population` entry, which\n"
         "# measures every line of it again before believing it. Not an expectation: the\n"
         "# expectation is tests/suite_population.txt + tests/entry_population.txt.\n"
         "run ${run_token}\n"
         "build ${build_abs}\n"
         "gates ${gates_text}\n"
         "expected ${expected_text}\n"
         "registered ${registered_text}\n")
endfunction()

# ---- THE `population` ENTRY'S DOOR -------------------------------------------------
#
# Reads one field out of the receipt. The leading newline is prepended rather than anchored
# with `^`, so a key can only ever match at the start of a line.
function(zen_entry_witness_field text key out_value)
    set(${out_value} "" PARENT_SCOPE)
    if("\n${text}" MATCHES "\n${key} ([^\n]*)")
        set(${out_value} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
endfunction()

# The custody the `population` entry carries, in addition to the suite/floor contract that
# has always been its own work. TWO INDEPENDENT THINGS, in this order:
#
#   1. it asks the build what it registered and compares that against the manifests ITSELF.
#      This is the custody that matters: after it, deleting any declared registration is red
#      whether or not the lane ever looks.
#   2. if this run was launched by the official full lane, it requires that lane's inventory
#      to have left a receipt FOR THIS RUN which agrees with the measurement it just made.
#      This is what makes removing the lane's own door visible from outside the lane.
#
# Step 2 is skipped, and says so, when no run token reaches it: a bare `ctest` is a supported
# way to work (AGENTS.md), and a developer running one owes no receipt. The lane exports the
# token beside its ctest invocation, structurally apart from the call in step 1 that earns
# the receipt -- so the ordinary edit that removes the inventory leaves the announcement
# standing and the entry finds a run that claims the lane and cannot show its work.
#
# The stated ceiling, which is real: an edit that removes BOTH the call and the lane's
# announcement of itself escapes both doors' notice of each other. It does not escape step 1,
# which is why step 1 is first and does not depend on any of this.
function(zen_entry_population_custody build_dir config)
    zen_entry_build_dir("${build_dir}" build_abs)
    zen_ctest_entry_listing("${build_abs}" "${config}" "" listing)
    zen_entry_names_from_listing("${listing}" "population/entries" registered total)
    zen_entry_identity_check("${registered}" "${build_abs}" "population/entries" gates expected)

    set(token "$ENV{ZEN_ENTRY_INVENTORY_RUN}")
    if(token STREQUAL "")
        message(STATUS
            "population/entries: lane receipt NOT REQUIRED -- no official-lane run token "
            "reached this entry, so this is a bare `ctest` or a subset run. The inventory "
            "above was still taken, and is this entry's own answer.")
        return()
    endif()

    zen_entry_witness_file("${build_abs}" witness)
    if(NOT EXISTS "${witness}")
        message(FATAL_ERROR
            "population/entries: this run was launched by the official lane (run token "
            "${token}), but the lane's CTest-entry inventory left no receipt at ${witness}. "
            "tests/verify.cmake has stopped calling zen_check_entry_population() -- it is "
            "running the tests without first checking WHICH entries this build registered. "
            "The entry inventory above still answered, so nothing was accepted blindly; "
            "restore the lane's call so both doors are open again.")
    endif()
    file(READ "${witness}" witness_text)
    zen_entry_witness_field("${witness_text}" "run" witness_run)
    if(NOT witness_run STREQUAL "${token}")
        message(FATAL_ERROR
            "population/entries: the CTest-entry inventory's receipt at ${witness} was "
            "written for run '${witness_run}', but this run is '${token}'. It is a leftover "
            "from an earlier lane run, which means THIS lane did not take the inventory. "
            "A receipt from a run that already ended is not evidence about this one.")
    endif()

    zen_entry_witness_field("${witness_text}" "build" witness_build)
    zen_entry_witness_field("${witness_text}" "gates" witness_gates)
    zen_entry_witness_field("${witness_text}" "expected" witness_expected)
    zen_entry_witness_field("${witness_text}" "registered" witness_registered)
    string(REPLACE ";" " " gates_text "${gates}")
    zen_entry_normalise("${expected}" expected_text)
    zen_entry_normalise("${registered}" registered_text)

    set(disagreements "")
    if(NOT witness_build STREQUAL "${build_abs}")
        list(APPEND disagreements
             "build directory: receipt says '${witness_build}', this entry is in '${build_abs}'")
    endif()
    if(NOT witness_gates STREQUAL "${gates_text}")
        list(APPEND disagreements
             "active gates: receipt says '${witness_gates}', this entry measured '${gates_text}'")
    endif()
    if(NOT witness_expected STREQUAL "${expected_text}")
        list(APPEND disagreements
             "expected identities: receipt says '${witness_expected}', this entry read "
             "'${expected_text}' out of the manifests")
    endif()
    if(NOT witness_registered STREQUAL "${registered_text}")
        list(APPEND disagreements
             "registered identities: receipt says '${witness_registered}', this entry read "
             "'${registered_text}' out of `ctest -N`")
    endif()
    if(NOT disagreements STREQUAL "")
        set(text "")
        foreach(item IN LISTS disagreements)
            string(APPEND text "  - ${item}\n")
        endforeach()
        message(FATAL_ERROR
            "population/entries: the lane's inventory receipt does not describe the build "
            "this entry is running in.\n${text}"
            "  A receipt is only worth the measurement it agrees with; this one does not.")
    endif()

    message(STATUS
        "population/entries: lane receipt OK -- the official lane's own inventory ran for "
        "this run (${token}) and agrees with this entry's independent measurement")
endfunction()
