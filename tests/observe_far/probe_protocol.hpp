// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_OBSERVE_FAR_PROBE_PROTOCOL_HPP
#define ZEN_TESTS_OBSERVE_FAR_PROBE_PROTOCOL_HPP

// THE OBSERVATION JOURNEY'S OWN SMALL VOCABULARY, shared by the far test host (far_host.cpp),
// which produces it, and the session host's vocabulary weave (weavelib/observe_probe_vocab.cpp),
// which declares it so a tool's asks can be encoded there. tests/session/observe_journey.py drives
// both. Test-only: nothing here is installed.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>

namespace zen_tests::observe_probe {

/// The producer's office on the far host, and the office its commands are asked of.
inline constexpr const char* kTicker = "far.ticker";

/// TELL THE PRODUCER WHAT TO DO, answered `zen.Ack` (or `zen.Refused` for a verb it does not know):
///   tick N      publish N ticks, then the state, in this delivery
///   burst N     the same, N large: one delivery that outruns a small window
///   foreign     a participant that does NOT hold the office publishes a tick
///   replace     the office changes hands: a new producer holds it from the next turn
///   revoke      the far host withdraws every subscription of the asking session
///   quit        the far host ends (the link's session with it)
struct ObserveProbeCommand {
    std::string verb;
    std::int64_t count = 0;
    ZEN_SHAPE(ObserveProbeCommand, 1, ZEN_FIELD(verb), ZEN_FIELD(count));
};

/// One occurrence, numbered by its producer 1, 2, ... with no gaps.
struct ProbeTick {
    std::int64_t n = 0;
    ZEN_SHAPE(ProbeTick, 1, ZEN_FIELD(n));
};

/// Where the producer stands: the last tick it said. Whole state -- a newer one says all an older
/// one did, so a subscriber may name it `latest`.
struct ProbeState {
    std::int64_t last = 0;
    ZEN_SHAPE(ProbeState, 1, ZEN_FIELD(last));
};

} // namespace zen_tests::observe_probe

#endif // ZEN_TESTS_OBSERVE_FAR_PROBE_PROTOCOL_HPP
