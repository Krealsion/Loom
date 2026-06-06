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
    test_integration.cpp
    test_fuzz.cpp)

target_include_directories(zen-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(zen-tests
    PRIVATE zen-core zen-warnings zen-sanitize)

# doctest's own header trips -Wsign-conversion etc.; it is third-party, so we do
# not hold it to Zen's warning policy. Mark its include dir as system.
target_include_directories(zen-tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# doctest returns the failed-assertion count as its exit code, so CTest's default
# exit-status check is the source of truth — no output-scraping needed.
set(ZEN_TEST_SUITES schema value gate registry serialize integration fuzz)
foreach(suite IN LISTS ZEN_TEST_SUITES)
    add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
endforeach()

# A catch-all that runs everything (so `ctest -R all` exercises the whole binary).
add_test(NAME all COMMAND zen-tests)
