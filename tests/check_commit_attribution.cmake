# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE COMMIT-ATTRIBUTION CHECK (HIST-1) -- the `attribution` CI job.
#
# The standing rule is that no AI assistant is ever recorded as a co-author of a Loom
# commit. The rule is a human instruction, and it has now failed in practice twice: a
# first wave was removed by HIST-0, and HIST-1 removed a second wave of 14 commits. A
# rule that has failed twice is not a rule, it is a hope. This is the mechanism.
#
# WHAT IT MATCHES, AND WHAT IT DELIBERATELY DOES NOT
#
# It asks git's OWN trailer parser for the Co-authored-by trailers of each commit and
# looks at their VALUES only. It therefore matches the thing that actually causes the
# attribution -- a real trailer in the trailer block, which is what GitHub reads -- and
# it cannot be tripped by prose. Discussing Claude in a commit message is normal and
# stays legal; the sentence you are reading would not trip it, and neither would a
# commit message describing this very check. Only the attribution itself is forbidden.
#
# It is keyed on the trailer VALUE, never on the trailer's presence: a Co-authored-by
# naming a human being is legitimate and must survive. [[marker-sniff-value-not-name]]
# is the same lesson from the other direction -- a check that fired on the bare key
# would fail in the widening direction, refusing honest human collaborators.
#
# WHOLE HISTORY, NOT JUST THE NEW COMMITS
#
# It scans everything reachable from the ref rather than the commits an event happened
# to introduce. That is stronger and much simpler: no event-payload range arithmetic, no
# force-push or first-push edge cases, and the invariant it states is the one actually
# wanted -- NO commit reachable from this ref carries the attribution, not merely none
# of the ones pushed today. HIST-1 made that invariant true across all of main, so it is
# a floor the repository can hold from here rather than an aspiration. Narrow it with
# -DZEN_RANGE=<base>..<head> if a deliberately-retained attribution is ever adopted.
#
# WHY IT IS A CI JOB AND NOT A CTEST ENTRY
#
# The other repository-owned checks run under CTest because they interrogate a BUILD.
# This one interrogates the repository's history, which a source export does not carry:
# as a CTest entry it would fail in any tarball or non-git checkout, for a reason that
# has nothing to do with the tree being wrong. It is also not a local git hook, because
# a hook that exists on one developer machine is a rule with the same failure mode as
# the one that already failed. It runs where the history is: in CI, on the hosted clone.
#
# THE SELF-TEST IS NOT OPTIONAL
#
# A clean repository and a broken detector produce byte-identical output, and after
# HIST-1 the repository IS clean -- so a passing run proves nothing at all unless the
# check has first been made to say NO. Before scanning anything real it manufactures two
# throwaway commit objects and requires the real code path to reach opposite verdicts on
# them: one carrying the forbidden attribution (must be caught) and one carrying a human
# co-author plus prose mentioning Claude (must NOT be caught, or the check has started
# refusing honest collaborators). Both are dangling objects -- no ref, no index, no
# working-tree change -- and git discards them at the next gc.

cmake_minimum_required(VERSION 3.16)

find_program(GIT_EXECUTABLE NAMES git)
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR
        "attribution: no git executable, so the history cannot be read. A check that "
        "cannot run has not passed.")
endif()

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
if(NOT DEFINED ZEN_RANGE OR ZEN_RANGE STREQUAL "")
    set(ZEN_RANGE "HEAD")
endif()

# ---- the predicate, in one place so the self-test exercises the real one ------------

# Sets ${out} to the offending trailer value, or "" if the commit is clean.
function(zen_attribution_verdict sha out)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log -1
                "--format=%(trailers:key=Co-authored-by,valueonly,separator=%x1F)" "${sha}"
        OUTPUT_VARIABLE values OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not read trailers of ${sha} (exit ${rc}).\n${err}")
    endif()
    string(TOLOWER "${values}" folded)
    if(folded MATCHES "claude" OR folded MATCHES "anthropic")
        set(${out} "${values}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Manufactures a dangling commit carrying ${message} and returns its sha.
function(zen_throwaway_commit message out)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                GIT_AUTHOR_NAME=attribution-selftest GIT_AUTHOR_EMAIL=selftest@invalid
                GIT_COMMITTER_NAME=attribution-selftest GIT_COMMITTER_EMAIL=selftest@invalid
                "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" commit-tree "${selftest_tree}" -m "${message}"
        OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not build a self-test commit (exit ${rc}).\n${err}")
    endif()
    set(${out} "${sha}" PARENT_SCOPE)
endfunction()

# ---- the self-test: make it say NO, and make it say YES -----------------------------

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" rev-parse "HEAD^{tree}"
    OUTPUT_VARIABLE selftest_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "attribution: '${ZEN_REPO}' is not a readable git repository.\n${err}")
endif()

zen_throwaway_commit(
    "Self-test: the wave HIST-1 removed\n\nCo-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
    caught_sha)
zen_attribution_verdict("${caught_sha}" caught)
if(caught STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check did not catch a commit carrying the "
        "exact trailer HIST-1 removed from 14 commits. It would have reported a clean "
        "history over a dirty one, which is the only outcome worse than not running.")
endif()

zen_throwaway_commit(
    "Self-test: legitimate collaboration\n\nThis message discusses Claude in prose, which is not attribution.\n\nCo-authored-by: A Human <human@example.invalid>"
    clean_sha)
zen_attribution_verdict("${clean_sha}" wrongly_caught)
if(NOT wrongly_caught STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check flagged a commit whose only co-author "
        "is a human being (\"${wrongly_caught}\"). It has started refusing honest "
        "collaborators, which is a different defect and not a safer one.")
endif()

message(STATUS "attribution: self-test OK -- the check catches the forbidden trailer and spares a human co-author")

# ---- the real population ------------------------------------------------------------

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" rev-list "${ZEN_RANGE}"
    OUTPUT_VARIABLE listing OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "attribution: could not list commits for '${ZEN_RANGE}' (exit ${rc}).\n${err}")
endif()

string(REPLACE "\n" ";" commits "${listing}")
list(REMOVE_ITEM commits "")
list(LENGTH commits commit_count)
if(commit_count EQUAL 0)
    message(FATAL_ERROR
        "attribution: '${ZEN_RANGE}' selected ZERO commits. An expectation of nothing is "
        "satisfied by anything (POP-01), so this is a failure and not a quiet pass -- "
        "check the range, and check that the clone carries history (fetch-depth: 0).")
endif()

set(offenders "")
foreach(sha IN LISTS commits)
    zen_attribution_verdict("${sha}" value)
    if(NOT value STREQUAL "")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log -1 --format=%s "${sha}"
            OUTPUT_VARIABLE subject OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND offenders "  ${sha}  ${subject}\n      Co-authored-by: ${value}")
    endif()
endforeach()

if(NOT offenders STREQUAL "")
    list(LENGTH offenders offender_count)
    string(REPLACE ";" "\n" text "${offenders}")
    message(FATAL_ERROR
        "attribution FAILED: ${offender_count} of ${commit_count} commit(s) reachable from "
        "'${ZEN_RANGE}' record an AI assistant as co-author.\n${text}\n\n"
        "  Loom records no AI co-authors. Remove the trailer from the commit message -- "
        "amend if it is the tip, otherwise rewrite the affected messages as HIST-1 did "
        "(message-only, final tree unchanged) and force-push with an exact lease.")
endif()

message(STATUS
    "attribution: PASSED -- ${commit_count} commits reachable from '${ZEN_RANGE}', none "
    "recording an AI co-author")
