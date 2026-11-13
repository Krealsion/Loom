// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_WEAVELIB_MADE_PROTOCOL_HPP
#define ZEN_TESTS_WEAVELIB_MADE_PROTOCOL_HPP

// The made-with probe's vocabulary (made_probe.cpp), shared with the suites that drive it. Every
// field is fixed-width or text, so the native and the wasm32 builds name the same shapes.

#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace made {

/// What the probe is made with.
struct MadeSetup {
    std::string place;
    std::int64_t limit = 0;
    ZEN_SHAPE(MadeSetup, 1, ZEN_FIELD(place), ZEN_FIELD(limit));
};

/// Asks the probe what it was made with.
struct MadeAsk {
    std::int64_t n = 0;
    ZEN_SHAPE(MadeAsk, 1, ZEN_FIELD(n));
};

/// The answer: the value the instance answering was made with, kept apart from its state.
struct MadeTold {
    std::string place;
    std::int64_t limit = 0;
    std::int64_t asks = 0;
    ZEN_SHAPE(MadeTold, 1, ZEN_FIELD(place), ZEN_FIELD(limit), ZEN_FIELD(asks));
};

/// The probe's state: how many asks it has answered.
struct MadeState {
    std::int64_t asks = 0;
    ZEN_SHAPE(MadeState, 1, ZEN_FIELD(asks));
};

} // namespace made

#endif // ZEN_TESTS_WEAVELIB_MADE_PROTOCOL_HPP
