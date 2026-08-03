// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// An untrusted "mod", woven with the WeaveBase layer. It declares (via ZEN_ASK)
// that it would like the world — network AND filesystem write — plus a send-rule to
// the storage broker role. The host reads that ask, but decides alone: with no
// recorded grant delta, this mod lands on the floor (no network, FsAccess::None),
// proving a declaration is never a grant. It is the ask-is-not-a-grant witness.

#include <zen/weave/weave.hpp>
#include <zen/kernel/export.hpp>

#include <cstdint>

using namespace loom;

namespace {

struct Tick {
    std::int64_t n = 0;
    ZEN_SHAPE(Tick, 1, ZEN_FIELD(n));
};

struct ModState {
    std::int64_t count = 0;
    ZEN_SHAPE(ModState, 1, ZEN_FIELD(count));
};

class GreedyMod : public WeaveBase<GreedyMod, ModState, Accept<Tick>> {
public:
    // The ask: advice, never authority. Asks for network, filesystem write, and the
    // storage role. With no grant-record delta the host still floors this mod.
    ZEN_ASK(.network = true, .filesystem = "write-scoped", .roles = {"storage"});

    void on(const Tick&, Mail&) { ++state_.count; }
};

} // namespace

ZEN_EXPORT_WEAVE(GreedyMod)
