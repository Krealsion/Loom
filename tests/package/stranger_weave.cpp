// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A REAL loadable weave, written by a stranger against the installed package.
//
// Deliberately in a NAMED namespace. Most of Loom's own weave fixtures hide their
// shapes in an anonymous one, where internal linkage means schema_of<T>()'s statics
// can never take the vague-linkage binding that KERN-05's build contract exists to
// mitigate -- so those fixtures cannot exercise it. A stranger writes ordinary
// namespaced code, which is exactly the shape that found F-22 in the first place.

#include "witness_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

namespace witness {

class Witness
    : public loom::WeaveBase<Witness, Tally, loom::Accept<Ping, Nested>, loom::Emit<Pong>> {
public:
    void on(const Ping& p, loom::Mail& mail) {
        ++state_.handled;
        state_.raw_total += p.seq;
        state_.label = "stranger";
        // Crossing back out through the C ABI's host callback table: this is the
        // half of the seam a load-only proof would never touch.
        mail.reply(Pong{p.seq});
    }
    // Accepted so that this artifact's vocabulary genuinely NESTS. Nothing here
    // needs to run for the closure witness -- what is under test is whether a
    // stranger can learn this shape's structure without ever having compiled
    // against Inner.
    void on(const Nested& n, loom::Mail&) {
        ++state_.handled;
        state_.raw_total += n.one.k;
    }
};

} // namespace witness

// The one line that generates the whole C ABI -- and the one that produced
// `C2375: 'zen_weave_abi': redefinition; different linkage` under MSVC before the
// export decoration moved beside the declaration in <zen/kernel/abi.h>.
ZEN_EXPORT_WEAVE(witness::Witness)
