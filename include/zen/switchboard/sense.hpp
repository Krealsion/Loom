// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_SENSE_HPP
#define ZEN_SWITCHBOARD_SENSE_HPP

// SENSES — the second thing a participant can say.
// SENSE-01..05; docs/laws/sense-laws.md · docs/reference/senses.md
//
//   MESSAGES   what happened / what I want done      causal, FIFO, queued
//   SENSES     what I currently claim is so          acausal, latest-only, pulled
//
// A Sense is A DELIBERATE IMMUTABLE CLAIM OF THE LATEST OBSERVATION A
// PARTICIPANT HAS MADE AVAILABLE. A renderer, inspector, status panel or editor
// warning wants already-known state many times; turning that into
// ask/FIFO/handler/answer/FIFO/reader adds traffic and latency without adding
// causality. So this is a repository of latest claims, read synchronously, and
// it is deliberately NOT a second message system:
//
//   - it carries no causality and participates in none;
//   - it reorders nothing and is reordered by nothing;
//   - it never applies queued work speculatively to look current;
//   - it never grows a journal — one entry per meaningful current key.
//
// THE TERM, used consistently everywhere: **latest claim**. Never "current
// state", never "same-frame truth", never "latest real state". A claim is not
// prophecy: pending FIFO work may already make it stale with respect to what
// happens next, and Loom will not pretend otherwise.

#include <zen/switchboard/message.hpp> // WeaveId
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace loom {

/// Why a claim or an observation did not produce a value. Each is a genuinely
/// different problem sending the reader somewhere different, so none of them is
/// an empty result.
enum class SenseRefusal : std::uint8_t {
    None = 0,
    /// NOTHING HAS EVER BEEN CLAIMED under this key — or what was claimed has
    /// been cleaned up because its key stopped meaning anything (the weave was
    /// unregistered; the role became unheld). Distinct from `NotAuthorized`,
    /// which is about the reader, and from a stale claim, which IS a value and
    /// arrives stamped rather than withheld.
    NoClaim,
    /// THE READER'S GRANT DOES NOT PERMIT OBSERVING THIS SHAPE. Reading a Sense
    /// is authorized like everything else in Loom: default-empty, host-granted,
    /// never widened in band. Deliberately distinct from `NoClaim` so a
    /// misconfigured grant cannot masquerade as "nobody has claimed anything" —
    /// the two send an operator to opposite places.
    NotAuthorized,
    /// THE CLAIMANT DID NOT DECLARE THIS SHAPE (claim side). A weave may claim
    /// only shapes it listed in `Claims<...>`, which is what makes Sense
    /// capabilities discoverable BEFORE the first runtime claim rather than
    /// after it. Undeclared is a maker error, and it is loud.
    Undeclared,
    /// THE CLAIMANT DOES NOT HOLD THE OFFICE it asked to claim as (claim side).
    /// The MSG-07 rule, reused because here it is exactly honest: holding is
    /// necessary and not sufficient, and asking to claim as an office you do not
    /// hold is refused at the claim moment — never downgraded to a personal
    /// claim.
    OfficeNotHeld,
    /// THE VALUE DID NOT PASS THE GATE for the shape it claimed (claim side).
    /// A Sense value crosses the same one gate every other value crosses; a
    /// malformed claim is refused rather than stored.
    GateRefused,
};

const char* name_of(SenseRefusal r) noexcept;

/// WHO CLAIMED THIS, AND IS THAT STILL WHO YOU THINK IT IS. Immutable, carried
/// on every reading, and never recomputed from later topology — the same
/// discipline office-authored delivery provenance follows (MSG-07).
///
/// A reader must be able to answer "who claimed this?" without trusting the
/// value, so a Sense value is never naked globally-trusted data.
struct SenseAuthorship {
    /// The exact weave that made the claim.
    WeaveId author{};
    /// The author's life and incarnation AT THE CLAIM MOMENT.
    std::uint64_t author_life = 0;
    std::uint64_t author_incarnation = 0;
    /// Is that life still the life at that address? False means the author has
    /// since died and been revived, or was removed — the claim is a fact about a
    /// life that has ended. The same question, and the same answer shape, as
    /// `BusEvent::sender_life` / `sender_life_now`.
    bool author_life_is_current = false;
    /// Is that INCARNATION still the code at that address? A SEPARATE QUESTION
    /// from the life, and the reason it exists is live replacement: a prepared
    /// replacement swaps the code behind an id without ending its life, so
    ///
    ///     life 7 / incarnation 3   claims X
    ///     ... replacement ...
    ///     life 7 / incarnation 4   is now current
    ///
    /// leaves `author_life_is_current == true` and this `false`. The claim stays
    /// historically truthful — it is still incarnation 3's, and Loom never
    /// rewrites it — but a reader that cannot tell "the predecessor's still-valid
    /// claim" from "the current incarnation's claim" cannot tell whether what it
    /// is reading survived the swap on purpose. Deriving this from
    /// `author_life_is_current` would erase exactly that distinction, which is
    /// why it is asked of the topology separately.
    bool author_incarnation_is_current = false;
    /// The office this claim was DELIBERATELY authored as; empty for a personal
    /// claim. Holding a role attaches nothing: a role-holder's personal claim
    /// arrives with this empty, which is the entire point.
    std::string office;
    /// Meaningful only when `office` is non-empty: does the author STILL hold
    /// that office? False after a replacement moved the role — the predecessor's
    /// claim is still readable and still says the predecessor claimed it. Loom
    /// never relabels it as the successor's.
    bool office_holder_is_current = false;
    /// Monotonic per key. Orders replacement of THIS claim: a higher revision is
    /// a later claim under the same key. Not a global clock and not comparable
    /// across keys.
    std::uint64_t revision = 0;
    /// The shape claimed. Carried so a reading is self-describing.
    std::string schema_name;
    std::uint32_t schema_version = 0;

    /// True when this claim was authored as an office whose holder has since
    /// changed. The reader decides what that means; Loom only refuses to hide it.
    bool office_claim_is_stale() const noexcept {
        return !office.empty() && !office_holder_is_current;
    }
};

/// ONE OBSERVATION, BY VALUE. The value is a copy the reader owns: there is no
/// pointer or reference into the claimant's state anywhere in this type, so
///
///     other.sense.health = 9000;
///
/// has no spelling. Mutation of another participant remains what it always was
/// — intentional Loom traffic (a domain message, a Poke, an authorized
/// operation) — and reading a Sense confers none of it.
struct SenseReading {
    /// `None` iff `value` holds the claim. Otherwise `value` is empty and this
    /// says exactly why, because refusal, staleness and absence are three
    /// different answers.
    SenseRefusal refusal = SenseRefusal::NoClaim;
    SenseAuthorship by{};
    /// The claim, by value. `std::optional` rather than a defaulted `Value`
    /// because a `Value` always claims a schema — there is no such thing as a
    /// blank one, and inventing an empty-schema placeholder to fill this slot
    /// would be a value nobody claimed.
    std::optional<Value> value{};

    /// True iff a claim was read. Staleness does NOT make this false — a stale
    /// office claim is a real claim, honestly stamped (see
    /// `SenseAuthorship::office_claim_is_stale`), and collapsing the two would
    /// destroy the distinction between "this office has never claimed" and
    /// "this office's claim is the previous holder's".
    explicit operator bool() const noexcept { return refusal == SenseRefusal::None; }
};

/// THE RESULT OF CLAIMING. `accepted` is the whole verdict; `why` names the
/// refusal when it is false. `revision` is the claim's own sequence under its
/// key, meaningful only when accepted.
struct SenseClaimResult {
    bool accepted = false;
    SenseRefusal why = SenseRefusal::None;
    std::uint64_t revision = 0;

    explicit operator bool() const noexcept { return accepted; }
};

// =============================================================================
// JOINT PUBLICATION OF LATEST CLAIMS
// =============================================================================
//
// Reference: docs/reference/joint-publication.md. Laws: SENSE-06 (a publication
// is shown, and what the showing came to is recorded, never assumed) and
// SENSE-07 (a record is kept until its operator releases it) in
// docs/laws/sense-laws.md. The consumer this was built for is an application
// whose document owner and presentation owner must change their facts TOGETHER
// at one observable boundary; Loom supplies the mechanism and no vocabulary of
// either.
//
// WHAT IT ADDS TO A SENSE. A latest claim already is a bus-owned, gate-admitted,
// revisioned, exactly-attributed value that dies with its claimant (SENSE-01..05).
// A JOINT PUBLICATION is an operation over several such keys:
//
//   begin    an OPERATOR holding a host-minted `JointAuthority` binds the exact
//            claimants (life + incarnation) of several claim keys and the keys'
//            current revisions;
//   offer    each claimant, from inside its own delivery, offers the NEXT value
//            of its own key for that exact operation (declared shape, the gate,
//            the bound revision);
//   commit   the operator asks the bus to publish: the bus revalidates every
//            participant, revision and offer, then exchanges every offered value
//            into its claim record in ONE step, running no participant code,
//            gate, allocation, I/O or cleanup between two exchanges;
//   hook     a claimant whose key was published BY AN OPERATION (not by its own
//            claim) hears `Weave::claim_published` before its next delivery and
//            before its next snapshot, so a reader can never observe the weave
//            behind its own published claim.
//
// PUBLICATION IS NOT APPLICATION. What each
// showing came to is a second fact the bus keeps (`JointApplication`, on the claim
// record and on the operation): Pending until shown, Applied when the hook completed,
// Declined when the claimant answered that it keeps state of its own instead (it is
// functioning and NOT held; its next ordinary claim replaces the published value),
// Failed when the hook did not complete -- a native throw, or a non-OK status across
// the seam -- and Lost when the claimant was removed unshown. A Failed claimant is
// HELD: deliveries to it are refused `ApplicationFailed`, its ordinary snapshot is
// refused, and the hook is not re-run; a reload (its successor is shown again) or a
// removal ends the hold. The operator is told once per settlement (`zen.JointApplied`)
// and re-reads `joint_status`.
//
// PUBLICATION IS NOT THE END OF AN OUTCOME'S LIFETIME, AND A QUEUED NOTIFICATION IS
// NOT CONSUMPTION. An operation's record is
// what its operator re-reads when the bus's notice reaches it, so the record is KEPT --
// Committed with its application, or Aborted with its reason -- until the operator
// RELEASES it (`release_joint`, its own explicit act), or until the operator's own life
// or incarnation changes (nobody is left to consume it: the bus releases it; a
// successor at the same address inherits nothing). Only a released slot is reused. The
// bound is `kMaxJointOperations` records live or unreleased; an operator that never
// releases meets `Exhausted` at its next begin, in words, and never another operator's
// outstanding record reused under it. After a release the operation reads Missing,
// legitimately. The per-key facts -- application, publishing operation, revision --
// stay on the claim record, so a held claimant is still held and still repaired, and a
// repair's re-settlement of a released record is owed to nobody.
//
// WHAT IT DOES NOT ADD. No document, layout or application vocabulary; no queue;
// no answer; no retry; no timeout; no durability across a process. An operation
// is bounded (`Switchboard::kMaxJointOperations`, `kMaxJointKeys`,
// `kMaxJointOfferBytes`), one live operation binds a key at a time, and an
// operation is aborted — its offers released — the moment a bound participant's
// life or incarnation changes, or its claimant claims ordinarily over a bound key.

/// One latest-claim key, as an operator names it: a claimant and a declared shape.
/// Personal keys only: an office key is a different key space, and no consumer
/// has needed one bound (docs/reference/joint-publication.md#what-is-not-here).
///
/// THE CLAIMANT MAY BE NAMED BY ROLE. An operator that coordinates offices rather
/// than weaves leaves `claimant` invalid and fills `role`; `begin`
/// resolves the office to its holder AT THAT MOMENT and binds that exact
/// participant, so a role that moves afterwards does not move the operation.
/// A key naming both is refused as the contradiction it is.
struct ClaimKey {
    WeaveId claimant{};
    std::string role;
    std::string schema_name;
    std::uint32_t schema_version = 0;

    friend bool operator==(const ClaimKey& a, const ClaimKey& b) noexcept {
        return a.claimant == b.claimant && a.role == b.role && a.schema_name == b.schema_name &&
               a.schema_version == b.schema_version;
    }
};

/// Why a joint-publication verb did not do what it was asked. Each names a
/// different fix, exactly as `SenseRefusal` does; none is an empty result.
enum class JointRefusal : std::uint8_t {
    None = 0,
    /// Not spoken from inside a live delivery of the weave that presented it — a
    /// Bus that is not a live participating context refuses truthfully.
    NoLiveDelivery,
    /// The authority was not issued by this Loom, or its board is gone.
    ForeignAuthority,
    /// The caller is not the exact operator (weave, life and incarnation) the
    /// authority names — a successor at the same address inherits nothing.
    NotOperator,
    /// A named claimant holds none of the roles the authority's ceiling names.
    OutsideCeiling,
    /// The key has no current claim to bind or to offer against.
    NoClaim,
    /// Another live operation already binds this key.
    KeyBusy,
    /// No operation slot is free, or more keys than one operation may bind.
    Exhausted,
    NoSuchOperation,
    /// The operation is not Preparing (already committed, aborted, or cancelled).
    WrongState,
    /// An offer for a key the operation does not bind.
    NotBound,
    /// An offer from a weave that does not own the key it offers for.
    NotClaimant,
    /// The key's revision moved since begin — its claimant claimed ordinarily.
    StaleRevision,
    /// The offered shape is not in the claimant's declared claim-set.
    Undeclared,
    /// The offered value did not pass the gate for its shape.
    GateRefused,
    /// The offered value's serialized size exceeds `kMaxJointOfferBytes`.
    TooLarge,
    /// A bound participant's life or incarnation changed, or it was removed.
    ParticipantChanged,
    /// Commit asked before every bound key had an offer.
    OfferMissing,
    /// The operator cancelled it explicitly.
    Cancelled,
};

const char* name_of(JointRefusal r) noexcept;

/// The whole state machine. `Preparing` is the only live state; the two terminals
/// are terminal, and a terminal record stays in its slot -- readable, its outcome
/// owed to its operator -- until that operator releases it or is itself replaced,
/// removed or dead; only then is the slot reused (bounded, deliberately — there is
/// no journal here, and `Exhausted` is the answer when nothing was released).
enum class JointState : std::uint8_t { Missing, Preparing, Committed, Aborted };

const char* name_of(JointState s) noexcept;

struct JointBegin {
    bool ok = false;
    std::uint64_t op = 0;
    JointRefusal why = JointRefusal::None;
    explicit operator bool() const noexcept { return ok; }
};

struct JointResult {
    bool ok = false;
    JointRefusal why = JointRefusal::None;
    explicit operator bool() const noexcept { return ok; }
};

/// WHAT BECAME OF A PUBLISHED VALUE AT ITS CLAIMANT.
///
/// A commit PUBLISHES: every reader sees the new values from that instant. Each
/// claimant is then SHOWN its value once, before its next delivery and before its
/// next snapshot, and that showing is a second, attributable fact -- kept on the
/// claim record and on the operation, so it outlives the claimant's removal.
enum class JointApplication : std::uint8_t {
    /// Nothing is owed: an ordinary claim, or an operation that has not committed.
    None = 0,
    /// Published, and the claimant has not been shown it yet.
    Pending,
    /// Shown, and the claimant's hook completed.
    Applied,
    /// Shown, and the hook did not complete -- a native throw, or a non-OK status
    /// across the seam. The claimant is HELD until reloaded or removed.
    Failed,
    /// The claimant was removed before it could be shown.
    Lost,
    /// Shown, and the claimant ANSWERED that it does not apply this value: it keeps
    /// or reconciles state of its own and re-claims that truth at its next delivery
    ///. Functioning, not held, nothing
    /// retried; the publication stands on the claim record until that claim. What
    /// a successor says when shown a value its predecessor prepared and it did not.
    Declined,
};

const char* name_of(JointApplication a) noexcept;

struct JointStatus {
    JointStatus() = default;
    JointStatus(JointState s, JointRefusal r) : state(s), reason(r) {}

    JointState state = JointState::Missing;
    JointRefusal reason = JointRefusal::None;
    /// After a commit, the application over every bound claimant: Failed if any
    /// failed, else Lost if any was removed unshown, else Declined if any declined,
    /// else Pending if any is still to be shown, else Applied. None while not
    /// committed. `failed` names the claimant a Failed, Lost or Declined aggregate
    /// is about, and `failed_role` the office it was bound through, if any.
    JointApplication application = JointApplication::None;
    WeaveId failed{};
    std::string failed_role;
};

/// THE RIGHT TO COORDINATE A JOINT PUBLICATION — host-minted, operator-bound.
///
/// Minted by the host (`Switchboard::mint_joint_authority`) for ONE operator weave
/// over a CEILING of roles: only claims whose claimants hold one of those roles at
/// `begin` may be bound. Presenting it authorizes nothing by itself: every verb
/// checks that the presenter is the exact operator (id, life and incarnation) it
/// names and that this Loom issued it. An operation id in a payload is never
/// authority; this object plus the live delivery is.
class JointAuthority {
public:
    JointAuthority() = default;
    JointAuthority(const JointAuthority&) = default;
    JointAuthority& operator=(const JointAuthority&) = default;
    JointAuthority(JointAuthority&&) = default;
    JointAuthority& operator=(JointAuthority&&) = default;

    bool valid() const noexcept { return operator_.valid(); }
    WeaveId operator_id() const noexcept { return operator_; }
    const std::vector<std::string>& ceiling() const noexcept { return ceiling_; }

private:
    friend class Switchboard;
    JointAuthority(std::weak_ptr<const LoomIdentity> issuer, WeaveId op,
                   std::vector<std::string> ceiling)
        : issuer_(std::move(issuer)), operator_(op), ceiling_(std::move(ceiling)) {}

    /// WEAK, for the reason every authority's is: it must not keep its board
    /// alive, and one from a dead world must not validate against a later board.
    std::weak_ptr<const LoomIdentity> issuer_;
    WeaveId operator_{};
    std::vector<std::string> ceiling_;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SENSE_HPP
