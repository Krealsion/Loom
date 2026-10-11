// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVER_VOCABULARY_HPP
#define ZEN_WEAVER_VOCABULARY_HPP

// The authority-policy vocabulary: the shapes a governed session, a human operator and a
// Weaver say to one another. docs/reference/weaver.md#the-vocabulary
//
// It is a header of its own so that a governed session can ask for authority knowing nothing
// about what decides: the session includes this file and never `weaver.hpp`. Every shape moves
// a decision or an answer, and none moves work: approval hands a session authority and never
// performs, replays or brokers what the session wanted to do.
//
// Four rules the field lists enforce, where a runtime check could be deleted:
//
//   No requester field. The Weaver takes the requester from the bus-stamped `mail.sender()`,
//       so there is nothing to forge.
//   No subject field, anywhere. The Weaver governs one subject, named by its capability, so no
//       shape can name another.
//   The decision is the shape, never a field. `ApproveAuthority` and `RefuseAuthority` are two
//       shapes that carry only the name of the prompt they answer; a mistyped shape name is a
//       gate refusal, never a yes.
//   The request language is narrower than `LiveAuthority`: one shape, one version, one office.
//       No "any shape", "any target", WeaveId or observe rule can be asked for, however wide
//       the Weaver's ceiling.
//
// docs/reference/weaver.md#four-rules-that-live-in-the-field-lists. The wire names carry the
// `zen.` prefix, so the registration blocks are written by hand, as in standard_shapes.hpp. A
// refusal is `zen.Refused` and a bare success `zen.Ack`.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

// ---- the governed session speaks -------------------------------------------

/// Ask for one exact send right: "may I say `shape v<version>` to whoever holds `to_role`?".
/// Sent as an ordinary message, so the Weaver answers through Loom's own answer authority and
/// the session reads `mail.answers_ask()`; there is no request id of its own. A role, not a
/// WeaveId: authority that follows an office follows whoever holds it at delivery, as
/// `Grant::allow_to_role` does. `purpose` is untrusted prose, never part of the decision, and
/// escaped by the Weaver before an operator sees it (`safe_operator_text`); it may be empty.
struct RequestAuthority {
    std::string shape;    ///< the shape name the session wants to be allowed to send
    std::int64_t version; ///< that shape's version (1 .. UINT32_MAX)
    std::string to_role;  ///< the office it wants to speak to
    std::string purpose;  ///< requester-authored prose. UNTRUSTED. may be empty

    using ZenSelf = RequestAuthority;
    static constexpr const char* zen_name = "zen.RequestAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(to_role),
                               ZEN_FIELD(purpose));
    }
};

// ---- the Weaver asks the human ---------------------------------------------

/// PUT ONE AUTHORITY REQUEST TO THE OPERATOR — everything a person needs to
/// decide, with the trusted and the untrusted told apart by name.
///
///   `prompt`         TRUSTED. This prompt's name: a number the Weaver gave it
///                    and never gives another prompt, which a decision on it
///                    carries back so that it lands on this request or nowhere.
///   `requester`      TRUSTED. The bus-stamped sender, which the Weaver has
///                    already checked is the one subject it governs.
///   `shape/version/
///    to_role`        TRUSTED. The rule AS THE WEAVER PARSED IT — not as the
///                    requester spelled it — so what the operator approves is
///                    what will be installed.
///   `until`          TRUSTED. How long a yes lasts. Present because "allow
///                    once" does not exist here and a decision surface that let
///                    a person assume it did would be lying about the grant it
///                    is about to make.
///   `requester_says` UNTRUSTED, and the field name says so at the point of
///                    reading rather than in documentation the operator does not
///                    have open. Escaped and bounded by the Weaver.
///
/// IT DOES NOT CLAIM THE DESTINATION EXISTS. "requests: Work v1 -> role
/// some.service" means exactly that and never "requests access to the running
/// FooService at #19": the Weaver does not resolve the role, is not authorized
/// to, and authority is not service discovery.
struct AuthorityPrompt {
    std::int64_t prompt;        ///< TRUSTED: this prompt's name, never reused by its Weaver
    std::int64_t requester;     ///< TRUSTED: the bus-stamped requester's WeaveId
    std::string shape;          ///< TRUSTED: the requested shape, as parsed
    std::int64_t version;       ///< TRUSTED: its version, as parsed
    std::string to_role;        ///< TRUSTED: the requested office, as parsed
    std::string until;          ///< TRUSTED: what a yes lasts for (never "once")
    std::string requester_says; ///< UNTRUSTED: the requester's own prose, escaped

    using ZenSelf = AuthorityPrompt;
    static constexpr const char* zen_name = "zen.AuthorityPrompt";
    static constexpr std::uint32_t zen_version = 2;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(prompt), ZEN_FIELD(requester), ZEN_FIELD(shape),
                               ZEN_FIELD(version), ZEN_FIELD(to_role), ZEN_FIELD(until),
                               ZEN_FIELD(requester_says));
    }
};

// ---- the human decides ------------------------------------------------------
//
// A decision names the prompt it answers, and nothing else. One pending request
// is not enough to make a decision safe without the name: the request pending
// when a decision arrives is not always the one its operator was shown. A key
// repeats, a press is doubled, a send arrives late, or the requester asks again
// the moment it hears "granted", and a decision that named nothing would land on
// the next request. So a Weaver names every prompt it puts, never reuses a
// name, and acts only on a decision naming the prompt pending now; any other
// decision, or one naming none, changes nothing and is refused to the seat in
// words. A name is not a credential: who decides is still the seat, by bus stamp.
// (`RevokeAuthority` below names nothing on purpose: it acts on what is
// installed, never on a request.)

/// YES — install the rule of the prompt named `prompt`, if that prompt is the
/// one pending. Refused by the Weaver unless the bus-stamped sender is the
/// configured operator seat: a weave that can REACH the Weaver is not thereby
/// the user.
struct ApproveAuthority {
    std::int64_t prompt = 0; ///< the `AuthorityPrompt::prompt` this decides; 0 names none

    using ZenSelf = ApproveAuthority;
    static constexpr const char* zen_name = "zen.ApproveAuthority";
    static constexpr std::uint32_t zen_version = 2;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(prompt)); }
};

/// NO to the prompt named `prompt`, if that prompt is the one pending — the
/// session is told, as the authenticated answer to the request it actually
/// sent, and nothing about the subject's authority changes.
struct RefuseAuthority {
    std::int64_t prompt = 0; ///< the `AuthorityPrompt::prompt` this decides; 0 names none

    using ZenSelf = RefuseAuthority;
    static constexpr const char* zen_name = "zen.RefuseAuthority";
    static constexpr std::uint32_t zen_version = 2;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(prompt)); }
};

/// The decisions of version 1, which carry nothing and so name no prompt. A
/// Weaver accepts them only to refuse them in words, so a seat still sending
/// them hears why nothing changed rather than meeting a missing door.
struct ApproveAuthorityV1 {
    using ZenSelf = ApproveAuthorityV1;
    static constexpr const char* zen_name = "zen.ApproveAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// See `ApproveAuthorityV1`.
struct RefuseAuthorityV1 {
    using ZenSelf = RefuseAuthorityV1;
    static constexpr const char* zen_name = "zen.RefuseAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The off switch: take back all delegated authority from the governed session at once, so a
/// person has one control whose effect is certain. The admission baseline is untouched (a union
/// cannot subtract), so revoking never costs a session what the host gave it.
struct RevokeAuthority {
    using ZenSelf = RevokeAuthority;
    static constexpr const char* zen_name = "zen.RevokeAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// WHAT IS ACTUALLY ENFORCED RIGHT NOW? Answered from the Kernel's own values
/// through the Kernel's own predicates, never from a policy mirror. Accepted from
/// the operator seat and from the governed session itself — the latter is
/// self-inspection, which reveals to a subject only what it could already
/// discover by trying.
struct DescribeAuthority {
    using ZenSelf = DescribeAuthority;
    static constexpr const char* zen_name = "zen.DescribeAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

// ---- the Weaver answers -----------------------------------------------------

/// THE AUTHENTICATED ANSWER TO `RequestAuthority`, on yes.
///
/// `basis` exists because there are two different yeses and confusing them would
/// let the Weaver take credit for authority it never granted:
///
///   "delegated"         an operator said yes, and a rule was installed
///   "already-permitted" the effective authority ALREADY covered this, so
///                       nothing was installed, nobody was asked, and the
///                       delegated overlay is exactly what it was
///
/// A session that hears the second learns something true and useful (its own
/// baseline is wider than it assumed); one that heard a bare "yes" for both would
/// have no way to tell an operator's decision from a no-op. It also means
/// repeated identical requests can never grow a duplicate-rule vector: the second
/// one is answered here and never reaches the operator at all.
///
/// RECEIVING IT PERFORMS NOTHING. The session must decide, in its own code,
/// whether and when to retry what it wanted to do; no layer replays the message
/// that was refused.
struct AuthorityGranted {
    std::string basis; ///< "delegated" | "already-permitted"

    using ZenSelf = AuthorityGranted;
    static constexpr const char* zen_name = "zen.AuthorityGranted";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(basis)); }
};

/// WHAT THE BUS WILL ACTUALLY DECIDE, rendered — the answer to
/// `DescribeAuthority`.
///
/// Two lists, not three: `base` is what the host admitted, `delegated` what an operator has
/// installed since, and effective authority is their union, read rather than sent. Loom keeps
/// no materialized effective authority either; it is computed at every delivery
/// (`effective_permits*`, zen/switchboard/grant.hpp).
///
/// Each entry is one rule, rendered by the Weaver from the snapshot the Kernel
/// handed it: `Work v1 -> role some.service`, `any shape -> any target`,
/// `observe Tick v1`. Rendered text, not a policy mirror — the values are read
/// fresh at every ask and nothing is kept between them.
struct AuthorityDescription {
    std::int64_t subject;                  ///< the one governed subject, from the capability
    std::vector<std::string> base;         ///< what the host attached at admission (immutable)
    std::vector<std::string> delegated;    ///< what an operator has installed since

    using ZenSelf = AuthorityDescription;
    static constexpr const char* zen_name = "zen.AuthorityDescription";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subject), ZEN_FIELD(base), ZEN_FIELD(delegated));
    }
};

} // namespace loom

#endif // ZEN_WEAVER_VOCABULARY_HPP
