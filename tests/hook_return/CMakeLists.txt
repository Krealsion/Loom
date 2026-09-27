# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The hook-return witness (the `hook_return_types` entry). `WeaveBase::claim_published` reads the
# return type of `on_claim_published(const T&)` as its answer: `void` is Applied, `bool` is
# Applied or Declined, `loom::Weave::PublishedClaim` says any of the three
# (docs/reference/joint-publication.md#the-showing-and-its-three-answers). Any other return type
# must be refused at compile time, never read as Applied.

# A project of its own, in a tree of its own, because `try_compile` needs a project and writes the
# tree it builds in. Four positive controls prove the harness compiles the supported forms; one
# negative proves the unsupported form is refused for the stated reason. The configure step is
# the test: it exits nonzero, naming the source, when an expectation is not met.
#   cmake -S tests/hook_return -B <own tree> -DZEN_LOOM_INCLUDE=<repo>/include

cmake_minimum_required(VERSION 3.22)

# COMPILE ONLY, from the compiler check onward. Nothing here is linked, so the
# toolchain's linker, resource compiler and manifest tool are never consulted --
# which is what lets this witness run from the fresh shell an official lane runs
# in, where an MSVC build tree still knows its `cl.exe` by absolute path but the
# dev shell's PATH (rc.exe, link.exe) is gone. Set BEFORE project(), or the
# compiler check links a test executable and fails there.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# THE COMPILER'S OWN ENVIRONMENT, HANDED THROUGH. `cl.exe` finds the standard
# headers through INCLUDE, which the dev shell exported when the OUTER tree was
# configured and a fresh shell does not carry. tests/CMakeLists.txt captures that
# value at configure time and passes it here with `;` spelled `|` (a `;` would
# split the argument into a CMake list); it is restored into this process's
# environment, which every try_compile build inherits. Empty everywhere else.
foreach(var IN ITEMS INCLUDE LIB)
    if(DEFINED ZEN_HOOK_ENV_${var} AND NOT ZEN_HOOK_ENV_${var} STREQUAL "")
        string(REPLACE "|" ";" zen_hook_env_value "${ZEN_HOOK_ENV_${var}}")
        set(ENV{${var}} "${zen_hook_env_value}")
    endif()
endforeach()

project(zen_hook_return_witness CXX)

if(NOT DEFINED ZEN_LOOM_INCLUDE OR NOT EXISTS "${ZEN_LOOM_INCLUDE}/zen/weave.hpp")
    message(FATAL_ERROR
        "hook_return: -DZEN_LOOM_INCLUDE=<Loom include directory> is required and must "
        "contain zen/weave.hpp (got '${ZEN_LOOM_INCLUDE}')")
endif()

set(zen_hook_flags "")
if(MSVC)
    # Loom's public macros use __VA_OPT__, which MSVC supports only under the
    # conforming preprocessor -- the same requirement loom::core carries.
    set(zen_hook_flags "/Zc:preprocessor /EHsc")
endif()

function(zen_hook_try name out_ok out_text)
    # `try_compile` caches its result variable and skips the compile when it is already set, so a
    # shared name would compile the first source only and report its verdict five times. One
    # name per source, cleared before every run.
    unset(zen_hook_ok_${name} CACHE)
    try_compile(zen_hook_ok_${name} "${CMAKE_BINARY_DIR}/try-${name}"
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${ZEN_LOOM_INCLUDE}"
                    "-DCMAKE_CXX_FLAGS=${zen_hook_flags}"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE text)
    set(${out_ok} "${zen_hook_ok_${name}}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

# Positive controls: the three supported answers, and a weave that declares no
# handler at all (applied trivially). A red here is the harness, not the contract.
foreach(name IN ITEMS hook_void hook_bool hook_enum hook_absent)
    zen_hook_try(${name} ok text)
    if(NOT ok)
        message(FATAL_ERROR
            "hook_return: POSITIVE CONTROL ${name}.cpp did not compile. Either the "
            "supported return type stopped being accepted or the witness cannot see "
            "Loom's headers; either way the negative below would prove nothing.\n${text}")
    endif()
    message(STATUS "hook_return: ${name} compiles (supported answer)")
endforeach()

# The negative: an `int` handler. It must fail, and it must fail on the static
# assertion that names the rule -- a failure for any other reason (a missing header,
# a typo in the fixture) is not the refusal this entry exists to pin.
zen_hook_try(hook_int ok text)
if(ok)
    message(FATAL_ERROR
        "hook_return: hook_int.cpp COMPILED. An on_claim_published returning `int` was "
        "accepted, which means the construction layer is reading an answer it cannot "
        "understand -- the permissive fallback is back, and an unreadable answer is "
        "being recorded as Applied.")
endif()
if(NOT text MATCHES "on_claim_published must return void")
    message(FATAL_ERROR
        "hook_return: hook_int.cpp was refused, but not by the static assertion that "
        "names the rule. Whatever refused it is not the contract this entry pins.\n${text}")
endif()
message(STATUS "hook_return: hook_int refused by name (unsupported answer)")
