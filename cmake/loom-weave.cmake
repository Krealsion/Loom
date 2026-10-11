# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The weave build contract (KERN-05): `loom_weave_build_contract(<target>)` applies to a weave's
# library what this platform and compiler need for `dlclose` to end that image's static lifetime,
# and on Windows for the image to carry its own C++ runtime. Forgetting it is no build error: the
# image stays resident after an unload, or runs on whichever runtime the machine holds. It is not
# isolation or trust, not every dlclose hazard, and not a runtime check (docs/reference/kernel.md).

# On the MSVC ABI the C++ runtime is fixed in every object as it is compiled, and every object of
# one image must agree, so a weave carries its own only if Loom's libraries, which it links, are
# built for the static runtime, and so then is every image that links them. Whoever includes this
# file, Loom's build or a project's find_package(loom), takes it as the default runtime; a project
# that names another is refused here. Before the guard below: each including directory needs it.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(LOOM_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "${LOOM_MSVC_RUNTIME_LIBRARY}")
    elseif(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL LOOM_MSVC_RUNTIME_LIBRARY)
        message(FATAL_ERROR
            "loom: this project names the C++ runtime '${CMAKE_MSVC_RUNTIME_LIBRARY}', and Loom's "
            "libraries are built for the static one, '${LOOM_MSVC_RUNTIME_LIBRARY}'. On the MSVC "
            "ABI every object of one image must agree, and a weave carries its own runtime only "
            "with the static one (KERN-05), so leave CMAKE_MSVC_RUNTIME_LIBRARY unset or set it "
            "to that.")
    endif()
    # Where flags choose the runtime (policy CMP0091 old, or a /MD written by hand) the default
    # above is ignored, and every image linking Loom's libraries would fail to link.
    foreach(_loom_flags IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(" ${${_loom_flags}} " MATCHES " [-/]MDd? ")
            message(FATAL_ERROR
                "loom: ${_loom_flags} chooses the DLL C++ runtime (${${_loom_flags}}), and "
                "Loom's libraries are built for the static one. Leave the runtime to CMake "
                "(policy CMP0091 new, cmake_minimum_required 3.15 or later, and no /MD in the "
                "flags).")
        endif()
    endforeach()
endif()

if(COMMAND loom_weave_build_contract)
    return() # the build tree and the installed package may both include this file
endif()

# What every MinGW-w64 weave links beside its runtime, found beside this file in Loom's source and
# in the package alike. A global property, since the function runs in whichever directory calls it.
set_property(GLOBAL PROPERTY LOOM_WEAVE_TLS_RETURN_SOURCE
             "${CMAKE_CURRENT_LIST_DIR}/loom-weave-tls-return.cpp")

# Apply Loom's weave build contract to an existing shared-library target:
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
            "about a dynamically loaded image: apply it to the weave's SHARED/MODULE library "
            "and to any STATIC/OBJECT library linked into it, not to a host executable or an "
            "INTERFACE target.")
    endif()

    # ELF with GNU unique binding is where statics escape an unload: GCC on Linux emits
    # vague-linkage statics (inline function statics, template statics, inline variables) as
    # STB_GNU_UNIQUE, and the option takes them to zero. PE-COFF and Mach-O have no unique
    # binding, and MinGW GCC accepting the option's spelling does not make it needed. On PE-COFF
    # the image instead carries its own C++ runtime. Anything else is unclassified, and says so.
    if(WIN32)
        set(_loom_pe "PE-COFF has no unique symbol binding")
        if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            # The property is ignored where flags choose the runtime (policy CMP0091 old, or a
            # /MD written by hand), and then the contract cannot be expressed.
            foreach(_loom_flags IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG
                    CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO
                    CMAKE_CXX_FLAGS_MINSIZEREL)
                if(" ${${_loom_flags}} " MATCHES " [-/]MDd? ")
                    message(FATAL_ERROR
                        "loom_weave_build_contract: ${_loom_flags} chooses the DLL C++ runtime "
                        "(${${_loom_flags}}), so '${target}' cannot be given its own. Leave the "
                        "runtime to CMake (policy CMP0091 new, cmake_minimum_required 3.15 or "
                        "later, and no /MD in the flags).")
                endif()
            endforeach()
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                         "${LOOM_MSVC_RUNTIME_LIBRARY}")
            set(_loom_wv "applied: the static C++ runtime (MultiThreaded) -- ${_loom_pe}")
        elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
            if(_loom_wt MATCHES "^(SHARED|MODULE)_LIBRARY$")
                # Only toolchains a reload has been measured on: the lowest GCC and Clang measured
                # whose weave came back whole over more loads than a process has TLS indexes. An
                # older one is refused, not assumed.
                set(LOOM_WEAVE_MINGW_GCC_LEAST 12.2)
                set(LOOM_WEAVE_MINGW_CLANG_LEAST 18.1)
                if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                    set(_loom_least ${LOOM_WEAVE_MINGW_GCC_LEAST})
                else()
                    set(_loom_least ${LOOM_WEAVE_MINGW_CLANG_LEAST})
                endif()
                if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _loom_least)
                    message(FATAL_ERROR
                        "loom_weave_build_contract: ${CMAKE_CXX_COMPILER_ID} "
                        "${CMAKE_CXX_COMPILER_VERSION} targeting MinGW-w64 is older than any "
                        "this Loom has measured a weave's reload on (${CMAKE_CXX_COMPILER_ID} "
                        "${_loom_least} and later), so whether '${target}' gives back what its "
                        "runtime takes is unknown. Refusing to build it as a weave.")
                endif()
                # A libstdc++ that registers a thread_local's destructor itself, rather than through
                # the C runtime's __cxa_thread_atexit, pins the image while the thread that used it
                # lives: an unload that reports success leaves it in place, and a thread that
                # outlives the unload hangs the process.
                include(CheckCXXSourceCompiles)
                string(CONCAT _loom_probe "#include <cstdlib>\n"
                       "#if defined(__GLIBCXX__) && !defined(_GLIBCXX_HAVE___CXA_THREAD_ATEXIT)\n"
                       "#error the destructor of a thread_local is registered by libstdc++ itself\n"
                       "#endif\nint main() { return 0; }")
                check_cxx_source_compiles("${_loom_probe}" LOOM_CXX_THREAD_EXIT_FROM_CRT)
                if(NOT LOOM_CXX_THREAD_EXIT_FROM_CRT)
                    message(FATAL_ERROR
                        "loom_weave_build_contract: this MinGW-w64 toolchain's libstdc++ was "
                        "built without the C runtime's __cxa_thread_atexit "
                        "(_GLIBCXX_HAVE___CXA_THREAD_ATEXIT is not defined), so a weave holding a "
                        "thread_local with a destructor stays loaded after an unload that reports "
                        "success, and a thread that outlives the unload hangs the process. "
                        "Refusing to build '${target}' as a weave; a toolchain whose libstdc++ "
                        "defines it does not have this.")
                endif()
                # The image's copy of the runtime takes Windows TLS and FLS indexes it never
                # returns, and reserves memory for exceptions it never frees, so the image also
                # links loom-weave-tls-return.cpp, which gives them back when it unloads, with the
                # import slots the runtime calls wrapped. Verify rather than assume: a toolchain
                # that cannot link its runtime statically, or whose linker cannot wrap an import
                # slot, is refused rather than called self-contained.
                get_property(_loom_tls GLOBAL PROPERTY LOOM_WEAVE_TLS_RETURN_SOURCE)
                set(_loom_wrap "")
                foreach(_loom_slot IN ITEMS TlsAlloc TlsFree FlsAlloc FlsFree)
                    list(APPEND _loom_wrap "LINKER:--wrap=__imp_${_loom_slot}")
                endforeach()
                set(CMAKE_REQUIRED_LINK_OPTIONS -static ${_loom_wrap})
                string(CONCAT _loom_probe "#include \"${_loom_tls}\"\n#include <string>\n"
                       "int main() { return static_cast<int>(std::string(\"x\").size()) - 1; }")
                check_cxx_source_compiles("${_loom_probe}" LOOM_CXX_LINKS_WEAVE_RUNTIME)
                unset(CMAKE_REQUIRED_LINK_OPTIONS)
                if(NOT LOOM_CXX_LINKS_WEAVE_RUNTIME)
                    message(FATAL_ERROR
                        "loom_weave_build_contract: this MinGW-w64 toolchain cannot link its C++ "
                        "runtime statically (-static) with the import slots of TlsAlloc, TlsFree, "
                        "FlsAlloc and FlsFree wrapped (--wrap), so '${target}' would either load "
                        "only beside whichever runtime a machine holds or keep what its runtime "
                        "takes after it unloads. Refusing to build it as a weave rather than call "
                        "it self-contained.")
                endif()
                target_sources(${target} PRIVATE "${_loom_tls}")
                target_link_options(${target} PRIVATE -static ${_loom_wrap})
                string(CONCAT _loom_wv "applied: -static, the C++ runtime linked into the image, "
                       "and what it takes returned at unload (loom-weave-tls-return.cpp) "
                       "-- ${_loom_pe}")
            else()
                string(CONCAT _loom_wv "not applicable to an archive: the image that links it "
                       "carries the runtime -- ${_loom_pe}")
            endif()
        else()
            message(WARNING
                "loom_weave_build_contract: ${CMAKE_CXX_COMPILER_ID} on Windows is not a "
                "combination this Loom has measured. Nothing was applied to '${target}', so it "
                "imports whatever C++ runtime this compiler links by default; verify its imports "
                "yourself.")
            string(CONCAT _loom_wv "unclassified: ${CMAKE_CXX_COMPILER_ID}/${CMAKE_SYSTEM_NAME}, "
                   "nothing applied")
        endif()
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
