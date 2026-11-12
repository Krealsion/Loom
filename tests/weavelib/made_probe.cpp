// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The made-with probe: a weave made with a `MadeSetup`, which it keeps apart from its state and
// answers each ask with, so the instance a reload makes is seen to be made with the value too.
// Built native, boxed and isolated alike.

#include "made_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

using namespace loom;
using namespace made;

namespace {

class MadeProbe final
    : public WeaveBase<MadeProbe, MadeState, Accept<MadeAsk>, Emit<MadeTold>> {
public:
    ZEN_MADE_WITH(MadeSetup);

    explicit MadeProbe(const MadeSetup& setup) : setup_(setup) {}

    void on(const MadeAsk&, Mail& mail) {
        ++state_.asks;
        (void)mail.reply(MadeTold{setup_.place, setup_.limit, state_.asks});
    }

private:
    MadeSetup setup_;
};

} // namespace

ZEN_EXPORT_WEAVE(MadeProbe)
