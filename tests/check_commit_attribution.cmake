# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Joshua DeMoss
#
# The `attribution` CI job: no commit reachable from the ref may be authored or committed by an
# AI agent, carry a Co-authored-by trailer (git's own parser, any value), or an assistant's
# credit. The merge copies a pull request's title and description into that commit; on the
# pull request the job reads them from the environment. Prose naming an assistant passes.
# The predicate is git's trailer parser and one credit grep, the same one Zengine's check carries.

# A CI job, since a source export has no history. From the repository root, no build:
# cmake -P tests/check_commit_attribution.cmake
# -DZEN_RANGE=<a>..<b> narrows the commits. ZEN_PR_TITLE and ZEN_PR_BODY, when set, are the
# pull request's title and description.

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

# The credit a message may not carry, as git's own --grep reads it, case folded.
#   generated with [claude code], or claude.com/claude-code — Claude Code's line.
#   cursor.com/agents/ or cursor.com/background-agent — Cursor's footer of links to its
#   agent's pages, as written on Zengine pull requests 125, 126 and 127.
#   the HTML comment naming CURSOR_AGENT_PR_BODY_BEGIN — Cursor opens a description with
#   it. The comment is built below, and a sentence that names the token is prose.
set(ZEN_ATTRIBUTION_CREDIT
    "generated with \\[claude code\\]|claude\\.com/claude-code|cursor\\.com/agents/|cursor\\.com/background-agent")
set(ZEN_ATTRIBUTION_MARKER_NAME "CURSOR_AGENT_PR_BODY_BEGIN")
string(CONCAT ZEN_ATTRIBUTION_MARKER "<!-- " "${ZEN_ATTRIBUTION_MARKER_NAME}" " -->")
string(CONCAT ZEN_ATTRIBUTION_CREDIT "${ZEN_ATTRIBUTION_CREDIT}" "|" "${ZEN_ATTRIBUTION_MARKER}")

# Agent identities refused as an author or a committer, beside the credit line. Each email
# notes where that agent was seen.
#   cursoragent@cursor.com — Cursor's agent, author and committer of Zengine's 22c354cb,
#   the commit pull request 126 opened with. The attribution job passed that commit.
set(ZEN_ATTRIBUTION_AGENTS "cursoragent@cursor.com")

# Sets ${out} TRUE when ${email} is one of the agent identities, case folded. An empty
# email is nobody's, so it is not an agent.
function(zen_attribution_is_agent email out)
    string(STRIP "${email}" email)
    string(TOLOWER "${email}" lower)
    set(hit FALSE)
    foreach(agent IN LISTS ZEN_ATTRIBUTION_AGENTS)
        string(TOLOWER "${agent}" agent_lower)
        if(lower STREQUAL agent_lower AND NOT lower STREQUAL "")
            set(hit TRUE)
        endif()
    endforeach()
    set(${out} "${hit}" PARENT_SCOPE)
endfunction()

# The words naming an agent author or committer, or "" when both are a person's.
function(zen_attribution_phrase an ae cn ce out)
    set(phrase "")
    zen_attribution_is_agent("${ae}" author_hit)
    zen_attribution_is_agent("${ce}" committer_hit)
    if(author_hit)
        set(phrase "author ${an} <${ae}>")
    endif()
    if(committer_hit)
        if(phrase STREQUAL "")
            set(phrase "committer ${cn} <${ce}>")
        else()
            string(APPEND phrase ", committer ${cn} <${ce}>")
        endif()
    endif()
    set(${out} "${phrase}" PARENT_SCOPE)
endfunction()

# ${record} is sha, author name, author email, committer name, committer email, tab
# separated. Semicolons in a name are swapped before the split, because a CMake list
# splits on one. Sets ${out} to the phrase, or "".
function(zen_attribution_phrase_of_record record out)
    string(ASCII 1 soh)
    string(REPLACE "\r" "" record "${record}")
    string(REPLACE ";" "${soh}" safe "${record}")
    string(REPLACE "\t" ";" fields "${safe}")
    list(LENGTH fields n)
    if(NOT n EQUAL 5)
        message(FATAL_ERROR
            "attribution: an author record did not have 5 fields (${n}). A check that "
            "cannot read an identity has not passed.")
    endif()
    list(GET fields 1 an)
    list(GET fields 2 ae)
    list(GET fields 3 cn)
    list(GET fields 4 ce)
    foreach(var an ae cn ce)
        string(REPLACE "${soh}" ";" ${var} "${${var}}")
    endforeach()
    zen_attribution_phrase("${an}" "${ae}" "${cn}" "${ce}" phrase)
    set(${out} "${phrase}" PARENT_SCOPE)
endfunction()

# Sets ${out} to the commit's agent phrase, or "" when its author and committer are people.
function(zen_attribution_identity sha out)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log -1
                "--format=%H%x09%an%x09%ae%x09%cn%x09%ce" "${sha}"
        OUTPUT_VARIABLE record OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not read the author of ${sha} (exit ${rc}).\n${err}")
    endif()
    zen_attribution_phrase_of_record("${record}" phrase)
    set(${out} "${phrase}" PARENT_SCOPE)
endfunction()

# One read of every commit in ${rev_args}. Sets ${out_shas} and ${out_phrases} to the commits
# an agent authored or committed, and ${out_rows} to how many author lines were read.
function(zen_attribution_agents rev_args out_shas out_phrases out_rows)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" log
                "--format=%H%x09%an%x09%ae%x09%cn%x09%ce" ${rev_args}
        OUTPUT_VARIABLE listing OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not read authors of ${rev_args} (exit ${rc}).\n${err}")
    endif()
    string(ASCII 1 soh)
    string(REPLACE ";" "${soh}" safe "${listing}")
    string(REPLACE "\n" ";" rows "${safe}")
    set(shas "")
    set(phrases "")
    set(nrows 0)
    foreach(row IN LISTS rows)
        if(row STREQUAL "")
            continue()
        endif()
        math(EXPR nrows "${nrows} + 1")
        string(REPLACE "\t" ";" probe "${row}")
        list(LENGTH probe nfields)
        if(NOT nfields EQUAL 5)
            message(FATAL_ERROR
                "attribution: an author line did not have 5 fields (${nfields}). A check that "
                "cannot read an identity has not passed.")
        endif()
        list(GET probe 0 sha)
        zen_attribution_phrase_of_record("${row}" phrase)
        if(NOT phrase STREQUAL "")
            string(REPLACE ";" "," phrase "${phrase}")
            list(APPEND shas "${sha}")
            list(APPEND phrases "${phrase}")
        endif()
    endforeach()
    set(${out_shas} "${shas}" PARENT_SCOPE)
    set(${out_phrases} "${phrases}" PARENT_SCOPE)
    set(${out_rows} "${nrows}" PARENT_SCOPE)
endfunction()

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

# One commit, in the order the population uses: its trailer, then an agent identity, then its
# credit. Sets ${out} to what offends, or "" if the commit is clean. The population reads the
# credit and the identities once for the whole range.
function(zen_attribution_verdict sha out)
    set(found "")
    zen_attribution_trailer("${sha}" trailer)
    if(NOT trailer STREQUAL "")
        set(found "${trailer}")
    endif()
    if(found STREQUAL "")
        zen_attribution_identity("${sha}" identity)
        if(NOT identity STREQUAL "")
            set(found "${identity}")
        endif()
    endif()
    if(found STREQUAL "")
        zen_attribution_credited("-1;${sha}" credited)
        if(credited)
            set(found "an assistant's credit")
        endif()
    endif()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# A directory for a message file. commit-tree -F reads it, so a message's semicolons and its
# length never become a command argument.
function(zen_attribution_tmpdir out)
    if(NOT "$ENV{TMPDIR}" STREQUAL "")
        set(dir "$ENV{TMPDIR}")
    elseif(NOT "$ENV{TEMP}" STREQUAL "")
        set(dir "$ENV{TEMP}")
    elseif(NOT "$ENV{TMP}" STREQUAL "")
        set(dir "$ENV{TMP}")
    else()
        set(dir "/tmp")
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

# Manufactures a dangling commit carrying ${message} and returns its sha. Optional arguments
# are the author name, author email, committer name and committer email; omitted, both are a
# person.
function(zen_throwaway_commit message out)
    set(author_name "attribution-selftest")
    set(author_email "selftest@invalid")
    set(committer_name "attribution-selftest")
    set(committer_email "selftest@invalid")
    if(ARGC EQUAL 6)
        set(author_name "${ARGV2}")
        set(author_email "${ARGV3}")
        set(committer_name "${ARGV4}")
        set(committer_email "${ARGV5}")
    elseif(NOT ARGC EQUAL 2)
        message(FATAL_ERROR
            "attribution: a self-test commit takes a message, or a message and both identities.")
    endif()
    zen_attribution_tmpdir(tmpdir)
    string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" nonce)
    set(msgfile "${tmpdir}/loom-attribution-${nonce}.txt")
    file(WRITE "${msgfile}" "${message}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "GIT_AUTHOR_NAME=${author_name}"
                "GIT_AUTHOR_EMAIL=${author_email}"
                "GIT_COMMITTER_NAME=${committer_name}"
                "GIT_COMMITTER_EMAIL=${committer_email}"
                "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" commit-tree "${selftest_tree}" -F "${msgfile}"
        OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    file(REMOVE "${msgfile}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "attribution: could not build a self-test commit (exit ${rc}).\n${err}")
    endif()
    set(${out} "${sha}" PARENT_SCOPE)
endfunction()

# Judges ${text} by the commit predicate: the same trailer parser and the same credit grep.
function(zen_attribution_judge_text text out)
    zen_throwaway_commit("${text}" sha)
    zen_attribution_verdict("${sha}" found)
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

string(CONCAT ZEN_ATTRIBUTION_REASON
    "  A commit is authored by the person who makes it, with no co-author line and no AI\n"
    "  credit, for anyone:\n"
    "\n"
    "    You will be the one held accountable for the code you open a pull request for, not\n"
    "    the agent you used. Take care in the work you do, and consider the reasons and\n"
    "    outcomes of the choices you make.\n")

# ---- the self-test: make it say NO, and make it say YES ---------------------------------
# Throwaway commits, all dangling -- no ref, no index, no working-tree change -- and git
# discards them at the next gc. Each new refusal says no and its neighbour says yes.

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" rev-parse "HEAD^{tree}"
    OUTPUT_VARIABLE selftest_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "attribution: '${ZEN_REPO}' is not a readable git repository.\n${err}")
endif()

if(NOT "cursoragent@cursor.com" IN_LIST ZEN_ATTRIBUTION_AGENTS)
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- cursoragent@cursor.com is not among the agent "
        "identities. Cursor's agent was seen as the author and committer of 22c354cb.")
endif()

zen_throwaway_commit(
    "Self-test: an assistant's co-author trailer\n\nCo-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
    caught_sha)
zen_attribution_verdict("${caught_sha}" caught)
if(caught STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check did not catch a commit carrying the "
        "exact co-author trailer an AI assistant writes. It would have reported a clean "
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

zen_throwaway_commit(
    "Self-test: an AI agent authors the commit\n\nThe change, with no trailer and no credit."
    agent_author_sha
    "Cursor Agent" "cursoragent@cursor.com"
    "A Person" "person@example.invalid")
zen_attribution_verdict("${agent_author_sha}" agent_author)
if(NOT agent_author STREQUAL "author Cursor Agent <cursoragent@cursor.com>")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- an AI agent's authorship was not refused in words "
        "naming that identity (\"${agent_author}\").")
endif()

zen_throwaway_commit(
    "Self-test: an AI agent commits the commit\n\nThe change, with no trailer and no credit."
    agent_committer_sha
    "A Person" "person@example.invalid"
    "Cursor Agent" "cursoragent@cursor.com")
zen_attribution_verdict("${agent_committer_sha}" agent_committer)
if(NOT agent_committer STREQUAL "committer Cursor Agent <cursoragent@cursor.com>")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- an AI agent's committer was not refused in words "
        "naming that identity (\"${agent_committer}\").")
endif()

zen_throwaway_commit(
    "Self-test: a person authors and commits\n\nThe change, with no trailer and no credit."
    person_sha
    "A Person" "person@example.invalid"
    "A Person" "person@example.invalid")
zen_attribution_verdict("${person_sha}" person_ok)
if(NOT person_ok STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check flagged a commit a person authored and "
        "committed (\"${person_ok}\").")
endif()

# A footer of links to an assistant's pages. The links and ids are made up; the shape is
# the one a description carried, and the links are what the credit grep refuses.
set(ZEN_ATTRIBUTION_CURSOR_FOOTER [=[<!-- CURSOR_AGENT_PR_BODY_END -->

<div><a href="https://cursor.com/agents/bc-00000000-0000-4000-8000-000000000000?cursor_ref=pr_footer&cursor_cta=open_in_web"><picture><source media="(prefers-color-scheme: dark)" srcset="https://cursor.com/assets/images/open-in-web-dark.png"><source media="(prefers-color-scheme: light)" srcset="https://cursor.com/assets/images/open-in-web-light.png"><img alt="Open in Web" width="114" height="28" src="https://cursor.com/assets/images/open-in-web-dark.png"></picture></a>&nbsp;<a href="https://cursor.com/background-agent?bcId=bc-00000000-0000-4000-8000-000000000000&cursor_ref=pr_footer&cursor_cta=open_in_cursor"><picture><source media="(prefers-color-scheme: dark)" srcset="https://cursor.com/assets/images/open-in-cursor-dark.png"><source media="(prefers-color-scheme: light)" srcset="https://cursor.com/assets/images/open-in-cursor-light.png"><img alt="Open in Cursor" width="131" height="28" src="https://cursor.com/assets/images/open-in-cursor-dark.png"></picture></a>&nbsp;</div>]=])

zen_throwaway_commit(
    "Self-test: the footer a pull request carried\n\nThe change.\n\n${ZEN_ATTRIBUTION_CURSOR_FOOTER}"
    footer_sha)
zen_attribution_verdict("${footer_sha}" footer_found)
if(NOT footer_found STREQUAL "an assistant's credit")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- Cursor's footer was not refused as a credit "
        "(\"${footer_found}\").")
endif()

zen_throwaway_commit(
    "Self-test: prose naming Cursor\n\nThis sentence names Cursor and its agent, which is not a footer."
    cursor_prose_sha)
zen_attribution_verdict("${cursor_prose_sha}" cursor_prose)
if(NOT cursor_prose STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the check flagged a sentence naming Cursor "
        "(\"${cursor_prose}\"). Prose naming an assistant is not attribution.")
endif()

zen_attribution_judge_text(
    "Self-test: a pull request title\n\n${ZEN_ATTRIBUTION_CURSOR_FOOTER}"
    pr_footer)
if(NOT pr_footer STREQUAL "an assistant's credit")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the pull request text was not refused when it "
        "carried Cursor's footer (\"${pr_footer}\").")
endif()

zen_attribution_judge_text(
    "Self-test: a pull request naming Cursor\n\nThis description names Cursor and says an assistant wrote the patch, which is prose."
    pr_prose)
if(NOT pr_prose STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- a pull request whose prose names Cursor was flagged "
        "(\"${pr_prose}\").")
endif()

# The comment Cursor opens a description with is a credit, on a commit and on a pull
# request. A sentence that names it is prose. The comment is the one the predicate built.
if(ZEN_ATTRIBUTION_MARKER STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the marker a description opens with was not built. "
        "An empty alternative in the credit pattern matches every message.")
endif()

zen_throwaway_commit(
    "Self-test: the marker a description opens with\n\n${ZEN_ATTRIBUTION_MARKER}\n\nThe change, and no footer."
    marker_sha)
zen_attribution_verdict("${marker_sha}" marker_found)
if(NOT marker_found STREQUAL "an assistant's credit")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the marker a description opens with was not refused "
        "as a credit (\"${marker_found}\").")
endif()

zen_throwaway_commit(
    "Self-test: a sentence naming the marker\n\nThis sentence names ${ZEN_ATTRIBUTION_MARKER_NAME}, the marker Cursor opens a description with."
    marker_prose_sha)
zen_attribution_verdict("${marker_prose_sha}" marker_prose)
if(NOT marker_prose STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- a sentence naming the marker was flagged "
        "(\"${marker_prose}\"). Prose naming it is not attribution.")
endif()

zen_attribution_judge_text(
    "Self-test: a pull request opening with the marker\n\n${ZEN_ATTRIBUTION_MARKER}\n\nThe change, and no footer."
    pr_marker)
if(NOT pr_marker STREQUAL "an assistant's credit")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- the pull request text was not refused when it "
        "opened with the marker (\"${pr_marker}\").")
endif()

zen_attribution_judge_text(
    "Self-test: a pull request naming the marker\n\nThis description names ${ZEN_ATTRIBUTION_MARKER_NAME}, the marker Cursor opens a description with, which is prose."
    pr_marker_prose)
if(NOT pr_marker_prose STREQUAL "")
    message(FATAL_ERROR
        "attribution: SELF-TEST FAILED -- a pull request whose prose names the marker was "
        "flagged (\"${pr_marker_prose}\").")
endif()

message(STATUS "attribution: self-test OK -- an agent's identity and a person's, a co-author "
               "line, Claude Code's credit line, Cursor's footer, the marker a description "
               "opens with, and prose naming an assistant or that marker")

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
zen_attribution_agents("${ZEN_RANGE}" agent_shas agent_phrases agent_rows)
if(NOT agent_rows EQUAL commit_count)
    message(FATAL_ERROR
        "attribution: read ${agent_rows} author lines for ${commit_count} commits in "
        "'${ZEN_RANGE}'. A check that did not read every identity has not passed.")
endif()
foreach(sha IN LISTS commits)
    zen_attribution_trailer("${sha}" value)
    if(value STREQUAL "")
        list(FIND agent_shas "${sha}" at)
        if(NOT at EQUAL -1)
            list(GET agent_phrases ${at} value)
        endif()
    endif()
    if(value STREQUAL "" AND sha IN_LIST credited_commits)
        set(value "an assistant's credit")
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
        "'${ZEN_RANGE}' carry an AI agent's identity, a co-author line or an AI credit.\n${text}\n\n"
        "${ZEN_ATTRIBUTION_REASON}\n"
        "  The author and the committer are that person. Remove the trailer or the credit "
        "from the message, and leave the agent off the author and the committer -- "
        "amend if it is the tip, otherwise rewrite the affected commits "
        "(message and identity only, final tree unchanged) and force-push with an exact lease.")
endif()

# The pull request's title and description, the text the merge copies onto main. Empty means
# this run is not a pull request. The values arrive as environment variables; the job's script
# does not interpolate them.
set(pr_title "$ENV{ZEN_PR_TITLE}")
set(pr_body "$ENV{ZEN_PR_BODY}")
set(pr_note "")
if(NOT pr_title STREQUAL "" OR NOT pr_body STREQUAL "")
    string(CONCAT pr_text "${pr_title}" "\n\n" "${pr_body}")
    zen_attribution_judge_text("${pr_text}" pr_found)
    if(NOT pr_found STREQUAL "")
        message(FATAL_ERROR
            "attribution FAILED: the pull request's title or description carries ${pr_found}.\n"
            "  The merge copies that title and description into a commit on main.\n\n"
            "${ZEN_ATTRIBUTION_REASON}\n"
            "  Edit the title or the description and remove the credit, then push a commit\n"
            "  or close and reopen the pull request. A re-run reads the text its run began\n"
            "  with.")
    endif()
    set(pr_note ", and the pull request's title and description carry none of that")
endif()

message(STATUS
    "attribution: PASSED -- ${commit_count} commits reachable from '${ZEN_RANGE}', none "
    "authored or committed by an AI agent, and none carrying a co-author line or an AI credit"
    "${pr_note}")
