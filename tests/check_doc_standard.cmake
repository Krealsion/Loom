# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `doc_standard` entry: does each current-facing document meet the documentation standard
# (CONTRIBUTING.md#comments-and-documents)? A private id is a red that names its line; an empty
# population is a red. `doc_links` holds the paths; neither check can see history, a phase or the
# maintainers' process described in words, and neither reads a page for truth.
#   cmake -P tests/check_doc_standard.cmake    (from the repository root, or -DZEN_REPO=<repo>)

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR "doc-standard: '${ZEN_REPO}' is not this repository's root "
                        "(no AGENTS.md). Pass -DZEN_REPO=<repository root>.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/private_ids.cmake")

# ---- scope -----------------------------------------------------------------------------
# Every *.md from the repository root, so a new folder is held the moment it exists; anything
# narrowing is a written rule. Not current-facing: build and editor state, the store, frozen
# history, rehomed source and vendored trees; and zen-vision.md, the founder's own statement of
# the project, which is kept as written.
set(ZEN_DOC_STANDARD_EXCLUDE
    "^build(-[^/]*)?/" "^cmake-build" "^_install" "^out/" "^[.]git(/|$)" "^[.]idea/" "^[.]vscode/"
    "^[.]claude/" "^docs/history/" "^archive/" "third_party/" "^zen-vision[.]md$")
# Documents not yet brought to the standard. The list only shrinks: a document leaves it when
# it meets the standard.
set(ZEN_DOC_STANDARD_PENDING
    "^docs/decisions/README[.]md$"
    "^docs/decisions/a-claim-is-not-a-message[.]md$"
    "^docs/decisions/admission-and-activation-share-one-boundary[.]md$"
    "^docs/decisions/committed-activation-is-not-answerable[.]md$"
    "^docs/decisions/declared-vocabulary-is-agreed-at-admission[.]md$"
    "^docs/decisions/dispatch-refusal-returns-to-its-author[.]md$"
    "^docs/decisions/lifecycle-authority-is-loom-owned[.]md$"
    "^docs/decisions/migration-is-authored-not-inferred[.]md$"
    "^docs/decisions/no-rollback-after-committed-production[.]md$"
    "^docs/decisions/office-authorship-is-deliberate[.]md$"
    "^docs/decisions/one-gate-at-every-boundary[.]md$"
    "^docs/decisions/readiness-is-authenticated-conversation[.]md$"
    "^docs/guides/diagnostics[.]md$"
    "^docs/guides/dynamic-weaves[.]md$"
    "^docs/guides/mental-model[.]md$"
    "^docs/guides/messaging[.]md$"
    "^docs/guides/observing[.]md$"
    "^docs/guides/replacing-a-service[.]md$"
    "^docs/guides/session-tools-map[.]md$"
    "^docs/guides/tools[.]md$"
    "^docs/guides/writing-a-weave[.]md$"
    "^docs/laws/README[.]md$"
    "^docs/laws/admission-laws[.]md$"
    "^docs/laws/answer-authority-laws[.]md$"
    "^docs/laws/handoff-laws[.]md$"
    "^docs/laws/kernel-laws[.]md$"
    "^docs/laws/lifecycle-laws[.]md$"
    "^docs/laws/messaging-laws[.]md$"
    "^docs/laws/population-laws[.]md$"
    "^docs/laws/replacement-laws[.]md$"
    "^docs/laws/sense-laws[.]md$"
    "^docs/reference/bridge[.]md$"
    "^docs/reference/dynamic-abi[.]md$"
    "^docs/reference/handoff[.]md$"
    "^docs/reference/history[.]md$"
    "^docs/reference/joint-publication[.]md$"
    "^docs/reference/kernel[.]md$"
    "^docs/reference/lifecycle[.]md$"
    "^docs/reference/observation[.]md$"
    "^docs/reference/prepared-replacement[.]md$"
    "^docs/reference/senses[.]md$"
    "^docs/reference/terminal[.]md$"
    "^docs/reference/values-and-admission[.]md$"
    "^docs/reference/weaver[.]md$")

string(ASCII 1 ZEN_SOH)
string(ASCII 2 ZEN_STX)
string(ASCII 3 ZEN_ETX)
string(ASCII 4 ZEN_EOT)

# A document as a CMake list of its lines, the four characters a list cannot hold swapped out.
function(zen_doc_standard_lines path out)
    file(READ "${path}" text)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE ";" "${ZEN_SOH}" text "${text}")
    string(REPLACE "[" "${ZEN_STX}" text "${text}")
    string(REPLACE "]" "${ZEN_ETX}" text "${text}")
    string(REPLACE "\\" "${ZEN_EOT}" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# Every finding in one document's lines.
function(zen_doc_standard_scan rel lines laws out)
    set(findings "")
    set(n 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        zen_private_ids("${line}" "${laws}" ids)
        foreach(id IN LISTS ids)
            list(APPEND findings "${rel}:${n}: `${id}` is a development-phase name or private id -- say the fact in words, and cite a law only by an id docs/laws/ declares")
        endforeach()
    endforeach()
    set(${out} "${findings}" PARENT_SCOPE)
endfunction()

function(zen_doc_standard_matches rel patterns out)
    set(hit FALSE)
    foreach(pattern IN LISTS patterns)
        if(rel MATCHES "${pattern}")
            set(hit TRUE)
        endif()
    endforeach()
    set(${out} ${hit} PARENT_SCOPE)
endfunction()

# ---- the population ----------------------------------------------------------------------
# Excluded top-level entries are pruned before the walk, so no build tree is enumerated.
file(GLOB top RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*")
set(documents "")
set(pending_count 0)
foreach(entry IN LISTS top)
    set(probe "${entry}")
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        set(probe "${entry}/")
    endif()
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(skip)
        continue()
    endif()
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        file(GLOB_RECURSE found RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/${entry}/*.md")
    elseif(entry MATCHES "[.]md$")
        set(found "${entry}")
    else()
        continue()
    endif()
    foreach(rel IN LISTS found)
        zen_doc_standard_matches("${rel}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
        zen_doc_standard_matches("${rel}" "${ZEN_DOC_STANDARD_PENDING}" pending)
        if(pending AND NOT skip)
            math(EXPR pending_count "${pending_count} + 1")
        elseif(NOT skip)
            list(APPEND documents "${rel}")
        endif()
    endforeach()
endforeach()
list(SORT documents)
list(LENGTH documents document_count)
zen_declared_laws("${ZEN_REPO}" laws)

# ---- the self-test ---------------------------------------------------------------------
zen_private_ids_self_test("doc-standard" "${laws}")
set(planted "# A page\n\nLoom routes it (MSG-09; see [x](y.md)).\nSince R2E-0 it does.\n")
string(REPLACE ";" "${ZEN_SOH}" planted "${planted}")
string(REPLACE "[" "${ZEN_STX}" planted "${planted}")
string(REPLACE "]" "${ZEN_ETX}" planted "${planted}")
string(REPLACE "\n" ";" planted "${planted}")
zen_doc_standard_scan("planted.md" "${planted}" "${laws}" got)
if(NOT got MATCHES "^planted[.]md:4: `R2E-0` " OR got MATCHES "planted[.]md:3:")
    message(FATAL_ERROR "doc-standard: self-test -- a planted page answered '${got}', want one "
                        "finding on its line 4 and none on its line 3")
endif()
foreach(probe "docs/history/x.md" "zen-vision.md" "build/x.md" "build-san/x.md" "tests/third_party/x.md")
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(NOT skip)
        message(FATAL_ERROR "doc-standard: self-test -- '${probe}' would be held")
    endif()
endforeach()
foreach(probe "README.md" "docs/README.md" "builder/x.md" "docs/historyx.md")
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(skip)
        message(FATAL_ERROR "doc-standard: self-test -- '${probe}' would be excluded")
    endif()
endforeach()
if(document_count EQUAL 0 OR NOT "CONTRIBUTING.md" IN_LIST documents)
    message(FATAL_ERROR "doc-standard: the population (${document_count} documents) does not hold "
                        "CONTRIBUTING.md, which states the standard; the walk has stopped reading the tree")
endif()

# ---- the tree --------------------------------------------------------------------------
set(findings "")
foreach(rel IN LISTS documents)
    zen_doc_standard_lines("${ZEN_REPO}/${rel}" lines)
    zen_doc_standard_scan("${rel}" "${lines}" "${laws}" found)
    list(APPEND findings ${found})
endforeach()
list(LENGTH findings finding_count)
list(LENGTH laws law_count)
message(STATUS "doc-standard: ${document_count} documents held, ${pending_count} not yet held "
               "(ZEN_DOC_STANDARD_PENDING); ${law_count} laws declared under docs/laws/; "
               "self-test passed")
if(finding_count GREATER 0)
    string(REPLACE "${ZEN_SOH}" "\;" findings "${findings}")
    string(REPLACE "${ZEN_STX}" "[" findings "${findings}")
    string(REPLACE "${ZEN_ETX}" "]" findings "${findings}")
    string(REPLACE "${ZEN_EOT}" "\\" findings "${findings}")
    string(REPLACE ";" "\n  " shown "${findings}")
    message(FATAL_ERROR "doc-standard: ${finding_count} findings:\n  ${shown}")
endif()
message(STATUS "doc-standard: PASSED -- no private id in a held document")
