// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_TESTS_TURN_PROTOCOL_HPP
#define ZEN_TESTS_TURN_PROTOCOL_HPP

// The turn probe's vocabulary (weavelib/turn_probe.cpp): a knock, and what the probe keeps of the
// turn's time each knock was handed.

#include <zen/weave/shape.hpp>

#include <cstdint>

namespace turning {

struct TurnKnock {
    std::int64_t n = 0;
    ZEN_SHAPE(TurnKnock, 1, ZEN_FIELD(n));
};

/// The probe's state: how many knocks, and the last one's turn as it was handed (`told` 0: none).
struct TurnHeard {
    std::int64_t knocks = 0;
    std::int64_t told = 0;
    std::int64_t turn_ms = 0;
    std::int64_t turn_date_ms = 0;
    ZEN_SHAPE(TurnHeard, 1, ZEN_FIELD(knocks), ZEN_FIELD(told), ZEN_FIELD(turn_ms),
              ZEN_FIELD(turn_date_ms));
};

} // namespace turning

#endif // ZEN_TESTS_TURN_PROTOCOL_HPP
