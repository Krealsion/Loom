// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

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
// executed, and a final coverage case asserts that count — so a green can never mean "every case
// silently skipped." The runtime's own fail-safe behavior is unchanged; this is purely the harness no
// longer reporting a pass it did not earn.
//
// POP-02 (R2F-D): THE TALLY IS PER DOMAIN, AND THE EXPECTED COUNT IS EXACT.
//
// It used to be one process-global `static int n` that both suites incremented and both floors read
// as `>= N`. In a dedicated run each suite saw only its own contribution and the numbers looked
// right; in the aggregate `all` lane — which is the one lane that mitigates a vanished suite —
// isolation ran first and left 15 on the counter, so policy's floor of 11 was already satisfied
// before policy executed a single proof. Neutering every one of policy's eleven OS-enforcement
// guards left the `all` lane reporting 584/584, exit 0 (COLD-1 F-24). A floor named for one
// population must be computed from that population's own witnesses, so the counter is keyed by
// ZEN_ENFORCEMENT_DOMAIN, which each suite's translation unit declares for itself.
//
// And the count is `==`, not `>=`. A `>=` floor with slack hides a deletion: isolation ran 15 then,
// against a floor of 12, so a genuine enforcement witness could be removed and the suite stayed
// fully green, 32/32, 230 assertions, nothing moved. An exact expected population makes a missing
// witness fail — and makes a NEW witness an intentional edit here, which is the correct price for a
// small, security-relevant, deliberately stable population. (The per-suite CASE floors in
// suite_population.txt are minimums instead, for the opposite reason: that population grows every
// phase by design. Two populations, two policies, each argued.)
//
// Linux-only: included by test_isolation.cpp / test_policy.cpp (which already depend on the isolation
// host). It is deliberately NOT in switchboard_fixtures.hpp, which portable suites include.

#include <doctest.h>

#include <zen/isolation/sandbox.hpp>

#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>

// The including translation unit must name the enforcement population it belongs to, before the
// include. There is no default on purpose: a domain that fell back to something shared would
// silently re-create F-24, and this way the mistake is a compile error at the first new suite
// rather than a wrong number in a green run.
#ifndef ZEN_ENFORCEMENT_DOMAIN
#error "define ZEN_ENFORCEMENT_DOMAIN (the suite's own name, as a string literal) before including enforcement_gate.hpp"
#endif

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

/// Per-DOMAIN tally of OS-enforcement cases that actually executed (imposed-and-confirmed), and a
/// per-domain flag set when any of that domain's cases ran degraded (opt-out). The domain is the
/// suite that owns the population; isolation's executions can never be read as policy's. The
/// coverage TEST_CASE at the end of each suite reads both, for its own domain only.
///
/// The maps are process-global storage, but nothing process-global is ever the ANSWER: every read
/// and every write is keyed, so the aggregate lane and a dedicated lane report the same number for
/// the same domain.
inline int& enforced_case_count(const std::string& domain) {
    static std::map<std::string, int> counts;
    return counts[domain];
}
inline bool& degraded_run(const std::string& domain) {
    static std::map<std::string, bool> degraded;
    return degraded[domain];
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
            ::zenh::degraded_run(ZEN_ENFORCEMENT_DOMAIN) = true;                                   \
            MESSAGE("DEGRADED SKIP: OS enforcement unavailable for "                               \
                    << (WHAT) << ": " << zen_missing__ << " (opt-out set)");                       \
            return;                                                                                \
        }                                                                                          \
        ++::zenh::enforced_case_count(ZEN_ENFORCEMENT_DOMAIN);                                     \
    } while (false)

// The coverage case at the end of a suite: assert that THIS domain's OS-enforcement population is
// exactly the one it claims. Two outcomes, and they are not interchangeable.
//
//   strict mode   -> the exact expected count, by name. A missing witness fails; so does an
//                    unannounced extra one.
//   opt-out mode  -> NON-ENFORCEMENT MODE. The population did not run, so there is nothing to
//                    assert about it, and the output says so in words that cannot be mistaken for
//                    the proof. The official lane (tests/verify.cmake) refuses to run at all in
//                    this mode, so an opt-out run can never be minted as enforcement evidence.
#define ZEN_ENFORCEMENT_POPULATION(EXPECTED)                                                       \
    do {                                                                                           \
        if (!::zenh::require_enforcement_strict() ||                                               \
            ::zenh::degraded_run(ZEN_ENFORCEMENT_DOMAIN)) {                                        \
            MESSAGE("*** NON-ENFORCEMENT MODE *** the OS-enforcement population for '"             \
                    << ZEN_ENFORCEMENT_DOMAIN                                                      \
                    << "' did NOT execute (ZEN_ALLOW_UNENFORCEABLE=1 / "                           \
                       "ZEN_REQUIRE_ENFORCEMENT=0). This run is NOT evidence that containment "    \
                       "was imposed; the expected " << (EXPECTED)                                  \
                    << " proofs were converted to marked-degraded skips.");                        \
            return;                                                                                \
        }                                                                                          \
        MESSAGE("OS-enforcement cases executed for '" << ZEN_ENFORCEMENT_DOMAIN                    \
                << "': " << ::zenh::enforced_case_count(ZEN_ENFORCEMENT_DOMAIN) << " of "          \
                << (EXPECTED) << " expected");                                                     \
        CHECK(::zenh::enforced_case_count(ZEN_ENFORCEMENT_DOMAIN) == (EXPECTED));                   \
    } while (false)

// The full powerbox floor needs all three capabilities OS-enforceable. A single preprocessor token
// (so it is ONE macro argument — a braced list written inline would be split on its commas).
#define ZEN_FLOOR_CAPS                                                                             \
    { Capability::Network, Capability::Filesystem, Capability::Resources }

#endif // ZEN_TESTS_ENFORCEMENT_GATE_HPP
