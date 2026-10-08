// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// Shared by the optional-member witness's translation units: a nested shape every member kind
// can reach, and a weave whose state is the shape under test, so the poke doors and snapshot are
// instantiated as every woven state's are.
#pragma once
#include <zen/weave.hpp>

#include <optional>

namespace optional_members {

struct Inner {
    std::int64_t x = 0;
    ZEN_SHAPE(Inner, 1, ZEN_FIELD(x));
};

struct Nudge {
    std::int64_t n = 0;
    ZEN_SHAPE(Nudge, 1, ZEN_FIELD(n));
};

template <class State>
class Holder : public loom::WeaveBase<Holder<State>, State, loom::Accept<Nudge>, loom::Emit<>> {
public:
    void on(const Nudge&, loom::Mail&) {}
};

template <class State>
loom::Value instantiate(const loom::Value& v) {
    Holder<State> weave;
    weave.revive(v);
    return weave.snapshot();
}

} // namespace optional_members
