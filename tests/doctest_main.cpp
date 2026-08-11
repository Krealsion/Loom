// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The one translation unit that compiles the doctest framework and provides
// main(). Every other test file includes <doctest.h> without this macro.
//
// POP-01: a run that executed ZERO test cases is a FAILURE, not a pass.
// docs/laws/population-laws.md
//
// doctest exits 0 when a filter selects nothing -- `--test-suite=switchboard`
// against a binary where that suite was renamed, #if'd out, or deleted prints
// "test cases: 0 ... Status: SUCCESS!" and returns 0, so CTest's exit-status
// check reports the named suite Passed in 0.00 s having verified nothing. That
// is the F-2 hole, and doctest 2.4.11 has no built-in option to close it (there
// is no --no-tests=error equivalent anywhere in Context::parseArgs). So the
// project's own main() closes it: the run's population is read out of doctest's
// own TestRunStats and an empty one is refused by name.
//
// The guard is deliberately scoped to a REAL RUN. doctest calls test_run_end
// only when `query_mode` is false -- `--count`, `--list-test-cases` and
// `--list-test-suites` report through report_query instead, and `--help`,
// `--version`, `--no-run` and `--list-reporters` return before either. Those
// are exactly the modes tests/check_population.cmake uses to take an inventory
// without running anything, so "no run happened" must stay a legitimate,
// silent, zero-exit outcome. Only a run that actually started and selected
// nothing is a lie about its own population.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdio>

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
