// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// hook_return witness: an on_claim_published returning `void`.
#include "hook_common.hpp"

namespace {
class W : public loom::WeaveBase<W, hook_return::State, loom::Accept<hook_return::Nudge>,
                                 loom::Emit<>, loom::Claims<hook_return::Fact>> {
public:
    void on(const hook_return::Nudge& n, loom::Mail&) { state_.n = n.n; }
    void on_claim_published(const hook_return::Fact& f) { state_.n = f.n; }
};
} // namespace

loom::Weave::PublishedClaim hook_return_witness(const loom::Value& v) {
    return hook_return::instantiate<W>(v);
}
