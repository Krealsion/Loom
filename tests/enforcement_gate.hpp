#ifndef ZEN_TESTS_ENFORCEMENT_GATE_HPP
#define ZEN_TESTS_ENFORCEMENT_GATE_HPP

// Harness honesty (the project's own ethos applied to itself: never report a pass you didn't earn).
//
// Every OS-security proof used to sit behind `if (!enforceable(cap)) { WARN(); return; }`, and
// doctest's WARN is a SOFT no-op — it neither fails nor skips, so the whole security suite went
// GREEN having verified nothing on any host without unprivileged userns + delegated cgroup-v2
// (hardened CI, many containers). That is fail-open while reporting success — the exact failure mode
// the runtime polices everywhere else.
//
// This gate flips the default: a security-relevant skip FAILS, naming the missing capability — UNLESS
// the explicit opt-out `ZEN_ALLOW_UNENFORCEABLE=1` (or `ZEN_REQUIRE_ENFORCEMENT=0`) converts it to a
// marked-degraded skip. Plus a positive tally: the suites count how many OS-enforcement cases actually
// executed, and a final coverage case asserts that count clears a floor — so a green can never mean
// "every case silently skipped." The runtime's own fail-safe behavior is unchanged; this is purely
// the harness no longer reporting a pass it did not earn.
//
// Linux-only: included by test_isolation.cpp / test_policy.cpp (which already depend on the isolation
// host). It is deliberately NOT in switchboard_fixtures.hpp, which portable suites include.

#include <doctest.h>

#include <zen/isolation/sandbox.hpp>

#include <cstdlib>
#include <initializer_list>
#include <string>

namespace zenh { // zen test harness

inline bool env_is(const char* name, const char* value) {
    const char* v = std::getenv(name);
    return v != nullptr && std::string(v) == value;
}

/// The strict gate is ON by default. Either opt-out env converts a missing-enforcement to a skip:
/// `ZEN_ALLOW_UNENFORCEABLE=1` (the canonical opt-out) or `ZEN_REQUIRE_ENFORCEMENT=0`.
inline bool require_enforcement_strict() {
    return !env_is("ZEN_ALLOW_UNENFORCEABLE", "1") && !env_is("ZEN_REQUIRE_ENFORCEMENT", "0");
}

enum class Gate { Proceed, FailHard, SkipDegraded };

/// The pure decision (no doctest side effect, so it is itself directly testable): every capability
/// enforceable -> Proceed; otherwise FailHard by default, or SkipDegraded under the opt-out. Names
/// the missing capabilities in `missing_out`.
inline Gate enforcement_decision(const loom::EnforcementReport& rep,
                                 std::initializer_list<loom::Capability> caps,
                                 std::string& missing_out) {
    std::string missing;
    for (loom::Capability c : caps) {
        if (!rep.enforceable(c)) {
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += loom::capability_name(c);
        }
    }
    if (missing.empty()) {
        return Gate::Proceed;
    }
    missing_out = missing;
    return require_enforcement_strict() ? Gate::FailHard : Gate::SkipDegraded;
}

/// Process-global tally of OS-enforcement cases that actually executed (imposed-and-confirmed), and
/// a flag set when any case ran degraded (opt-out). The coverage TEST_CASE reads both.
inline int& enforced_case_count() {
    static int n = 0;
    return n;
}
inline bool& degraded_run() {
    static bool d = false;
    return d;
}

} // namespace zenh

// Consult the gate at a security proof's entry. On Proceed: bump the tally and fall through. On
// FailHard: FAIL loudly naming the missing capability (aborts the case) and return. On SkipDegraded:
// mark the run degraded, WARN, and return from the calling test/subcase. Usage:
//   ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Network}, "the case description");
#define ZEN_REQUIRE_ENFORCEABLE(REPORT, CAPS, WHAT)                                                \
    do {                                                                                           \
        std::string zen_missing__;                                                                 \
        const ::zenh::Gate zen_gate__ =                                                            \
            ::zenh::enforcement_decision((REPORT), CAPS, zen_missing__);                           \
        if (zen_gate__ == ::zenh::Gate::FailHard) {                                                \
            FAIL("OS enforcement unavailable for " << (WHAT) << ": " << zen_missing__              \
                 << " -- run under tests/run-under-scope.sh, or set ZEN_ALLOW_UNENFORCEABLE=1 to " \
                    "convert to a marked-degraded skip");                                          \
            return;                                                                                \
        }                                                                                          \
        if (zen_gate__ == ::zenh::Gate::SkipDegraded) {                                            \
            ::zenh::degraded_run() = true;                                                         \
            MESSAGE("DEGRADED SKIP: OS enforcement unavailable for "                               \
                    << (WHAT) << ": " << zen_missing__ << " (opt-out set)");                       \
            return;                                                                                \
        }                                                                                          \
        ++::zenh::enforced_case_count();                                                           \
    } while (false)

// The full powerbox floor needs all three capabilities OS-enforceable. A single preprocessor token
// (so it is ONE macro argument — a braced list written inline would be split on its commas).
#define ZEN_FLOOR_CAPS                                                                             \
    { Capability::Network, Capability::Filesystem, Capability::Resources }

#endif // ZEN_TESTS_ENFORCEMENT_GATE_HPP
