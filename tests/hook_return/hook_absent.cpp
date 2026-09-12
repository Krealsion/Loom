// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// hook_return witness: a claimant that declares NO on_claim_published. Applied
// trivially -- a weave that derives nothing from its claim stands behind whatever
// the bus published under it -- and it must keep compiling.
#include "hook_common.hpp"

namespace {
class W : public loom::WeaveBase<W, hook_return::State, loom::Accept<hook_return::Nudge>,
                                 loom::Emit<>, loom::Claims<hook_return::Fact>> {
public:
    void on(const hook_return::Nudge& n, loom::Mail&) { state_.n = n.n; }
};
} // namespace

loom::Weave::PublishedClaim hook_return_witness(const loom::Value& v) {
    return hook_return::instantiate<W>(v);
}
