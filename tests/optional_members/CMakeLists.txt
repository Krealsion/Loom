# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The optional-member witness (the `optional_member_types` entry). A ZEN_SHAPE member of type
# std::optional<T> is an optional field of T's type; a std::optional anywhere else -- a list's
# element, an optional's value, a field type asked for directly -- is refused at compile time.

# A project of its own, as tests/hook_return is, because `try_compile` writes the tree it builds
# in. The positive controls prove the harness compiles the supported forms; each negative must be
# refused by the static assertion that names its rule. The configure step is the test.
#   cmake -S tests/optional_members -B <own tree> -DZEN_LOOM_INCLUDE=<repo>/include

cmake_minimum_required(VERSION 3.22)

# Compile only: nothing is linked (see tests/hook_return/CMakeLists.txt for why, and for the
# compiler environment an MSVC tree hands through).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
foreach(var IN ITEMS INCLUDE LIB)
    if(DEFINED ZEN_MEMBERS_ENV_${var} AND NOT ZEN_MEMBERS_ENV_${var} STREQUAL "")
        string(REPLACE "|" ";" zen_members_env_value "${ZEN_MEMBERS_ENV_${var}}")
        set(ENV{${var}} "${zen_members_env_value}")
    endif()
endforeach()

project(zen_optional_member_witness CXX)

if(NOT DEFINED ZEN_LOOM_INCLUDE OR NOT EXISTS "${ZEN_LOOM_INCLUDE}/zen/weave.hpp")
    message(FATAL_ERROR
        "optional_members: -DZEN_LOOM_INCLUDE=<Loom include directory> is required and must "
        "contain zen/weave.hpp (got '${ZEN_LOOM_INCLUDE}')")
endif()

# MSVC expands ZEN_EXPOSE(x) and ZEN_HIDE(x) (__VA_OPT__) only under its conforming preprocessor.
# try_compile builds with this project's CMAKE_CXX_FLAGS, so the switch is set here.
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /Zc:preprocessor")
endif()

function(zen_members_try name out_ok out_text)
    # try_compile caches its result variable, so each source has its own, cleared every run.
    unset(zen_members_ok_${name} CACHE)
    try_compile(zen_members_ok_${name} "${CMAKE_BINARY_DIR}/try-${name}"
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${ZEN_LOOM_INCLUDE}"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE text)
    set(${out_ok} "${zen_members_ok_${name}}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

# Positive controls: every kind through every derivation and a woven state, and from_value alone.
foreach(name IN ITEMS every_kind from_value_only)
    zen_members_try(${name} ok text)
    if(NOT ok)
        message(FATAL_ERROR
            "optional_members: POSITIVE CONTROL ${name}.cpp did not compile, so the negatives "
            "below would prove nothing.\n${text}")
    endif()
    message(STATUS "optional_members: ${name} compiles (supported members)")
endforeach()

# Each negative, and the words of the static assertion that must refuse it: a refusal for any
# other reason is not the rule this entry pins.
set(zen_members_negatives
    "list_element|a list's element (std::vector<std::optional<T>>) or an optional's value"
    "optional_value|a list's element (std::vector<std::optional<T>>) or an optional's value"
    "unsupported_inner|a ZEN_SHAPE member may also be std::optional<T> of one of them"
    "not_a_type|std::optional<T> is a field's optionality, not a field type")
foreach(entry IN LISTS zen_members_negatives)
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} name)
    math(EXPR after "${bar} + 1")
    string(SUBSTRING "${entry}" ${after} -1 words)
    zen_members_try(${name} ok text)
    if(ok)
        message(FATAL_ERROR "optional_members: ${name}.cpp COMPILED; the form it holds must be "
                            "refused at compile time")
    endif()
    string(FIND "${text}" "${words}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR
            "optional_members: ${name}.cpp was refused, but not by the static assertion that "
            "says \"${words}\".\n${text}")
    endif()
    message(STATUS "optional_members: ${name} refused by name")
endforeach()
