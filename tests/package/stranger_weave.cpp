// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A REAL loadable weave, written by a stranger against the installed package, in a NAMED
// namespace: most of Loom's own fixtures hide their shapes in an anonymous one, where
// schema_of<T>()'s statics can never take the vague-linkage binding KERN-05's build contract
// exists for. Ordinary namespaced code is exactly the shape that binding reaches.

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
        state_.last = p.seq;
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

// The one line that generates the whole C ABI. Under MSVC it compiles because the export
// decoration sits beside the declaration in <zen/kernel/abi.h>, not at this definition.
ZEN_EXPORT_WEAVE(witness::Witness)
