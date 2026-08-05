# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE RELOADABLE-WEAVE BUILD CONTRACT (KERN-05, R2F-E).
#
# One exported function, `loom_weave_build_contract(<target>)`, which applies to a
# weave's shared library whatever the current platform and compiler require for
# `dlclose` to actually end that image's static lifetime.
#
# WHY THIS IS A MECHANISM AND NOT A SENTENCE IN A GUIDE. The Loom already knew this
# law and had already fixed it for itself -- the flag sat in tests/CMakeLists.txt and
# in Zengine's private helper -- while the installed package handed a stranger nothing
# but prose (COLD-1 F-22). Forgetting it does not produce a build error. It produces a
# use-after-free on unload/reload, and the loader reports success on the way there:
# measured on GCC 11.4/glibc 2.35, `dlclose()` returns 0, `kernel.unload()` returns
# true, and the image is still resident afterwards -- so the next load of a DIFFERENT
# library sharing the same vocabulary header binds to the dead image's statics. A
# safety property whose only enforcement is the reader's memory is not enforced.
#
# WHAT THE HAZARD ACTUALLY IS. The maker path (ZEN_SHAPE + WeaveBase) instantiates
# loom's inline templates -- `schema_of<T>()`'s function-local static (shape.hpp) --
# with vague linkage. On ELF, GCC configured --enable-gnu-unique-object emits those as
# STB_GNU_UNIQUE, and glibc resolves unique symbols through a PROGRAM-WIDE table that
# ignores RTLD_LOCAL, marks the defining image NODELETE, and outlives dlclose. The
# Loom's own fixtures never hit it because their shapes live in anonymous namespaces
# (internal linkage is never unique) -- which is exactly why the house did not find
# this and its first real consumer did.
#
# WHAT THIS FUNCTION DOES NOT MEAN
#   - it does not make dlopen'ed code isolated, or trusted, or sandboxed;
#   - it does not make every shared library in the process reload-safe;
#   - it does not reach a consumer who never calls it (that is a supported path, not
#     a cage -- see the manual-build note in docs/guides/dynamic-weaves.md);
#   - it does not solve every dlclose hazard: a library registering a callback, an
#     atexit handler, or a thread that outlives it is still that author's problem;
#   - it is not a runtime check. Nothing here inspects a loaded artifact's symbols.
#
# WHY A FUNCTION AND NOT AN INTERFACE TARGET. An INTERFACE target's linkage scope is
# the consumer's choice, so `PUBLIC` would leak the option onto everything downstream;
# it cannot refuse a target type that makes the contract a lie; and it cannot fail
# loudly on a compiler that cannot express the contract. A function modifies exactly
# the target it is handed, PRIVATE, and can do all three. There was also no existing
# weave-only target to hang it on: a weave links loom::core and loom::switchboard, and
# so does every host executable, so either of those would have sprayed an ELF compile
# option across unrelated consumers.

if(COMMAND loom_weave_build_contract)
    return() # the build tree and the installed package may both include this file
endif()

# Apply Loom's reloadable-weave build contract to an existing shared-library target.
#
#   add_library(my-weave SHARED my_weave.cpp)
#   target_link_libraries(my-weave PRIVATE loom::core loom::switchboard)
#   loom_weave_build_contract(my-weave)
#
# It deliberately does NOT create the target: source lists, target naming, output
# layout, extra libraries and project definitions are the consumer's decisions, and
# Loom owning them would make this a plugin SDK rather than the one law it is.
#
# The verdict is recorded on the target as the LOOM_WEAVE_BUILD_CONTRACT property, so
# what was imposed is readable rather than assumed -- the same habit the runtime keeps
# with Kernel::containment_note().
function(loom_weave_build_contract target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "loom_weave_build_contract: '${target}' is not a target. Create the weave "
            "library first, then hand it to this function.")
    endif()

    # THE CONTRACT COVERS A COMPILATION, NOT A FILE. Every translation unit that ends up
    # inside a loadable image needs it -- the weave's own sources AND any static library
    # linked into it. R2F-E found that the hard way: with the option on the weave's TU
    # alone, `libzen_test_weave.so` still would not unload, because libloom.a had been
    # compiled without it and dragged 20 of its own unique symbols in. ONE such symbol
    # is enough to mark the whole image NODELETE. So STATIC and OBJECT libraries are
    # legitimate subjects here, and Loom applies it to its own core and switchboard.
    #
    # An EXECUTABLE is not: it is never the thing dlopen'ed, and claiming a reloadable
    # lifetime for it would be a claim with no subject. An INTERFACE library has no
    # compilation of its own to contract.
    get_target_property(_loom_wt ${target} TYPE)
    if(NOT _loom_wt MATCHES "^(SHARED|MODULE|STATIC|OBJECT)_LIBRARY$")
        message(FATAL_ERROR
            "loom_weave_build_contract: '${target}' is a ${_loom_wt}. This contract is "
            "about ending a dynamically loaded image's static lifetime; apply it to the "
            "weave's SHARED/MODULE library and to any STATIC/OBJECT library linked into "
            "it, not to a host executable or an INTERFACE target.")
    endif()

    # The affected combination is ELF + GNU. Classified from measurement on the lanes
    # this project actually has, not from the flag's spelling:
    #
    #   Linux / GCC 11.4 (--enable-gnu-unique-object)  AFFECTED. Four vague-linkage
    #       patterns are emitted STB_GNU_UNIQUE (inline function statics, function
    #       template statics, class template static members, inline variables); the
    #       option takes all four to zero.
    #   Windows / MinGW GCC 13.1 (PE-COFF)             NOT AFFECTED. PE has no unique
    #       binding at all -- DLL statics are per-DLL by construction. This compiler
    #       ACCEPTS the option (exit 0), which is precisely why the predicate must be
    #       semantic: a flag a compiler tolerates is not a contract it needs.
    #   Apple / Mach-O                                 NOT AFFECTED, same reasoning.
    #   anything else                                  UNCLASSIFIED, and said so out
    #       loud rather than assumed either way.
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
