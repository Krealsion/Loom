# A single test binary; the doctest framework is compiled exactly once in
# doctest_main.cpp. Each suite is registered with CTest so `ctest` reports them
# individually and `-R <suite>` can target one.

add_executable(zen-tests
    doctest_main.cpp
    test_schema.cpp
    test_value.cpp
    test_gate.cpp
    test_registry.cpp
    test_serialize.cpp
    test_compat.cpp
    test_integration.cpp
    test_fuzz.cpp
    test_switchboard.cpp
    test_harness.cpp
    test_breathing.cpp
    test_schema_codec.cpp
    test_kernel.cpp)

target_include_directories(zen-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(zen-tests
    PRIVATE zen-core zen-switchboard zen-kernel zen-warnings zen-sanitize)

# ---- Loadable test Shard libraries (.so) for the kernel tests -------------
# One configurable source compiled into several variants, plus a bad-ABI lib.
function(zen_add_test_shard target)
    add_library(${target} SHARED shardlib/test_shard.cpp)
    target_link_libraries(${target} PRIVATE zen-core zen-switchboard zen-warnings zen-sanitize)
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_test_shard(zen_test_shard)
zen_add_test_shard(zen_test_shard_b) # an identical "rebuild", for hot-reload
zen_add_test_shard(zen_test_shard_badsnap ZEN_SHARD_MALFORMED_SNAPSHOT)
zen_add_test_shard(zen_test_shard_badmsg ZEN_SHARD_MALFORMED_MESSAGE)
zen_add_test_shard(zen_test_shard_v2 ZEN_SHARD_STATE_V2)

add_library(zen_test_badabi SHARED shardlib/bad_abi.cpp)
target_link_libraries(zen_test_badabi PRIVATE zen-core zen-warnings zen-sanitize)

set(ZEN_SHARD_LIBS zen_test_shard zen_test_shard_b zen_test_shard_badsnap
    zen_test_shard_badmsg zen_test_shard_v2 zen_test_badabi)
add_dependencies(zen-tests ${ZEN_SHARD_LIBS})

# Hand the .so paths to the kernel tests as string macros.
target_compile_definitions(zen-tests PRIVATE
    "ZEN_SO_SHARD=\"$<TARGET_FILE:zen_test_shard>\""
    "ZEN_SO_SHARD_B=\"$<TARGET_FILE:zen_test_shard_b>\""
    "ZEN_SO_BADSNAP=\"$<TARGET_FILE:zen_test_shard_badsnap>\""
    "ZEN_SO_BADMSG=\"$<TARGET_FILE:zen_test_shard_badmsg>\""
    "ZEN_SO_V2=\"$<TARGET_FILE:zen_test_shard_v2>\""
    "ZEN_SO_BADABI=\"$<TARGET_FILE:zen_test_badabi>\"")

# doctest's own header trips -Wsign-conversion etc.; it is third-party, so we do
# not hold it to Zen's warning policy. Mark its include dir as system.
target_include_directories(zen-tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# doctest returns the failed-assertion count as its exit code, so CTest's default
# exit-status check is the source of truth — no output-scraping needed.
set(ZEN_TEST_SUITES schema value gate registry serialize compat integration fuzz
    switchboard harness breathing schema_codec kernel)
foreach(suite IN LISTS ZEN_TEST_SUITES)
    add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
endforeach()

# A catch-all that runs everything (so `ctest -R all` exercises the whole binary).
add_test(NAME all COMMAND zen-tests)
