# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `weave_contract` entry (KERN-05) reads the built artifacts, not the build system's opinion
# of them, and on ELF confirms each claim against the symbol table: (1) no contracted artifact
# carries an STB_GNU_UNIQUE symbol; (2) the uncontracted twin still carries its unique sentinel,
# so claim 1 cannot pass because the sentinel stopped being able to fail; (3) the host binary
# caught no contracted target's options; (4) on PE-COFF the ELF-only option was applied to nothing.

foreach(v ZEN_MANIFEST ZEN_HOST_EXE ZEN_HOST_OPTIONS ZEN_BYPASS_LIB ZEN_PLATFORM)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "weave_contract: -D${v}=... is required")
    endif()
endforeach()

if(NOT EXISTS "${ZEN_MANIFEST}")
    message(FATAL_ERROR "weave_contract: no manifest at ${ZEN_MANIFEST}")
endif()

set(mitigation "fno-gnu-unique")
set(elf_platform FALSE)
if(NOT ZEN_PLATFORM STREQUAL "Windows" AND NOT ZEN_PLATFORM STREQUAL "Darwin")
    set(elf_platform TRUE)
endif()

# ---- the reader ------------------------------------------------------------------
#
# `readelf -sW` over the whole symbol table; UNIQUE is how binutils spells STB_GNU_UNIQUE
# in the Bind column. Absence of the tool is a FAILURE, never a skip: a proof that cannot
# run has not passed (the harness-honesty rule this project applied to itself).
function(zen_unique_symbols path kind out_count out_names)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "weave_contract: artifact missing: ${path}")
    endif()
    if(ZEN_NM STREQUAL "")
        message(FATAL_ERROR
            "weave_contract: no nm found, so the symbol bindings of ${path} cannot be "
            "read. This check is the only thing standing between a stranger and a silent "
            "use-after-free on unload; it fails rather than skips. Install binutils, or "
            "run this lane on a host that has it.")
    endif()
    # `nm` spells STB_GNU_UNIQUE as the type letter `u` and agrees with `readelf -s`, at a
    # fraction of the cost. A loaded image is judged by its .dynsym, from which glibc's unique
    # table is filled; an archive has none and is read whole, since its objects join another
    # image's .dynsym later.
    set(nm_args --defined-only)
    if(NOT kind STREQUAL "STATIC_LIBRARY" AND NOT kind STREQUAL "OBJECT_LIBRARY")
        list(APPEND nm_args -D)
    endif()
    execute_process(COMMAND ${ZEN_NM} ${nm_args} "${path}"
                    OUTPUT_VARIABLE dump RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "weave_contract: nm failed on ${path} (exit ${rc})\n${err}")
    endif()
    set(names "")
    string(REPLACE "\n" ";" lines "${dump}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[0-9a-fA-F]+ u (.+)$")
            list(APPEND names "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES names)
    list(LENGTH names n)
    set(${out_count} "${n}" PARENT_SCOPE)
    set(${out_names} "${names}" PARENT_SCOPE)
endfunction()

# ---- claims 1 and 4: every contracted artifact ------------------------------------

file(STRINGS "${ZEN_MANIFEST}" rows)
set(checked 0)
set(failures "")

foreach(row IN LISTS rows)
    if(row STREQUAL "")
        continue()
    endif()
    string(REPLACE "|" ";" f "${row}")
    list(LENGTH f nf)
    if(NOT nf EQUAL 5)
        message(FATAL_ERROR "weave_contract: malformed manifest row: ${row}")
    endif()
    list(GET f 0 name)
    list(GET f 1 artifact)
    list(GET f 2 kind)
    list(GET f 3 verdict)
    list(GET f 4 options)

    math(EXPR checked "${checked} + 1")

    if(elf_platform)
        if(NOT options MATCHES "${mitigation}")
            list(APPEND failures
                 "${name}: took the contract but its compile options do not carry the "
                 "mitigation (verdict was '${verdict}', options were '${options}')")
        endif()
        zen_unique_symbols("${artifact}" "${kind}" n syms)
        if(NOT n EQUAL 0)
            list(APPEND failures
                 "${name}: ${n} STB_GNU_UNIQUE symbol(s) survived the contract -- ${syms}")
        endif()
    else()
        # PE-COFF / Mach-O: the contract must have applied NOTHING.
        if(options MATCHES "${mitigation}")
            list(APPEND failures
                 "${name}: an ELF-only compile option was injected on ${ZEN_PLATFORM} "
                 "(options were '${options}'). This compiler accepting the flag is not "
                 "the same as this platform needing it.")
        endif()
    endif()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR
        "weave_contract: the manifest named ZERO contracted targets. Either nothing in "
        "this configuration builds a loadable weave, or loom_weave_build_contract() "
        "stopped recording what it touched -- and a check over an empty population is "
        "not a pass (POP-01).")
endif()

# ---- claim 2: the bypass twin can still express the failure -----------------------

if(elf_platform)
    zen_unique_symbols("${ZEN_BYPASS_LIB}" SHARED_LIBRARY bypass_n bypass_syms)
    set(sentinel_seen FALSE)
    foreach(s IN LISTS bypass_syms)
        if(s MATCHES "reload_sentinel")
            set(sentinel_seen TRUE)
        endif()
    endforeach()
    if(NOT sentinel_seen)
        list(APPEND failures
             "the BYPASS control has no unique reload_sentinel symbol (${bypass_n} unique "
             "symbols in total). The negative control no longer reproduces the binding, so "
             "the contracted artifacts above prove nothing: same source, same compiler, "
             "and the difference is supposed to be the contract alone.")
    endif()
endif()

# ---- claim 3: the host binary did not catch the contract --------------------------

if(ZEN_HOST_OPTIONS MATCHES "${mitigation}")
    list(APPEND failures
         "the host test binary received the weave-only compile option. The contract is "
         "supposed to reach exactly the targets handed to it, and nothing else.")
endif()
if(elf_platform)
    zen_unique_symbols("${ZEN_HOST_EXE}" EXECUTABLE host_n host_syms)
    if(host_n EQUAL 0)
        list(APPEND failures
             "the host test binary carries no unique symbols at all, so 'the contract did "
             "not leak onto it' is unfalsifiable here -- this control has gone vacuous.")
    endif()
endif()

# ---- verdict ----------------------------------------------------------------------

if(NOT failures STREQUAL "")
    string(REPLACE ";" "\n  - " pretty "${failures}")
    message(FATAL_ERROR "weave_contract FAILED:\n  - ${pretty}")
endif()

if(elf_platform)
    message(STATUS
        "weave_contract: ${checked} contracted artifacts carry 0 STB_GNU_UNIQUE symbols; "
        "the bypass control still reproduces the binding; the host binary kept its "
        "${host_n} unique symbols and none of the weave option.")
else()
    message(STATUS
        "weave_contract: ${checked} contracted artifacts on ${ZEN_PLATFORM}; the ELF-only "
        "option was applied to none of them, and not to the host binary.")
endif()
