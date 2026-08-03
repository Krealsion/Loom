// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_MESSAGE_HPP
#define ZEN_SWITCHBOARD_MESSAGE_HPP

#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace loom {

/// A stable handle to a registered Weave, assigned by the Switchboard. The
/// zero id is the null/invalid handle (e.g. "no reply_to", "from outside").
struct WeaveId {
    std::uint64_t value = 0;

    bool valid() const noexcept { return value != 0; }
    friend bool operator==(WeaveId a, WeaveId b) noexcept { return a.value == b.value; }
    friend bool operator!=(WeaveId a, WeaveId b) noexcept { return a.value != b.value; }
};

class Switchboard;

/// THE PRIVATE IDENTITY OF ONE LOOM.
///
/// An empty type whose only purpose is to be a thing that exists exactly once
/// per Switchboard and cannot be manufactured. Ordinary code cannot construct
/// one (the constructor is private to the Switchboard), cannot name a value that
/// equals one, and cannot obtain one except by holding an authority that board
/// itself issued.
///
/// IT IS HELD BY SHARED OWNERSHIP, AND THAT IS THE LIFETIME ANSWER rather than a
/// convenience. A raw address would be the obvious representation and is the
/// wrong one: a board can be destroyed and a later board allocated at the same
/// address, at which point an authority from the dead world would validate
/// against the living one. A control block is not recycled that way — the board
/// holds the only `shared_ptr`, an authority holds a `weak_ptr`, and when a board
/// dies every authority it ever issued expires with it, permanently.
class LoomIdentity {
private:
    friend class Switchboard;
    LoomIdentity() = default;
};

/// WHY A RECIPIENT MAY TRUST WHAT IT WAS JUST HANDED — a delivery fact, and the
/// third of three questions the system has always needed to keep apart:
///
///   SHAPE      "can this be represented and admitted?"   — answered by the gate
///   SENDER     "which weave emitted it?"                 — answered by the bus stamp
///   PROVENANCE "did Loom itself authorize this message's standing in a
///               lifecycle conversation?"                 — answered here
///
/// The three are related and NOT interchangeable, and conflating them is the
/// exact mistake this type exists to end. A perfectly-shaped `zen.Bequest` from
/// a weave that merely holds the grant for that shape passes the first two and
/// fails the third; before R2B-1 nothing could tell the difference, so a heir
/// claiming its inheritance by role had no way to know whether the answer came
/// from the steward it actually reached.
///
/// TWO AXES, DELIBERATELY, NOT ONE ENUM (R2D-0). The conversation/lifecycle
/// fact (`Kind`) and the AUTHORED OFFICE are different questions about one
/// delivery:
///
///   KIND          "what standing does Loom give this message in a
///                  conversation or a lifecycle?"    (None / Answer / Activation)
///   AUTHORED ROLE "which office did the sender deliberately speak as,
///                  with Loom verifying it held that office at authorship?"
///                  (empty = none — spoken personally)
///
/// They are carried as separate fields precisely so a future statement can be
/// both — an authenticated answer that is also office speech — without this
/// type being redesigned. The public V1 authoring surface does not produce the
/// combination; the representation refuses to make it unrepresentable.
///
/// IT IS NOT A PAYLOAD FIELD, AND IT CANNOT BECOME ONE. There is no wire
/// representation: it is never serialized, never part of any schema, and every
/// ordinary enqueue path (`send`, `send_to_role`, `publish`, and their `_as`
/// forms) OVERWRITES it with nothing. So a weave that stores a delivered Message
/// and re-sends it — the copy-what-you-observed attack — sends an ordinary
/// message. Only the Switchboard's attesting paths (the answer doors, the
/// lifecycle door, and the office-authorship doors) write a non-empty one.
class Provenance {
public:
    enum class Kind : std::uint8_t {
        None = 0,       ///< an ordinary message; it stands on shape and stamp alone
        Answer = 1,     ///< THE one authorized answer to a request this weave sent
        Activation = 2, ///< Loom attests a lifecycle commit for THIS incarnation
    };

    Provenance() = default;

    /// Describe the provenance the HOST computed for a delivery it is about to
    /// hand to its own handler.
    ///
    /// Deliberately public, and safe for a reason worth stating rather than
    /// hiding behind an access specifier: provenance has no wire representation
    /// and every enqueue path clears it, so constructing one can only describe a
    /// delivery the constructor is itself about to dispatch in its own process.
    /// It buys no reach. The one real caller is a weave library's own dispatch
    /// shim, translating the fact back across the C ABI (kernel/export.hpp) —
    /// and a library that lied here would be lying only to itself.
    static Provenance attested(Kind kind, std::int64_t sequence) noexcept {
        Provenance p;
        p.kind_ = kind;
        p.sequence_ = sequence;
        return p;
    }

    /// Attach the authored-office fact to this provenance — the second axis,
    /// composable onto any Kind. Public for exactly the reason `attested` is:
    /// there is nothing to reach with it. Every enqueue path overwrites a
    /// caller-supplied provenance, so the only writers whose value survives to a
    /// recipient are the Switchboard's own authorship doors (which verified the
    /// membership first) and a library dispatch shim describing a delivery its
    /// host already stamped.
    Provenance with_authored_role(std::string role) && {
        authored_role_ = std::move(role);
        return std::move(*this);
    }

    Kind kind() const noexcept { return kind_; }

    /// Is this delivery THE one authorized answer to a request this weave sent?
    bool answers_ask() const noexcept { return kind_ == Kind::Answer; }

    /// Does Loom attest a lifecycle commit for the incarnation being delivered to?
    bool lifecycle_activation() const noexcept { return kind_ == Kind::Activation; }

    /// The sequence Loom attested, for an activation. Compared by the consumer
    /// against the payload's own — an attestation for one sequence must not
    /// authenticate another.
    std::int64_t attested_sequence() const noexcept { return sequence_; }

    /// The office this delivery was DELIBERATELY authored as, verified by Loom
    /// at the authorship moment — or empty, which means exactly "spoken
    /// personally". Empty cannot be mistaken for a real office: an empty string
    /// is never a bindable role, and `authored_from_role` never matches it.
    ///
    /// A historical fact about the statement, never a claim about now: the
    /// author held the office when it deliberately spoke as it. Later role
    /// movement does not rewrite it, and current membership is a different
    /// question (`Switchboard::role_holder`).
    std::string_view authored_role() const noexcept { return authored_role_; }

    /// Was this delivery deliberately authored as `role`? False for empty
    /// `role`, so "no office" can never satisfy a membership question.
    bool authored_from_role(std::string_view role) const noexcept {
        return !role.empty() && authored_role_ == role;
    }

private:
    Kind kind_ = Kind::None;
    std::int64_t sequence_ = 0;
    /// The second axis. Empty = spoken personally (the overwhelmingly common
    /// case, and the default every ordinary enqueue restores).
    std::string authored_role_{};
};

/// The right to attach a lifecycle attestation — a capability OBJECT, not a
/// grant and not a payload flag.
///
/// THE BOUNDARY, STATED WHOLE. "The constructor is private" is not the
/// protection and never was — a private constructor behind a reachable factory
/// protects nothing, which is exactly the hole R2B-1 shipped and R2B-1a closes.
/// The durable statement is:
///
///   Lifecycle provenance can be attached only by trusted host/kernel
///   infrastructure holding an authority that is unavailable through the
///   supported weave-authoring surface.
///
/// Two walls hold that up, and both are the compiler's rather than a comment's:
///
///   1. THE MINT IS NON-STATIC AND PRIVATE ON THE SWITCHBOARD. Minting requires
///      the Switchboard ITSELF — and a weave never holds one. It is handed a
///      `Bus&`, and `Bus` has no such member. This is the same line that already
///      separates `send` (root) from `send_as` (a weave speaking as itself): the
///      Switchboard *is* the host's authority in this codebase, and lifecycle
///      minting now sits on the correct side of a boundary that already existed.
///   2. THE ONLY EXPRESSION THAT REACHES IT lives in a host-wiring header
///      (`zen/host/lifecycle_wiring.hpp`) that no weave-authoring header
///      includes, and is the Switchboard's one friend.
///
/// So the three tiers stay distinct, and the third is not implied by the first
/// two:
///
///   PUBLIC SHAPE       ordinary code may represent `zen.Activated`
///   EXACT GRANT        ordinary code may be permitted to EMIT it
///   LIFECYCLE AUTHORITY only Loom infrastructure may ATTEST it as a lifecycle fact
///
/// A grant to emit the shape is intentionally insufficient. Copying an authority
/// you were handed is ordinary — it is yours — and there is no path from "I know
/// the shape" or "I may send it" to "I hold the authority".
///
/// It does NOT widen the holder's grant either. An attested SEND is still
/// authorized against the sender's ordinary grant at delivery, so the authority
/// answers "may this message carry Loom's word?" and never "may this weave send
/// it?". `announce_lifecycle` is a send, and stays one.
///
/// ONE THING IS NOT A SEND, AND IT IS THE EXCEPTION THAT KEEPS THE RULE HONEST
/// (R2B-3d). The activation Loom delivers as part of an admission is not a
/// weave's speech being decorated — it is Loom's own act, authorized by this
/// authority at the moment the admission is scheduled and performed by the
/// Switchboard itself. No ordinary grant is consulted for it, because there is
/// no sender exercising one: the coordinator's id is stamped so the consumer's
/// per-operator lineage rule has something to key on, and that stamp describes
/// WHO ADMITTED rather than claiming who spoke.
///
/// The distinction is load-bearing rather than a carve-out. Before R2B-3d the
/// committed activation travelled the ordinary gated path, so a coordinator
/// without an `Emit<zen.Activated>` grant committed a replacement successfully
/// and its successor was refused its own first breath — publicly the service,
/// never told it was alive. An act the Loom performs cannot be un-performed by
/// re-asking a question about somebody's permission to speak.
///
/// AND IT IS RELATIVE TO THE LOOM THAT ISSUED IT (R2B-1a left this open;
/// R2B-1b closes it). R2B-1a required a `Switchboard&` to mint — but the result
/// was an EMPTY MARKER, so every board accepted every board's authority. An
/// ordinary weave could stand up a second Switchboard of its own, mint a
/// perfectly real authority from that decoy, and spend it through the running
/// system:
///
///     loom::Switchboard decoy;
///     auto forged = loom::host_lifecycle_authority(decoy);
///     mail.announce_lifecycle(forged, victim, loom::Activated{1}, 1);
///
/// Constructing that decoy is legal and stays legal — a Switchboard is an
/// ordinary object and anyone may own one. What changed is that an authority now
/// carries WHICH board issued it, and the issuing board checks. So:
///
///     Holding a Switchboard grants host authority only within that
///     Switchboard's Loom.
///
/// Every Loom is its own authority domain. The decoy's authority is not fake —
/// it is entirely real, and real somewhere else.
class LifecycleAuthority {
public:
    LifecycleAuthority(const LifecycleAuthority&) = default;
    LifecycleAuthority& operator=(const LifecycleAuthority&) = default;

private:
    friend class Switchboard;
    explicit LifecycleAuthority(std::weak_ptr<const LoomIdentity> issuer)
        : issuer_(std::move(issuer)) {}

    /// WEAK, deliberately. An authority must not keep its board's identity alive:
    /// the rule is that authority lasts only as long as the Loom that issued it,
    /// and a strong reference would quietly make a stale authority outlive the
    /// world it belonged to. Expired means refused.
    std::weak_ptr<const LoomIdentity> issuer_;
};

/// THE RIGHT TO ANSWER LATER — the same one answer, retained (R2B-2).
///
/// R2B-1's immediate authority lives and dies with the handler, which is right
/// for a responder that already knows the answer and is the whole reason that
/// path stays the smallest and strongest one. It is not enough for a responder
/// whose answer depends on messages it has not received yet: a dynamically
/// loaded steward, a preparation that needs queue turns, a Manager that is
/// itself a weave.
///
/// THE LAW: *an answer may outlive the handler, but never the conversation or
/// the incarnation that earned it.*
///
///   IMMEDIATE ANSWER AUTHORITY  one answer during the delivered request handler
///   DEFERRED ANSWER AUTHORITY   the SAME one answer, retained by the exact
///                               respondent incarnation that earned it
///   MESSAGE PAYLOAD             what the answer says
///   ANSWER PROVENANCE           why the requester may trust it as THE answer
///
/// WHAT IT IS NOT. Not a payload field, not serializable, not a future, not a
/// promise, and not an ambient send door: spending it requires a live `Mail`
/// belonging to the exact incarnation that earned it, so holding one buys the
/// right to finish ONE conversation and nothing else. The response shape is
/// still governed by that weave's ordinary emit grant — possession authorizes a
/// reply, never a vocabulary.
///
/// MOVE-ONLY, deliberately. Copying an answer right would invite two answers to
/// one question; making the type move-only means the compiler refuses the
/// question rather than the bus refusing the second answer. A default-constructed
/// one is INVALID and says so (`valid()`), so it can be held as a member before a
/// conversation exists — and spending an invalid one fails visibly rather than
/// silently doing nothing.
class DeferredAnswer {
public:
    DeferredAnswer() = default;
    DeferredAnswer(const DeferredAnswer&) = delete;
    DeferredAnswer& operator=(const DeferredAnswer&) = delete;
    DeferredAnswer(DeferredAnswer&& other) noexcept
        : issuer_(std::move(other.issuer_)), token_(other.token_) {
        other.token_ = 0; // moved-from is invalid: one right, one holder
    }
    DeferredAnswer& operator=(DeferredAnswer&& other) noexcept {
        if (this != &other) {
            issuer_ = std::move(other.issuer_);
            token_ = other.token_;
            other.token_ = 0;
        }
        return *this;
    }

    /// Does this name a conversation at all? False for a default one and for one
    /// that has been moved from. It does NOT promise the conversation is still
    /// answerable — only the bus can say that, and only at the moment of
    /// spending.
    bool valid() const noexcept { return token_ != 0; }

    /// The bus-private index this capability names.
    ///
    /// PUBLIC, AND INERT — the number is not the authority. Every spend is checked
    /// against the record's bound requester, respondent, both incarnations, the
    /// original correlation, AND the issuing board, so a number carries nothing on
    /// its own. It is exposed because a Bus implementation on the far side of the C
    /// ABI has to be able to hand it to the host, and hiding it behind friendship
    /// spanning a library boundary would buy obscurity rather than safety.
    std::uint64_t opaque_token() const noexcept { return token_; }

    /// Which Loom issued it. Empty for a capability held inside a dynamic library:
    /// there, board-relativity is structural instead — the host context a loaded
    /// weave spends through IS the board that delivered its request, so a token
    /// can only ever be presented to its own registry.
    std::weak_ptr<const LoomIdentity> issuer() const noexcept { return issuer_; }

    /// Rebuild a capability a dynamic library was handed across the C ABI.
    ///
    /// It carries NO issuer, and that is correct rather than a gap: a loaded
    /// weave can only ever present a token through the host context of the very
    /// delivery that gave it one, so board-relativity is STRUCTURAL there — the
    /// token reaches its own registry or no registry at all. Fabricating a number
    /// here buys nothing: every spend is still checked against the record's bound
    /// requester, respondent, both incarnations and correlation, so the worst a
    /// library can reach is a conversation it already owns.
    static DeferredAnswer from_host_token(std::uint64_t token) noexcept {
        DeferredAnswer d;
        d.token_ = token;
        return d;
    }

private:
    friend class Switchboard;
    DeferredAnswer(std::weak_ptr<const LoomIdentity> issuer, std::uint64_t token)
        : issuer_(std::move(issuer)), token_(token) {}

    /// Board-relative, exactly as LifecycleAuthority is: an answer right minted
    /// by one Loom has no standing in another, and dies with its issuer.
    std::weak_ptr<const LoomIdentity> issuer_;
    /// An index into bus-private state, not a secret and not a name a weave can
    /// invent usefully: every spend is checked against the record's bound
    /// requester, respondent, incarnations and correlation, so a fabricated
    /// number can at most reach a conversation the caller already owns.
    std::uint64_t token_ = 0;
};

/// The routed envelope. The payload is a self-describing Value — its own schema
/// is its routing shape. Routing metadata rides alongside it: who sent it, an
/// optional reply address, an optional opaque correlation token so a handler can
/// reply by sending (synchronous await is a deliberate seam, not built here),
/// and — set only by Loom, never by a sender — the delivery's provenance.
struct Message {
    Value payload;
    WeaveId sender{};
    WeaveId reply_to{};
    std::uint64_t correlation = 0;
    /// Written by the Switchboard alone; cleared on every ordinary enqueue. A
    /// value set here by a sender never survives to a recipient.
    Provenance provenance{};

    explicit Message(Value payload_, WeaveId sender_ = {}, WeaveId reply_to_ = {},
                     std::uint64_t correlation_ = 0)
        : payload(std::move(payload_)), sender(sender_), reply_to(reply_to_),
          correlation(correlation_) {}
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_MESSAGE_HPP
