cmake_minimum_required(VERSION 3.16)

project(zen-core
    VERSION 0.1.0
    DESCRIPTION "Self-describing-value-and-gate core for Zen"
    LANGUAGES CXX)

# ---- Project-wide policy --------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The kernel loads Shards from shared libraries, which statically link the core.
# Build everything position-independent so the static libraries can be pulled
# into a .so.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZEN_WERROR    "Treat warnings as errors (CI default)"        ON)
option(ZEN_SANITIZE  "Build with -fsanitize=address,undefined"     OFF)
option(ZEN_BUILD_TESTS    "Build the test suite"                    ON)
option(ZEN_BUILD_EXAMPLES "Build the examples"                      ON)

# Warnings that every Zen target inherits. The library earns trust by being
# clean under the strictest reasonable flags.
add_library(zen-warnings INTERFACE)
target_compile_options(zen-warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<BOOL:${ZEN_WERROR}>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${ZEN_WERROR}>>:/WX>)

# Sanitizers, applied to compile and link of everything that opts in.
add_library(zen-sanitize INTERFACE)
if(ZEN_SANITIZE)
    target_compile_options(zen-sanitize INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(zen-sanitize INTERFACE
        -fsanitize=address,undefined)
endif()

# ---- The library ----------------------------------------------------------

add_library(zen-core
    src/schema.cpp
    src/value.cpp
    src/gate.cpp
    src/registry.cpp
    src/serialize.cpp
    src/detail/json.cpp
    src/detail/base64.cpp
    src/detail/binary.cpp)

target_include_directories(zen-core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(zen-core PUBLIC cxx_std_20)
target_link_libraries(zen-core
    PUBLIC  zen-sanitize
    PRIVATE zen-warnings)

add_library(zen::core ALIAS zen-core)

# ---- The switchboard (the first live boundary) ----------------------------

add_library(zen-switchboard
    src/switchboard/switchboard.cpp)

target_include_directories(zen-switchboard
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-switchboard PUBLIC cxx_std_20)
target_link_libraries(zen-switchboard
    PUBLIC  zen-core zen-sanitize
    PRIVATE zen-warnings)

add_library(zen::switchboard ALIAS zen-switchboard)

# ---- The kernel (DLL loading + the C ABI + hot-reload) --------------------

add_library(zen-kernel
    src/kernel/kernel.cpp)

target_include_directories(zen-kernel
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-kernel PUBLIC cxx_std_20)
target_link_libraries(zen-kernel
    PUBLIC  zen-core zen-switchboard zen-sanitize
    PRIVATE zen-warnings ${CMAKE_DL_LIBS})

add_library(zen::kernel ALIAS zen-kernel)

# ---- Isolation (out-of-process Shard hosting + crash supervision) ---------

# The host-side library: the IPC channel, the proxy Shard, and the supervisor.
# It reuses the kernel's header-only schema codec to reconstruct a child's schemas;
# it does not depend on the kernel's compiled code.
add_library(zen-isolation
    src/isolation/channel.cpp
    src/isolation/host.cpp)

target_include_directories(zen-isolation
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-isolation PUBLIC cxx_std_20)
target_link_libraries(zen-isolation
    PUBLIC  zen-core zen-switchboard zen-sanitize
    PRIVATE zen-warnings)

add_library(zen::isolation ALIAS zen-isolation)

# The child executable: runs one Shard out-of-process via the kernel C ABI. It is
# a byte shuttler — it deliberately does NOT link zen-core; it speaks the wire
# format only through the protocol header and lets the host gate everything.
add_executable(zen-shard-host
    src/isolation/shard_host_main.cpp)

target_include_directories(zen-shard-host
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(zen-shard-host PRIVATE cxx_std_20)
target_link_libraries(zen-shard-host
    PRIVATE zen-warnings zen-sanitize ${CMAKE_DL_LIBS})

# ---- Tests ----------------------------------------------------------------

if(ZEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ZEN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
