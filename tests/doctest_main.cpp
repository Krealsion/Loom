// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The one translation unit that compiles the doctest framework and provides main(); every
// other test file includes <doctest.h> without this macro.

// A run that executed ZERO test cases is a FAILURE: doctest exits 0 when a filter selects
// nothing, and has no --no-tests=error option, so main() reads the run's population from
// doctest's TestRunStats and refuses an empty one by name.
// POP-01; docs/laws/population-laws.md
// Only a REAL RUN is judged: `--count`, the `--list-*` modes, `--help`, `--version` and
// `--no-run` start no run, and tests/check_population.cmake takes its inventory through them.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

/// What the run reported about its own size. Written by the listener below
/// (which doctest owns and deletes), read by main() afterwards.
struct RunCensus {
    bool ran = false;
    unsigned cases_selected = 0;
};

RunCensus& census() {
    static RunCensus c;
    return c;
}

/// A listener, not a reporter: listeners are prepended to whatever reporter the
/// command line chose, so this observes every run without replacing the console
/// output. It exists only to remember how many cases passed the filters.
struct PopulationListener : doctest::IReporter {
    explicit PopulationListener(const doctest::ContextOptions&) {}

    void test_run_end(const doctest::TestRunStats& stats) override {
        census().ran = true;
        census().cases_selected = stats.numTestCasesPassingFilters;
    }

    // Everything else is deliberately inert.
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_case_start(const doctest::TestCaseData&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

} // namespace

DOCTEST_REGISTER_LISTENER("zen-population", 0, PopulationListener);

int main(int argc, char** argv) {
#if defined(_WIN32)
    // THE TEST PROGRAM IS A HOST, and keeps the loader's dialogs away as one should: a case that
    // loads a file that is not a library is refused in words, never left waiting on a modal.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // It runs in the code page every program Loom builds runs in, UTF-8, from its manifest
    // (cmake/loom-code-page.cmake); in any other, its narrow paths are not a shipped program's.
    if (::GetACP() != CP_UTF8) {
        std::fprintf(stderr,
                     "[zen] this test program runs in code page %u, not UTF-8: it was built "
                     "without the manifest every program here carries "
                     "(cmake/loom-code-page.cmake), or this Windows is older than 10 version "
                     "1903 and ignores it\n",
                     ::GetACP());
        return 71; // beside the empty population's 70, so the cause is legible
    }
#endif
    doctest::Context context(argc, argv);
    const int result = context.run();

    if (census().ran && census().cases_selected == 0) {
        std::fprintf(stderr,
                     "\n"
                     "===============================================================================\n"
                     "[zen] EMPTY TEST POPULATION -- this run selected 0 test cases.\n"
                     "[zen] A named verification target that executes nothing has not passed; it has\n"
                     "[zen] no evidence at all. Something the filter names is gone: a renamed or\n"
                     "[zen] deleted TEST_SUITE, a suite compiled out by an #if, or a stale filter.\n"
                     "[zen] Command line:");
        for (int i = 1; i < argc; ++i) {
            std::fprintf(stderr, " %s", argv[i]);
        }
        std::fprintf(stderr,
                     "\n"
                     "===============================================================================\n");
        return 70; // distinct from doctest's own EXIT_FAILURE, so the cause is legible
    }

    return result;
}
