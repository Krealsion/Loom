# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# Every program Loom builds runs in the UTF-8 code page on Windows, its tests included: a manifest
# says so, and `argv`, the environment and every narrow path are then UTF-8 there as on every other
# platform (docs/guides/tools.md). What a toolchain's own manifest says stays. Only programs carry
# it: a library leaves the code page to the program that hosts it.

set_property(GLOBAL PROPERTY LOOM_CODE_PAGE_MANIFEST
             "${CMAKE_CURRENT_LIST_DIR}/utf8-code-page.manifest")

# Under MinGW-w64: the manifest linked into every program, as one object CMake's resource compiler
# builds in the build tree. The toolchain may link a default manifest of its own, and a program's
# own replaces it, so this one is the default with the code page added; a toolchain that links
# none gets the code page alone.
function(loom_mingw_code_page)
    get_property(fragment GLOBAL PROPERTY LOOM_CODE_PAGE_MANIFEST)
    file(READ "${fragment}" manifest)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=default-manifest.o
                    OUTPUT_VARIABLE default OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE asked ERROR_QUIET)
    set(kept "the toolchain links no manifest of its own")
    set(configured_from "")
    if(asked EQUAL 0 AND IS_ABSOLUTE "${default}" AND EXISTS "${default}")
        # The default's text is one printable run of its object, from `<?xml` to `</assembly>`.
        file(STRINGS "${default}" runs NEWLINE_CONSUME)
        set(theirs "")
        foreach(run IN LISTS runs)
            string(FIND "${run}" "<?xml" at)
            if(at GREATER -1)
                string(SUBSTRING "${run}" ${at} -1 theirs)
                break()
            endif()
        endforeach()
        string(FIND "${theirs}" "</assembly>" theirs_end)
        string(FIND "${manifest}" "<application" ours_begin)
        string(FIND "${manifest}" "</assembly>" ours_end)
        if(theirs_end EQUAL -1 OR ours_begin EQUAL -1 OR ours_end EQUAL -1)
            message(FATAL_ERROR
                "loom: cannot read the manifest ${default} links into every program")
        endif()
        string(SUBSTRING "${theirs}" 0 ${theirs_end} head)
        math(EXPR length "${ours_end} - ${ours_begin}")
        string(SUBSTRING "${manifest}" ${ours_begin} ${length} added)
        set(manifest "${head}  ${added}</assembly>\n")
        set(kept "the toolchain's own kept: ${default}")
        set(configured_from "${default}")
    endif()
    # The manifest is written into the resource script, one quoted line each, so the resource
    # compiler opens no other file; and the script is written only when its text changes, so a
    # configure relinks nothing it did not change.
    string(REPLACE "\r" "" quoted "${manifest}")
    string(REGEX REPLACE "\n$" "" quoted "${quoted}")
    string(REPLACE "\\" "\\\\" quoted "${quoted}")
    string(REPLACE "\"" "\"\"" quoted "${quoted}")
    string(REPLACE "\n" "\\n\"\n\"" quoted "${quoted}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/loom-code-page.rc")
    file(WRITE "${rc}.new" "1 24\nBEGIN\n\"${quoted}\\n\"\nEND\n")
    configure_file("${rc}.new" "${rc}" COPYONLY)
    # The resource compiler reads its command line in its own code page, which may lack a letter
    # of the build directory's name, so it runs in that directory and is given the script and the
    # object by their own names.
    set(object "${CMAKE_CURRENT_BINARY_DIR}/loom-code-page.obj")
    separate_arguments(flags NATIVE_COMMAND "${CMAKE_RC_FLAGS}")
    add_custom_command(OUTPUT "${object}"
                       COMMAND "${CMAKE_RC_COMPILER}" ${flags} -O coff loom-code-page.rc
                               loom-code-page.obj
                       DEPENDS "${rc}"
                       WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                       COMMENT "Building the UTF-8 code page manifest every program links"
                       VERBATIM)
    add_custom_target(loom-code-page DEPENDS "${object}")
    add_library(loom-code-page-object OBJECT IMPORTED GLOBAL)
    set_property(TARGET loom-code-page-object PROPERTY IMPORTED_OBJECTS "${object}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fragment}" ${configured_from})
    set_property(GLOBAL PROPERTY LOOM_CODE_PAGE_KEPT "${kept}")
endfunction()

# loom_program_code_page() -- give the manifest naming UTF-8 its active code page to every
# executable Loom's directories define; called once, after the last. MSVC's linker writes a
# manifest of its own and CMake merges a listed one into it. Under MinGW-w64 the object is one of
# the program's own, which CMake names relative to the build directory, as the linker too reads its
# command line in its own code page. Elsewhere, nothing.
function(loom_program_code_page)
    if(NOT WIN32)
        return()
    endif()
    if(NOT MSVC AND NOT MINGW)
        message(STATUS "loom: no program manifest for this Windows toolchain, so its programs "
                       "run in the system's code page")
        return()
    endif()
    set(programs "")
    set(dirs "${PROJECT_SOURCE_DIR}")
    while(dirs)
        list(GET dirs 0 dir)
        list(REMOVE_AT dirs 0)
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND dirs ${subdirs})
        get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "EXECUTABLE")
                list(APPEND programs ${target})
            endif()
        endforeach()
    endwhile()
    get_property(fragment GLOBAL PROPERTY LOOM_CODE_PAGE_MANIFEST)
    if(MINGW AND NOT TARGET loom-code-page)
        loom_mingw_code_page()
    endif()
    foreach(target IN LISTS programs)
        if(MSVC)
            target_sources(${target} PRIVATE "${fragment}")
        else()
            target_sources(${target} PRIVATE "$<TARGET_OBJECTS:loom-code-page-object>")
            add_dependencies(${target} loom-code-page)
        endif()
    endforeach()
    if(MSVC)
        set(said "merged into the linker's own")
    else()
        get_property(said GLOBAL PROPERTY LOOM_CODE_PAGE_KEPT)
    endif()
    list(LENGTH programs count)
    message(STATUS "loom: ${count} programs run in the UTF-8 code page (${said})")
endfunction()
