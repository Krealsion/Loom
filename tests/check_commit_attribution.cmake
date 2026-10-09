# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The commit-attribution check, run as a CI job: no commit reachable from the ref carries a
# co-author line or credits an AI assistant. It refuses every Co-authored-by trailer, whatever its
# value (git's own parser), and reads every message for an assistant's credit line, which the merge
# copies from a pull request's description. Prose about an assistant passes. The whole reachable
# history is scanned; narrow it with -DZEN_RANGE=<base>..<head>.

# A CI job, not a CTest entry, because it reads history, which a source export does not carry.
# Before scanning, it makes five dangling commits and requires the right verdict on each: an AI
# co-author, a human co-author and the credit line (caught), and prose naming an assistant and
# prose about the credit line, with no trailer (not caught). They have no ref, and git discards
# them at its next gc.

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

# The credit line, as git's own --grep reads a message, case folded: the link the line carries or
# its words. A sentence about the line ("the credit line was removed") names neither.
set(ZEN_ATTRIBUTION_CREDIT "generated with \\[claude code\\]|claude\\.com/claude-code")

# Sets ${out} to the commits among ${rev_args} (git log's revision arguments, a list) whose
# message holds the credit line, one git call for any number of commits.
function(zen_attribution_credited rev_args out)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log --format=%H -i -E
                "--grep=${ZEN_ATTRIBUTION_CREDIT}" ${rev_args}
        OUTPUT_VARIABLE shas OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not read messages of ${rev_args} (exit ${rc}).\n${err}")
    endif()
    string(REPLACE "\n" ";" shas "${shas}")
    list(REMOVE_ITEM shas "")
    set(${out} "${shas}" PARENT_SCOPE)
endfunction()

# Sets ${out} to the commit's co-author line, or "" if it carries none. Any value is refused: a
# commit is authored by the person who makes it.
function(zen_attribution_trailer sha out)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log -1
                "--format=%(trailers:key=Co-authored-by,valueonly,separator=%x1F)" "${sha}"
        OUTPUT_VARIABLE values OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not read trailers of ${sha} (exit ${rc}).\n${err}")
    endif()
    if(values STREQUAL "")
        set(${out} "" PARENT_SCOPE)
    else()
        set(${out} "Co-authored-by: ${values}" PARENT_SCOPE)
    endif()
endfunction()

# Both readings of one commit: its trailer, then its credit line. Sets ${out} to what offends,
# or "" if the commit is clean. The population reads the credit line once for the whole range.
function(zen_attribution_verdict sha out)
    zen_attribution_trailer("${sha}" found)
    if(found STREQUAL "")
        zen_attribution_credited("-1;${sha}" credited)
        if(credited)
            set(found "a credit line: Generated with [Claude Code]")
        endif()
    endif()
    set(${out} "${found}" PARENT_SCOPE)
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
    "Self-test: a forbidden attribution\n\nCo-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
    caught_sha)
zen_attribution_verdict("${caught_sha}" caught)
if(caught STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check did not catch a commit carrying the "
        "forbidden trailer. It would have reported a clean "
        "history over a dirty one, which is the only outcome worse than not running.")
endif()

zen_throwaway_commit(
    "Self-test: a human co-author\n\nCo-authored-by: A Human <human@example.invalid>"
    human_sha)
zen_attribution_verdict("${human_sha}" human)
if(human STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check passed a commit whose co-author is a human "
        "being. A commit is authored by the person who makes it, so a co-author line of any "
        "value is refused.")
endif()

zen_throwaway_commit(
    "Self-test: prose about an assistant\n\nThis message discusses Claude and co-authorship in prose, which is not attribution."
    clean_sha)
zen_attribution_verdict("${clean_sha}" wrongly_caught)
if(NOT wrongly_caught STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check flagged a commit with no co-author line and "
        "no credit line (\"${wrongly_caught}\"). Prose about an assistant is not attribution.")
endif()

zen_throwaway_commit(
    "Self-test: the merge of a described pull request\n\nThe change.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)"
    credit_sha)
zen_attribution_verdict("${credit_sha}" credit)
if(credit STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check did not catch a commit carrying the credit "
        "line an assistant appends to a pull request's description, which the merge copies into "
        "its commit.")
endif()

zen_throwaway_commit(
    "Self-test: prose about the credit line\n\nThe Claude Code credit line was removed from the description before the merge."
    prose_sha)
zen_attribution_verdict("${prose_sha}" wrongly_credited)
if(NOT wrongly_credited STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check flagged a commit that only talks about the "
        "credit line (\"${wrongly_credited}\"). Prose about Claude is not attribution.")
endif()

message(STATUS "attribution: self-test OK -- the check catches an AI co-author, a human co-author "
               "and the credit line, and spares prose")

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
zen_attribution_credited("${ZEN_RANGE}" credited_commits)
foreach(sha IN LISTS commits)
    zen_attribution_trailer("${sha}" value)
    if(value STREQUAL "" AND sha IN_LIST credited_commits)
        set(value "a credit line: Generated with [Claude Code]")
    endif()
    if(NOT value STREQUAL "")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log -1 --format=%s "${sha}"
            OUTPUT_VARIABLE subject OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND offenders "  ${sha}  ${subject}\n      ${value}")
    endif()
endforeach()

if(NOT offenders STREQUAL "")
    list(LENGTH offenders offender_count)
    string(REPLACE ";" "\n" text "${offenders}")
    message(FATAL_ERROR
        "attribution FAILED: ${offender_count} of ${commit_count} commit(s) reachable from "
        "'${ZEN_RANGE}' carry a co-author line or an AI credit.\n${text}\n\n"
        "  A commit is authored by the person who makes it, with no co-author line and no AI "
        "credit, for anyone:\n\n"
        "    You will be the one held accountable for the code you open a pull request for, not\n"
        "    the agent you used. Take care in the work you do, and consider the reasons and\n"
        "    outcomes of the choices you make.\n\n"
        "  Remove the trailer or credit line from the message -- "
        "amend if it is the tip, otherwise rewrite the affected messages "
        "(message-only, final tree unchanged) and force-push with an exact lease.")
endif()

message(STATUS
    "attribution: PASSED -- ${commit_count} commits reachable from '${ZEN_RANGE}', none "
    "carrying a co-author line or an AI credit")
