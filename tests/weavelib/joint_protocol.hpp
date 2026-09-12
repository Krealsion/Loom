// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// The vocabulary the joint-publication witnesses (suite `joint`) share between
// the native suite and the loaded fixture. Two claim
// shapes with no application meaning — a "document fact" and a "view fact" —
// because the substrate mechanism under test must have no document or layout
// vocabulary of its own; the pair is what any two owners changing together look
// like from the bus.
#pragma once
#include <zen/weave.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace joint_test {

/// One owner's fact: which thing it currently holds, and its generation.
struct DocFact {
    std::string path;
    std::int64_t epoch = 0;
    ZEN_SHAPE(DocFact, 1, ZEN_FIELD(path), ZEN_FIELD(epoch));
};

/// The other owner's fact: what it currently shows, and whether it has focus.
struct ViewFact {
    std::string shown;
    bool focus = false;
    ZEN_SHAPE(ViewFact, 1, ZEN_FIELD(shown), ZEN_FIELD(focus));
};

/// A shape nobody declares in a claim-set, for the Undeclared refusal.
struct Stray {
    std::int64_t n = 0;
    ZEN_SHAPE(Stray, 1, ZEN_FIELD(n));
};

/// What the test tells a participant to do, from inside its own delivery.
struct Cmd {
    std::string verb; ///< claim | offer | begin | commit | cancel | status | release | note | arm-fail | arm-decline | arm-fail-quietly
    std::int64_t op = 0;
    std::string path;
    std::int64_t epoch = 0;
    Cmd() = default;
    Cmd(std::string v, std::int64_t o = 0, std::string p = std::string(), std::int64_t e = 0)
        : verb(std::move(v)), op(o), path(std::move(p)), epoch(e) {}
    ZEN_SHAPE(Cmd, 1, ZEN_FIELD(verb), ZEN_FIELD(op), ZEN_FIELD(path), ZEN_FIELD(epoch));
};

/// The loaded probe's state — everything the suite reads back through a
/// snapshot, so the hook's effect is visible without reaching into the image.
///
/// EXPOSED WHOLE: the substrate's own mutation
/// doors -- `zen.PokeWrite`, `zen.PokeResetState` -- can change `path` from outside the
/// probe's handlers, which is the reusable SDK path the corrections' witnesses drive. A
/// participant that exposes a writable path must still keep its claim true after such a
/// write; a fixture that hid every field could not ask that question.
struct ProbeState {
    std::string path = "A";
    std::int64_t epoch = 1;
    std::int64_t published_seen = 0;    ///< times on_claim_published ran (attempts, not successes)
    std::int64_t deliveries = 0;        ///< times a Cmd was handled
    std::string seen_at_delivery;       ///< `path` as the handler found it, last delivery
    std::int64_t last_offer_ok = -1;    ///< 1 accepted, 0 refused, -1 never offered
    std::string last_refusal;
    std::int64_t hook_runs = 0;         ///< times after_delivery ran
    /// WHAT THE CLAIM LAST SAID, carried in the state on purpose: a participant that
    /// mirrors its state into its claim must carry that bookkeeping across a reload,
    /// or its successor's first delivery re-claims a value the bus already holds and
    /// aborts an operation that had just bound it. The state's own defaults.
    std::string claimed_path = "A";
    std::int64_t claimed_epoch = 1;
    ZEN_EXPOSE();
    ZEN_SHAPE(ProbeState, 1, ZEN_FIELD(path), ZEN_FIELD(epoch), ZEN_FIELD(published_seen),
              ZEN_FIELD(deliveries), ZEN_FIELD(seen_at_delivery), ZEN_FIELD(last_offer_ok),
              ZEN_FIELD(last_refusal), ZEN_FIELD(hook_runs), ZEN_FIELD(claimed_path),
              ZEN_FIELD(claimed_epoch));
};

} // namespace joint_test
