// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_WEAVELIB_ADDRESSED_PROTOCOL_HPP
#define ZEN_TESTS_WEAVELIB_ADDRESSED_PROTOCOL_HPP

// The addressed-role probe's vocabulary (addressed_probe.cpp), shared with the suites that drive
// it. Every field is fixed-width or text, so the native and the wasm32 builds name the same
// shapes.

#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace addressed {

/// A knock on the probe; `n` numbers it.
struct AddressedKnock {
    std::int64_t n = 0;
    ZEN_SHAPE(AddressedKnock, 1, ZEN_FIELD(n));
};

/// What the probe last heard: the role its last knock was addressed to (empty for a directed
/// send or a publication), the role it was authored as, and how many knocks it has heard.
struct AddressedHeard {
    std::string addressed;
    std::string authored;
    std::int64_t knocks = 0;
    ZEN_SHAPE(AddressedHeard, 1, ZEN_FIELD(addressed), ZEN_FIELD(authored), ZEN_FIELD(knocks));
};

} // namespace addressed

#endif // ZEN_TESTS_WEAVELIB_ADDRESSED_PROTOCOL_HPP
