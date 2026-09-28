// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_ENFORCEMENT_GATE_HPP
#define ZEN_TESTS_ENFORCEMENT_GATE_HPP

// The harness's enforcement gate: a security proof that cannot run FAILS, naming the missing
// capability, unless `ZEN_ALLOW_UNENFORCEABLE=1` (or `ZEN_REQUIRE_ENFORCEMENT=0`) turns it into
// a marked-degraded skip; a doctest WARN would neither fail nor skip, and a suite could go green
// having verified nothing. Each suite also tallies the OS-enforcement cases that actually ran,
// and its last case asserts that tally EXACTLY.
// POP-02; docs/laws/population-laws.md

// The tally is keyed by ZEN_ENFORCEMENT_DOMAIN, so one suite's executions never satisfy
// another's count, in a dedicated run or in the aggregate `all`; it is `==`, not `>=`, so
// deleting a witness fails and adding one is a deliberate edit. (Case floors in
// suite_population.txt are minimums instead: that population grows by design.) Linux-only, for
// test_isolation.cpp and test_policy.cpp; deliberately not in switchboard_fixtures.hpp.

#include <doctest.h>

#include <zen/isolation/sandbox.hpp>

#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>

// The including translation unit must name the enforcement population it belongs to, before the
// include. There is no default on purpose: a domain that fell back to something shared would
// let one suite's executions satisfy another's count, so the mistake is a compile error instead.
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

/// Per-DOMAIN tally of OS-enforcement cases that actually executed (imposed and confirmed), and
/// a per-domain flag set when any of them ran degraded (opt-out). The coverage case at the end
/// of each suite reads both for its own domain; the maps are process-global, but every read and
/// write is keyed, so the aggregate lane and a dedicated lane report the same number.
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

// The coverage case at the end of a suite: THIS domain's OS-enforcement population is exactly
// the one it claims. In strict mode, the exact expected count by name: a missing witness fails,
// and so does an unannounced extra one. In opt-out mode, NON-ENFORCEMENT MODE: nothing ran, so
// nothing is asserted and the output says so; the official lane (tests/verify.cmake) refuses to
// run in this mode, so an opt-out run is never enforcement evidence.
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
