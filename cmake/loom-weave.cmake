# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The reloadable-weave build contract (KERN-05): `loom_weave_build_contract(<target>)` applies to
# a weave's library whatever this platform and compiler need for `dlclose` to end that image's
# static lifetime. Forgetting it is no build error: on ELF/GNU the image stays resident after an
# unload that reports success, and the next load binds to its statics. It is not isolation or
# trust, not every dlclose hazard, and not a runtime check (docs/reference/kernel.md).

if(COMMAND loom_weave_build_contract)
    return() # the build tree and the installed package may both include this file
endif()

# Apply Loom's reloadable-weave build contract to an existing shared-library target:
#   add_library(my-weave SHARED my_weave.cpp)
#   target_link_libraries(my-weave PRIVATE loom::core loom::switchboard)
#   loom_weave_build_contract(my-weave)
# It does not create the target. The verdict is recorded on the target's
# LOOM_WEAVE_BUILD_CONTRACT property, so what was imposed is readable rather than assumed.
function(loom_weave_build_contract target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "loom_weave_build_contract: '${target}' is not a target. Create the weave "
            "library first, then hand it to this function.")
    endif()

    # The contract covers a compilation, not a file: every translation unit inside a loadable
    # image needs it, the weave's own and any static library linked in, since one unique symbol
    # marks the whole image NODELETE. So STATIC and OBJECT libraries are subjects; an EXECUTABLE
    # is never dlopen'ed and an INTERFACE library has no compilation, so both are refused.
    get_target_property(_loom_wt ${target} TYPE)
    if(NOT _loom_wt MATCHES "^(SHARED|MODULE|STATIC|OBJECT)_LIBRARY$")
        message(FATAL_ERROR
            "loom_weave_build_contract: '${target}' is a ${_loom_wt}. This contract is "
            "about ending a dynamically loaded image's static lifetime; apply it to the "
            "weave's SHARED/MODULE library and to any STATIC/OBJECT library linked into "
            "it, not to a host executable or an INTERFACE target.")
    endif()

    # The affected combination is ELF with GNU unique binding: GCC on Linux emits vague-linkage
    # statics (inline function statics, template statics, inline variables) as STB_GNU_UNIQUE,
    # and the option takes them to zero. PE-COFF and Mach-O have no unique binding, and MinGW GCC
    # accepting the option's spelling does not make it needed. Anything else is unclassified, and
    # the verdict says so.
    if(WIN32)
        set(_loom_wv "not applicable: PE-COFF has no unique symbol binding")
    elseif(APPLE)
        set(_loom_wv "not applicable: Mach-O has no unique symbol binding")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Verify rather than assume the compiler took it. The project's rule is that a
        # claim of enforcement is only worth what was actually imposed, and this check
        # discriminates: -fno-gnu-unique passes, a deliberately misspelt one fails.
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-fno-gnu-unique LOOM_CXX_HAS_FNO_GNU_UNIQUE)
        if(NOT LOOM_CXX_HAS_FNO_GNU_UNIQUE)
            message(FATAL_ERROR
                "loom_weave_build_contract: this GNU compiler targets ELF, where C++ "
                "vague-linkage statics can be emitted STB_GNU_UNIQUE and escape their "
                "image's lifetime, but it rejects -fno-gnu-unique -- so the contract "
                "cannot be expressed here. Refusing to build '${target}' as a "
                "reloadable weave rather than call it reload-safe.")
        endif()
        target_compile_options(${target} PRIVATE -fno-gnu-unique)
        set(_loom_wv "applied: -fno-gnu-unique")
    else()
        message(WARNING
            "loom_weave_build_contract: ${CMAKE_CXX_COMPILER_ID} on ${CMAKE_SYSTEM_NAME} "
            "is not a combination this Loom has measured. No unique-symbol mitigation "
            "was applied to '${target}'. If this toolchain can give a shared library "
            "symbols that outlive dlclose, verify the artifact yourself.")
        set(_loom_wv "unclassified: ${CMAKE_CXX_COMPILER_ID}/${CMAKE_SYSTEM_NAME}, nothing applied")
    endif()

    set_property(TARGET ${target} PROPERTY LOOM_WEAVE_BUILD_CONTRACT "${_loom_wv}")

    # The roll of everything that took the contract, so a project can check its own
    # artifacts against a DERIVED list rather than a hand-kept one that drifts (the
    # lesson POP-01 paid for). Loom's own tests read it; it is inert for everyone else.
    set_property(GLOBAL APPEND PROPERTY LOOM_WEAVE_CONTRACT_TARGETS ${target})
endfunction()
