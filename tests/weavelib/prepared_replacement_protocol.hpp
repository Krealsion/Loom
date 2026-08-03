// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_PREPARED_REPLACEMENT_PROTOCOL_HPP
#define ZEN_TESTS_PREPARED_REPLACEMENT_PROTOCOL_HPP

// The preparation conversation and the production query of the `versioned.service`
// fixture pair (R2B-3b-3). Shared by the v1/v2 .so artifacts and by the kernel
// suite, so every party derives the same content-id from the same ZEN_SHAPE and
// agrees across the .so boundary exactly as the gate requires.
//
// THESE ARE NOT LOOM LIFECYCLE VOCABULARY, and they live here rather than in any
// `zen/` header to keep that true. The Switchboard hard-codes exactly one schema
// — its own lifecycle-policy grammar — and the readiness mechanism deliberately
// did not make it two: the bus authenticates the *conversation* (who spoke, which
// ask they answered, whether Loom attests it) and never reads a byte of what was
// said. So a coordinator and its candidate may agree on any shapes they like;
// these are simply the ones this fixture agreed on.
//
// The transaction id travels in the payload because the RECORD has to be named.
// It is not authority and cannot become authority: `accept_preparation_answer`
// uses it as a lookup key and then proves the delivery is that record's one
// preparation answer from the bus's own private facts. Nothing here carries a
// life, an incarnation, a token, an answer right, or envelope provenance — a
// payload that could carry any of those would be a payload worth forging.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>

namespace versioned {

// ---- the production contract (what the role answers) -----------------------

/// "Which version are you?" — the domain query the production role serves, so a
/// test can ASK who is answering rather than infer it from topology.
struct QueryVersion {
    std::int64_t seq = 0;
    ZEN_SHAPE(QueryVersion, 1, ZEN_FIELD(seq));
};

struct VersionReply {
    std::string version;
    ZEN_SHAPE(VersionReply, 1, ZEN_FIELD(version));
};

// ---- the preparation conversation (coordinator <-> sealed candidate) -------

/// The coordinator's ask. `plan` is the preparation the candidate is being asked
/// to perform, and validating it is the candidate's own business — an unknown
/// plan is refused by the candidate, not by the bus.
///
///   "deferred"  take the answer right away, finish across later deliveries
///   "immediate" complete inside this handler and answer now
///   "refuse"    decline, authentically
///
/// `escape_to` names a stranger the candidate is told to try to reach. It is
/// fixture control, in the tradition of `storage::DoForge{key, victim}`: a sealed
/// candidate has no way to look an address up, so an isolation proof that wanted
/// it to attempt a direct escape had to hand it one. Nothing is authorized by
/// carrying it — the send is refused as `SealedSpeech` like every other.
struct PrepareReplacement {
    std::int64_t transaction = 0;
    std::string plan;
    std::int64_t escape_to = 0;
    ZEN_SHAPE(PrepareReplacement, 1, ZEN_FIELD(transaction), ZEN_FIELD(plan),
              ZEN_FIELD(escape_to));
};

/// A later step of the same preparation, through the same coordinator-only door.
/// An ordinary message: it carries no answer right and grants none — the right
/// the candidate spends was earned by the ask, and is being *retained*, not
/// re-issued.
struct ContinuePreparation {
    std::int64_t transaction = 0;
    ZEN_SHAPE(ContinuePreparation, 1, ZEN_FIELD(transaction));
};

struct CandidateReady {
    std::int64_t transaction = 0;
    ZEN_SHAPE(CandidateReady, 1, ZEN_FIELD(transaction));
};

struct CandidateRefused {
    std::int64_t transaction = 0;
    std::string reason;
    ZEN_SHAPE(CandidateRefused, 1, ZEN_FIELD(transaction), ZEN_FIELD(reason));
};

/// ORDINARY DOMAIN SPEECH FROM INSIDE AN ACTIVATION (R2B-3d-1). The positive
/// control that keeps "activation is not answerable" from quietly meaning
/// "activation is mute": a freshly admitted candidate has no answer authority
/// and still has every ordinary right its grant gives it, so it says something
/// on its own initiative, to the very weave whose imaginary question it was just
/// refused permission to answer.
struct ActivationObserved {
    std::int64_t sequence = 0;
    std::string version;
    ZEN_SHAPE(ActivationObserved, 1, ZEN_FIELD(sequence), ZEN_FIELD(version));
};

/// The private retirement word a coordinator says to an incumbent it has just
/// sealed. Part of the fixture, not of Loom: retirement is a conversation the
/// operator owns, and the substrate's only part in it is that a sealed weave can
/// still hear its coordinator.
struct RetireNow {
    std::int64_t transaction = 0;
    ZEN_SHAPE(RetireNow, 1, ZEN_FIELD(transaction));
};

} // namespace versioned

#endif // ZEN_TESTS_PREPARED_REPLACEMENT_PROTOCOL_HPP
