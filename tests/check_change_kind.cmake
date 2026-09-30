# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# Is a change documentation-only (AGENTS.md, the documentation lane)? Reads what changed from the
# base to the head, or to the working tree and its untracked files, and prints each file's kind;
# ZEN_KIND_OUT gets `compiled=true|false`. Exit 0 once it has answered, either way.
#   cmake [-DZEN_BASE=<rev>] [-DZEN_HEAD=<rev>] [-DZEN_KIND_OUT=<file>] -P tests/check_change_kind.cmake

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR "change-kind: '${ZEN_REPO}' is not this repository's root "
                        "(no AGENTS.md). Pass -DZEN_REPO=<repository root>.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/text_checks.cmake")
find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "change-kind: git is needed to read what changed")
endif()

string(ASCII 1 ZEN_SOH)   # ;
string(ASCII 2 ZEN_STX)   # [
string(ASCII 3 ZEN_ETX)   # ]
string(ASCII 4 ZEN_EOT)   # backslash
string(ASCII 5 ZEN_SP)    # a space inside a literal
string(ASCII 6 ZEN_TB)    # a tab inside a literal
string(ASCII 7 ZEN_DIR)   # the end of a preprocessing directive

# ---- what a file is ------------------------------------------------------------------------
# documentation: Markdown, an image under docs/, a text check and the files only the text checks
# read. source: a C/C++ file, judged by its tokens. Anything else is built or run by the official
# lane, and so is this file, the lane and the list they read.
function(zen_kind_of_path rel out)
    set(checks "")
    foreach(c IN LISTS ZEN_TEXT_CHECKS)
        list(APPEND checks "tests/check_${c}.cmake")
    endforeach()
    if(rel MATCHES "[.]md$" OR rel MATCHES "^docs/.*[.](png|jpe?g|gif|svg)$"
       OR rel IN_LIST ZEN_TEXT_ONLY_FILES OR rel IN_LIST checks)
        set(${out} documentation PARENT_SCOPE)
    elseif(rel MATCHES "[.](h|hpp|ipp|inl|c|cc|cpp|cxx)$")
        set(${out} source PARENT_SCOPE)
    else()
        set(${out} other PARENT_SCOPE)
    endif()
endfunction()

# ---- the proof that only comments changed ----------------------------------------------------
# A C/C++ text as its tokens would compare: line splices joined, comments taken out (a `/* */` is
# one space, as translation makes it), whitespace inside a string or character literal kept and
# every other run of it one space, and a line break kept only where it ends a preprocessing
# directive. Two texts whose readings are equal differ in comments and layout alone. Sets
# ${out_why} instead when this reading cannot be trusted: a raw string or a digit separator.
function(zen_kind_reading text out out_why)
    set(${out} "" PARENT_SCOPE)
    set(${out_why} "" PARENT_SCOPE)
    string(REPLACE "\r" "" t "${text}")
    string(REPLACE ";" "${ZEN_SOH}" t "${t}")
    string(REPLACE "[" "${ZEN_STX}" t "${t}")
    string(REPLACE "]" "${ZEN_ETX}" t "${t}")
    string(REPLACE "\\" "${ZEN_EOT}" t "${t}")
    string(REPLACE "${ZEN_EOT}\n" "" t "${t}")
    if(t MATCHES "(^|[^A-Za-z0-9_])(u8|u|U|L)?R\"")
        set(${out_why} "it holds a raw string literal" PARENT_SCOPE)
        return()
    endif()
    if(t MATCHES "[0-9]'[0-9A-Fa-f]")
        set(${out_why} "it holds a digit separator" PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCHALL
        "\"([^\"${ZEN_EOT}\n]|${ZEN_EOT}.)*\"|'([^'${ZEN_EOT}\n]|${ZEN_EOT}.)*'|//[^\n]*|/[*]([^*]|[*]+[^*/])*[*]+/|[^\"'/]+|."
        parts "${t}")
    set(r "")
    foreach(p IN LISTS parts)
        string(SUBSTRING "${p}" 0 2 head)
        string(SUBSTRING "${p}" 0 1 first)
        if(head STREQUAL "//")
            continue()
        elseif(head STREQUAL "/*")
            string(APPEND r " ")
        elseif(first STREQUAL "\"" OR first STREQUAL "'")
            string(REPLACE " " "${ZEN_SP}" p "${p}")
            string(REPLACE "\t" "${ZEN_TB}" p "${p}")
            string(APPEND r "${p}")
        else()
            string(APPEND r "${p}")
        endif()
    endforeach()
    string(REGEX REPLACE "[ \t]+" " " r "${r}")
    string(REGEX REPLACE " ?\n ?" "\n" r "${r}")
    string(REGEX REPLACE "\n+" "\n" r "${r}")
    string(REGEX REPLACE "^\n|\n$" "" r "${r}")
    string(REGEX REPLACE "(^|\n)(#[^\n]*)" "\\1\\2${ZEN_DIR}" r "${r}")
    string(REPLACE "\n" " " r "${r}")
    string(REGEX REPLACE " +" " " r "${r}")
    string(STRIP "${r}" r)
    set(${out} "${r}" PARENT_SCOPE)
endfunction()

# Sets ${out} to 1 when ${before} and ${after} differ only in comments and layout, and ${out_why}
# to why not otherwise.
function(zen_kind_comments_only before after out out_why)
    zen_kind_reading("${before}" a why_a)
    zen_kind_reading("${after}" b why_b)
    set(${out} 0 PARENT_SCOPE)
    if(NOT why_a STREQUAL "" OR NOT why_b STREQUAL "")
        set(${out_why} "cannot be read token by token: ${why_a}${why_b}" PARENT_SCOPE)
    elseif(a STREQUAL b)
        set(${out} 1 PARENT_SCOPE)
        set(${out_why} "" PARENT_SCOPE)
    else()
        set(${out_why} "its code changed" PARENT_SCOPE)
    endif()
endfunction()

# ---- the self-test: make it say YES, and make it say NO ------------------------------------
foreach(case
        "1|int a = 1; // one\n|int a = 1; // uno\n"
        "1|int a = 1;\n|// a note\nint a = 1;\n"
        "1|int a = 1;   int b;\n|/* spaced */ int a = 1; int b;\n"
        "1|x = \"//\";\n|x = \"//\"; // and a note\n"
        "1|#define A 1 // one\nint x;\n|#define A 1\nint x; /* two */\n"
        "0|int a = 1;\n|int a = 2;\n"
        "0|s = \"a  b\";\n|s = \"a b\";\n"
        "0|a/*c*/b;\n|ab;\n"
        "0|// c\nint x;\n|// c \\\nint x;\n"
        "0|#define A 1\nint x;\n|#define A 1 /*\n*/ int x;\n"
        "0|f(1, 2);\n|f(1 , 3);\n")
    string(REGEX MATCH "^([01])[|]([^|]*)[|]([^|]*)$" _ "${case}")
    set(want "${CMAKE_MATCH_1}")
    zen_kind_comments_only("${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}" got why)
    if(NOT got EQUAL want)
        message(FATAL_ERROR "change-kind: SELF-TEST FAILED -- '${CMAKE_MATCH_2}' against "
                            "'${CMAKE_MATCH_3}' read ${got} (${why}), want ${want}")
    endif()
endforeach()
zen_kind_comments_only("auto s = R\"(a)\";\n" "auto s = R\"(a)\"; // b\n" st_raw st_raw_why)
if(st_raw OR NOT st_raw_why MATCHES "raw string")
    message(FATAL_ERROR "change-kind: SELF-TEST FAILED -- a raw string was read token by token")
endif()
foreach(pair "notes.md|documentation" "docs/workshop/images/a.png|documentation"
             "tests/check_code_values.cmake|documentation" "src/a.cpp|source"
             "CMakeLists.txt|other" "tests/text_checks.cmake|other" ".github/workflows/ci.yml|other")
    string(REPLACE "|" ";" pp "${pair}")
    list(GET pp 0 p)
    list(GET pp 1 want)
    zen_kind_of_path("${p}" got)
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "change-kind: SELF-TEST FAILED -- ${p} read as ${got}, want ${want}")
    endif()
endforeach()

# ---- what changed ----------------------------------------------------------------------------
function(zen_kind_git out)
    # UTF-8 as the bytes are: on Windows the default decoding would rewrite a source's em dash, and
    # a comment-only change would then read as a code change.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ZEN_REPO}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e ENCODING UTF8)
    if(NOT rc EQUAL 0)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "change-kind: git ${shown} failed (exit ${rc}): ${e}")
    endif()
    set(${out} "${o}" PARENT_SCOPE)
endfunction()

if(NOT DEFINED ZEN_BASE OR ZEN_BASE STREQUAL "")
    zen_kind_git(ZEN_BASE merge-base HEAD origin/main)
    string(STRIP "${ZEN_BASE}" ZEN_BASE)
endif()
if(DEFINED ZEN_HEAD AND NOT ZEN_HEAD STREQUAL "")
    zen_kind_git(changes diff --name-status -M "${ZEN_BASE}" "${ZEN_HEAD}")
    set(untracked "")
else()
    zen_kind_git(changes diff --name-status -M "${ZEN_BASE}")
    zen_kind_git(untracked ls-files --others --exclude-standard)
endif()
string(REPLACE ";" "${ZEN_SOH}" changes "${changes}")
string(REPLACE "\n" ";" changes "${changes}")
string(REPLACE "\n" ";" untracked "${untracked}")
foreach(u IN LISTS untracked)
    if(NOT u STREQUAL "")
        list(APPEND changes "A\t${u}")
    endif()
endforeach()

set(compiled "")
set(count 0)
foreach(line IN LISTS changes)
    if(line STREQUAL "")
        continue()
    endif()
    math(EXPR count "${count} + 1")
    string(REPLACE "\t" ";" f "${line}")
    list(GET f 0 status)
    list(LENGTH f nf)
    math(EXPR last "${nf} - 1")
    list(GET f ${last} rel)
    set(paths "${rel}")
    if(status MATCHES "^[RC]")
        list(GET f 1 from)
        list(APPEND paths "${from}")
    endif()
    set(verdict "documentation")
    foreach(p IN LISTS paths)
        zen_kind_of_path("${p}" k)
        if(k STREQUAL "other")
            set(verdict "compiled: built or run by the official lane")
        elseif(k STREQUAL "source" AND verdict STREQUAL "documentation")
            if(NOT status STREQUAL "M")
                set(verdict "compiled: a source added, removed or renamed")
            elseif(NOT ZEN_COMMENT_ONLY_QUALIFIES)
                set(verdict "compiled: a source, and here a comment is read by compiled tests too")
            else()
                zen_kind_git(before show "${ZEN_BASE}:${p}")
                if(DEFINED ZEN_HEAD AND NOT ZEN_HEAD STREQUAL "")
                    zen_kind_git(after show "${ZEN_HEAD}:${p}")
                else()
                    file(READ "${ZEN_REPO}/${p}" after)
                endif()
                zen_kind_comments_only("${before}" "${after}" same why)
                if(same)
                    set(verdict "documentation: comments only")
                else()
                    set(verdict "compiled: ${why}")
                endif()
            endif()
        endif()
    endforeach()
    message(STATUS "change-kind: ${status} ${rel} -- ${verdict}")
    if(verdict MATCHES "^compiled")
        list(APPEND compiled "${rel}")
    endif()
endforeach()

list(LENGTH compiled n_compiled)
if(n_compiled EQUAL 0)
    set(answer false)
    message(STATUS "change-kind: documentation-only -- ${count} files from ${ZEN_BASE}; the documentation lane verifies it")
else()
    set(answer true)
    message(STATUS "change-kind: compiled -- ${n_compiled} of ${count} files from ${ZEN_BASE} are built or run; the official lane verifies it")
endif()
if(DEFINED ZEN_KIND_OUT AND NOT ZEN_KIND_OUT STREQUAL "")
    file(APPEND "${ZEN_KIND_OUT}" "compiled=${answer}\n")
endif()
