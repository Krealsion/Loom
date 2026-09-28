// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// Shared by the hook-return witness's five translation units: one claimed shape, one state,
// and a function that CONSTRUCTS the weave and calls `claim_published` on it. Construction
// instantiates the class template's virtual members and the call odr-uses the routing: with a
// reference parameter and no construction, the unsupported `int` form compiled, because its
// body was never instantiated.
#pragma once
#include <zen/weave.hpp>

namespace hook_return {

struct Fact {
    std::int64_t n = 0;
    ZEN_SHAPE(Fact, 1, ZEN_FIELD(n));
};

struct State {
    std::int64_t n = 0;
    ZEN_SHAPE(State, 1, ZEN_FIELD(n));
};

struct Nudge {
    std::int64_t n = 0;
    ZEN_SHAPE(Nudge, 1, ZEN_FIELD(n));
};

template <class W>
loom::Weave::PublishedClaim instantiate(const loom::Value& value) {
    W weave;
    return weave.claim_published(value);
}

} // namespace hook_return
