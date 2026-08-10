// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVER_VOCABULARY_HPP
#define ZEN_WEAVER_VOCABULARY_HPP

// THE AUTHORITY-POLICY VOCABULARY — the shapes a governed session, a human
// operator and a Weaver say to one another (WEAVER-1).
//
// It is a HEADER OF ITS OWN, apart from the Weaver that implements the policy,
// and that separation is the architecture rather than tidiness: a governed
// session must be able to ask for authority while knowing nothing whatever
// about the thing that decides. It includes this file and not `weaver.hpp`, so
// "the session depends on the request language, never on the policy" is a fact
// the include graph enforces.
//
// THE SENTENCE THESE SHAPES CARRY:
//
//     the Kernel enforces.  the Weaver decides.  the session acts.
//
// so every shape here moves a DECISION or an ANSWER, and none of them moves
// work. Approval hands a session authority; it never performs, replays or
// brokers what the session wanted to do (that is why there is no shape here
// meaning "and go do it").
//
// FOUR RULES THE FIELD LISTS ENFORCE, each of which would otherwise be a runtime
// check somebody could delete:
//
//   NO REQUESTER FIELD.  `RequestAuthority` carries no identity, so there is
//       nothing to forge. The Weaver derives the requester from the bus-stamped
//       `mail.sender()` and compares it with the one subject its capability
//       names. An identity field beside a trusted stamp is an impersonation
//       surface whose only defence is that everyone remembers to ignore it.
//
//   NO SUBJECT FIELD, ANYWHERE.  The operator's four decision shapes name no
//       subject either. The Weaver already governs exactly one, from the
//       capability, so cross-subject administration is not refused — it is a
//       sentence with nowhere to put the other subject (the same argument
//       `GrantAuthority` itself makes one level down).
//
//   THE DECISION IS THE SHAPE, NEVER A FIELD.  `ApproveAuthority` and
//       `RefuseAuthority` are two contentless shapes rather than one carrying a
//       bool. A field can be defaulted, mis-parsed or mis-typed into meaning
//       yes; a shape identity cannot — a mistyped shape name is a gate refusal,
//       which is the safe direction for a security decision.
//
//   THE REQUEST LANGUAGE IS NARROWER THAN `LiveAuthority`.  A request names one
//       shape, one version and one office. There is no way to spell "any shape",
//       "any target", an exact WeaveId, or an observe rule, so the widest thing
//       a session can ASK for is one exact send right — regardless of how wide a
//       ceiling the host happened to hand the Weaver.
//
// The wire names carry the substrate's `zen.` prefix, which is why these
// registration blocks are hand-written rather than `ZEN_SHAPE(...)` (the macro
// derives the name from the struct, and `#ShapeName` cannot produce a dot) —
// exactly as `standard_shapes.hpp` does, and for the same reason.
//
// REUSED, NOT REINVENTED: a refusal is `zen.Refused` and a bare success is
// `zen.Ack` (standard_shapes.hpp). Only two answers here are bespoke, and each
// earns it by carrying structure whose absence would leave the reader confused
// rather than merely less-informed.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

// ---- the governed session speaks -------------------------------------------

/// ASK FOR ONE EXACT SEND RIGHT: "may I say `shape v<version>` to whoever holds
/// `to_role`?"
///
/// Sent as an ordinary message, which is what makes it an ask: a delivery from a
/// live weave confers the authority to answer it, so the Weaver answers through
/// Loom's own correlation and the session reads `mail.answers_ask()`. There is
/// no request id here, and no place for one — inventing a correlation beside the
/// one Loom already keeps would be a second, weaker way to know which answer
/// belongs to which question.
///
/// ROLE, NOT WEAVEID, ON PURPOSE. Authority that follows an office follows
/// whoever holds it at delivery, which is what a session actually wants from a
/// service, and it is what `Grant::allow_to_role` already means. Asking by
/// WeaveId would freeze one incarnation into permanent authority and would put a
/// weave's identity in a payload, which is the shape this vocabulary is built to
/// avoid.
///
/// `purpose` IS UNTRUSTED. It is prose the requester wrote about itself. It is
/// not evidence of anything, it never reaches the authority decision, and the
/// Weaver escapes it before an operator ever sees it (see `Weaver::safe_text`).
/// Leaving it empty is ordinary and costs the requester nothing.
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
    std::int64_t requester;     ///< TRUSTED: the bus-stamped requester's WeaveId
    std::string shape;          ///< TRUSTED: the requested shape, as parsed
    std::int64_t version;       ///< TRUSTED: its version, as parsed
    std::string to_role;        ///< TRUSTED: the requested office, as parsed
    std::string until;          ///< TRUSTED: what a yes lasts for (never "once")
    std::string requester_says; ///< UNTRUSTED: the requester's own prose, escaped

    using ZenSelf = AuthorityPrompt;
    static constexpr const char* zen_name = "zen.AuthorityPrompt";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(requester), ZEN_FIELD(shape), ZEN_FIELD(version),
                               ZEN_FIELD(to_role), ZEN_FIELD(until), ZEN_FIELD(requester_says));
    }
};

// ---- the human decides ------------------------------------------------------
//
// Four contentless shapes. They carry nothing because there is nothing left to
// say: one Weaver, one governed subject, at most one request pending — so the
// only thing an approval could name is the request it is already the only
// candidate for. That is not a shortcut; it is what keeps a decision from
// landing on a request the operator never saw (see `RevokeAuthority` below for
// the one place the absence is load-bearing in the other direction).

/// YES — install the pending request's rule as delegated authority. Refused by
/// the Weaver unless the bus-stamped sender is the configured operator seat: a
/// weave that can REACH the Weaver is not thereby the user.
struct ApproveAuthority {
    using ZenSelf = ApproveAuthority;
    static constexpr const char* zen_name = "zen.ApproveAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// NO — the session is told, as the authenticated answer to the request it
/// actually sent, and nothing about the subject's authority changes.
struct RefuseAuthority {
    using ZenSelf = RefuseAuthority;
    static constexpr const char* zen_name = "zen.RefuseAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// THE OFF SWITCH — take back ALL delegated authority from the governed session,
/// at once. Whole-overlay rather than per-rule on purpose: the first user seat
/// needs one control it can be certain of, and "which of my four rules did I just
/// remove?" is a question a person should not have to answer under pressure. The
/// admission baseline is untouched — a union cannot subtract — so revoking never
/// costs a session what the host gave it.
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
/// TWO LISTS, NOT THREE, and the missing one is the point. `base` is what the
/// host admitted; `delegated` is what an operator has installed since; EFFECTIVE
/// authority is their union BY DEFINITION and so is read rather than transmitted.
/// Loom deliberately has no materialized "effective authority" object for the
/// same reason (grant.hpp: "There is no third, materialized effective authority
/// object to fall out of date") — shipping one here would recreate exactly the
/// second store this whole design exists to avoid.
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
