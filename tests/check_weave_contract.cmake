# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `weave_contract` entry (KERN-05) reads the built artifacts, not the build system's opinion of
# them. On ELF: (1) no contracted artifact carries an STB_GNU_UNIQUE symbol; (2) the uncontracted
# twin still does; (3) the host binary caught no contracted target's options. On PE-COFF: (4) the
# ELF-only option was applied to nothing; (5) no contracted image imports a C++ runtime library;
# (6) the twin still does; (7) under MinGW-w64 the host binary still imports its own.

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

# ---- the PE reader -----------------------------------------------------------------
#
# The libraries a PE image imports, delay-loaded ones too, lower-cased: objdump's `DLL Name:`
# lines under MinGW-w64, the MSVC linker's `-dump -dependents` list on the MSVC ABI. A C++ runtime
# library is one of these names; the C runtime Windows itself holds (msvcrt, the UCRT) is not.
set(zen_runtime_libraries "^(libstdc\\+\\+|libgcc_s|libwinpthread|libc\\+\\+|libunwind|")
string(APPEND zen_runtime_libraries "libmcfgthread|vcruntime|msvcp|concrt|vccorlib|ucrtbased)")
set(zen_pe_kind "")
if(ZEN_PLATFORM STREQUAL "Windows")
    if(NOT DEFINED ZEN_PE_READER OR ZEN_PE_READER STREQUAL "" OR NOT EXISTS "${ZEN_PE_READER}")
        message(FATAL_ERROR
            "weave_contract: no reader for a PE image's imports ('${ZEN_PE_READER}'), so whether "
            "a contracted weave carries its own C++ runtime cannot be read. It fails rather than "
            "skips: name binutils' objdump, or the MSVC linker, at configure.")
    endif()
    get_filename_component(zen_pe_kind "${ZEN_PE_READER}" NAME_WE)
    string(TOLOWER "${zen_pe_kind}" zen_pe_kind)
    if(NOT zen_pe_kind MATCHES "objdump")
        set(zen_pe_kind "link")
    endif()
endif()

function(zen_pe_imports path out)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "weave_contract: artifact missing: ${path}")
    endif()
    if(zen_pe_kind STREQUAL "link")
        execute_process(COMMAND "${ZEN_PE_READER}" -dump -nologo -dependents "${path}"
                        OUTPUT_VARIABLE dump RESULT_VARIABLE rc ERROR_VARIABLE err)
        set(pattern "^[ \t]+([^ \t\r]+\\.[dD][lL][lL])[ \t\r]*$")
    else()
        execute_process(COMMAND "${ZEN_PE_READER}" -p "${path}"
                        OUTPUT_VARIABLE dump RESULT_VARIABLE rc ERROR_VARIABLE err)
        set(pattern "^[ \t]*DLL Name: ([^ \t\r]+)")
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "weave_contract: ${ZEN_PE_READER} failed on ${path} (exit ${rc})\n${err}")
    endif()
    string(REPLACE "\n" ";" lines "${dump}")
    set(names "")
    foreach(line IN LISTS lines)
        if(line MATCHES "${pattern}")
            string(TOLOWER "${CMAKE_MATCH_1}" lowered)
            list(APPEND names "${lowered}")
        endif()
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

function(zen_runtime_imports path out)
    zen_pe_imports("${path}" names)
    set(runtime "")
    foreach(n IN LISTS names)
        if(n MATCHES "${zen_runtime_libraries}")
            list(APPEND runtime "${n}")
        endif()
    endforeach()
    set(${out} "${runtime}" PARENT_SCOPE)
endfunction()

# On the MSVC ABI an archive's objects say which runtime they were compiled for in their linker
# directives: `RuntimeLibrary=MT...` and LIBCMT for the static one, `MD...` and MSVCRT for the DLL.
function(zen_msvc_archive_runtime path out)
    execute_process(COMMAND "${ZEN_PE_READER}" -dump -nologo -directives "${path}"
                    OUTPUT_VARIABLE dump RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "weave_contract: ${ZEN_PE_READER} failed on ${path} (exit ${rc})\n${err}")
    endif()
    string(TOLOWER "${dump}" dump)
    if(dump MATCHES "runtimelibrary=md" OR dump MATCHES "/defaultlib:\"?msvcrt")
        set(${out} "the DLL runtime" PARENT_SCOPE)
    elseif(dump MATCHES "runtimelibrary=mt" OR dump MATCHES "/defaultlib:\"?libcmt")
        set(${out} "static" PARENT_SCOPE)
    else()
        set(${out} "no runtime it names" PARENT_SCOPE)
    endif()
endfunction()

# ---- claims 1, 4 and 5: every contracted artifact ----------------------------------

file(STRINGS "${ZEN_MANIFEST}" rows)
set(checked 0)
set(images 0)       # PE images read for claim 5
set(archives "")    # PE archives, and how each was judged
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
        # PE-COFF / Mach-O: the ELF-only option must have been applied to NOTHING.
        if(options MATCHES "${mitigation}")
            list(APPEND failures
                 "${name}: an ELF-only compile option was injected on ${ZEN_PLATFORM} "
                 "(options were '${options}'). This compiler accepting the flag is not "
                 "the same as this platform needing it.")
        endif()
        # PE-COFF: a contracted image carries its C++ runtime, so it imports none. An archive's
        # runtime is chosen when the image that links it is linked under MinGW-w64, and in each
        # of its objects on the MSVC ABI.
        if(ZEN_PLATFORM STREQUAL "Windows" AND kind MATCHES "^(SHARED|MODULE)_LIBRARY$")
            math(EXPR images "${images} + 1")
            zen_runtime_imports("${artifact}" runtime)
            string(REPLACE ";" ", " runtime "${runtime}")
            if(runtime)
                list(APPEND failures
                     "${name}: imports its C++ runtime (${runtime}) although it took the "
                     "contract (verdict '${verdict}'), so the runtime it runs with is whichever "
                     "copy the machine or the process holds")
            endif()
        elseif(ZEN_PLATFORM STREQUAL "Windows" AND zen_pe_kind STREQUAL "link")
            zen_msvc_archive_runtime("${artifact}" built_for)
            list(APPEND archives "${name} (${built_for})")
            if(NOT built_for STREQUAL "static")
                list(APPEND failures
                     "${name}: an archive inside contracted images, compiled for ${built_for}, "
                     "not the static runtime those images carry (verdict '${verdict}')")
            endif()
        elseif(ZEN_PLATFORM STREQUAL "Windows")
            list(APPEND archives "${name} (its image's link)")
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

if(ZEN_PLATFORM STREQUAL "Windows" AND images EQUAL 0)
    message(FATAL_ERROR
        "weave_contract: the manifest named no contracted SHARED or MODULE image on Windows, so "
        "claim 5 judged nothing -- and a check over an empty population is not a pass (POP-01).")
endif()

# ---- claims 2 and 6: the bypass twin can still express the failure ------------------

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
set(bypass_runtime "")
if(ZEN_PLATFORM STREQUAL "Windows")
    zen_runtime_imports("${ZEN_BYPASS_LIB}" bypass_runtime)
    if(NOT bypass_runtime)
        list(APPEND failures
             "the BYPASS control imports no C++ runtime library. The negative control no longer "
             "expresses an image that depends on the machine's runtime, so the contracted "
             "images above prove nothing: same source, same compiler, the contract the only "
             "difference.")
    endif()
endif()

# ---- claims 3 and 7: the host binary did not catch the contract ---------------------

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
# Under MinGW-w64 the runtime is a link option, so a host the contract did not reach still links
# the toolchain's runtime library. On the MSVC ABI every image of this build shares the static
# runtime Loom's libraries are built for, the contract's doing or not, so there is no such claim.
set(host_runtime "")
if(ZEN_PLATFORM STREQUAL "Windows" AND NOT zen_pe_kind STREQUAL "link")
    zen_runtime_imports("${ZEN_HOST_EXE}" host_runtime)
    if(NOT host_runtime)
        list(APPEND failures
             "the host test binary imports no C++ runtime library: the contract's link option "
             "reached a target it was not handed, or this control has gone vacuous.")
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
elseif(ZEN_PLATFORM STREQUAL "Windows")
    string(REPLACE ";" ", " archives "${archives}")
    string(REPLACE ";" ", " bypass_runtime "${bypass_runtime}")
    string(REPLACE ";" ", " host_runtime "${host_runtime}")
    if(host_runtime)
        set(host_said "the host binary still imports its own (${host_runtime})")
    else()
        set(host_said "the host binary shares this build's static runtime (the MSVC ABI)")
    endif()
    message(STATUS
        "weave_contract: ${checked} contracted artifacts on Windows; the ELF-only option was "
        "applied to none of them; ${images} contracted images import no C++ runtime library "
        "(read by ${ZEN_PE_READER}); archives: ${archives}; the bypass control still imports "
        "${bypass_runtime}; ${host_said}.")
else()
    message(STATUS
        "weave_contract: ${checked} contracted artifacts on ${ZEN_PLATFORM}; the ELF-only "
        "option was applied to none of them, and not to the host binary.")
endif()
