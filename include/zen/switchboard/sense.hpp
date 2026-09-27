// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_SENSE_HPP
#define ZEN_SWITCHBOARD_SENSE_HPP

// Senses: the second thing a participant can say.
// SENSE-01..05; docs/laws/sense-laws.md · docs/reference/senses.md
//
//   MESSAGES   what happened, or what I want done   causal, FIFO, queued
//   SENSES     what I currently claim is so          acausal, latest-only, pulled
//
// A Sense is a participant's deliberate, immutable claim of the latest observation it has made
// available, read synchronously, for consumers that want known state many times (a renderer,
// an inspector, a status panel). It is not a second message system: it carries no causality,
// reorders nothing, never applies queued work to look current, and keeps one entry per current
// key. A latest claim is not a prediction: queued work may already make it stale.

#include <zen/switchboard/message.hpp> // WeaveId
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace loom {

/// Why a claim or an observation produced no value; each sends the reader somewhere different.
enum class SenseRefusal : std::uint8_t {
    None = 0,
    /// Nothing has been claimed under this key, or its key stopped meaning anything (the weave
    /// was removed, the role became unheld). A stale claim is a value, not this.
    NoClaim,
    /// The reader's grant does not permit observing this shape. Distinct from `NoClaim`, so a
    /// misconfigured grant never looks like an empty world.
    NotAuthorized,
    /// The claimant did not declare this shape in `Claims<...>` (claim side).
    Undeclared,
    /// The claimant does not hold the office it asked to claim as (claim side). Refused, never
    /// made a personal claim instead (as MSG-07 for speech).
    OfficeNotHeld,
    /// The value did not pass the gate for the shape it claimed (claim side).
    GateRefused,
};

const char* name_of(SenseRefusal r) noexcept;

/// Who claimed this, and whether that is still who it was: carried on every reading and never
/// recomputed, like office-authored delivery provenance (MSG-07).
struct SenseAuthorship {
    /// The exact weave that made the claim.
    WeaveId author{};
    /// The author's life and incarnation when it claimed.
    std::uint64_t author_life = 0;
    std::uint64_t author_incarnation = 0;
    /// Is that life still the life at that address? False once the author died and was
    /// revived, or was removed.
    bool author_life_is_current = false;
    /// Is that incarnation still the code at that address? Separate from the life: a live
    /// replacement changes the code without ending the life, leaving the life current and this
    /// false, so a reader can tell a predecessor's claim from the current code's.
    bool author_incarnation_is_current = false;
    /// The office this claim was deliberately authored as; empty for a personal claim, which
    /// is what a role holder's ordinary claim is.
    std::string office;
    /// For an office claim: does the author still hold the office? False after a replacement
    /// moved it; the claim still says the predecessor made it.
    bool office_holder_is_current = false;
    /// Increases with each claim under this key; not a clock, and not comparable across keys.
    std::uint64_t revision = 0;
    /// The shape claimed, so a reading describes itself.
    std::string schema_name;
    std::uint32_t schema_version = 0;

    /// True when this claim was authored as an office whose holder has since changed.
    bool office_claim_is_stale() const noexcept {
        return !office.empty() && !office_holder_is_current;
    }
};

/// One observation, by value: a copy the reader owns, with no reference into the claimant, so
/// reading a Sense confers no way to change its author. Changing another participant is still
/// done by message.
struct SenseReading {
    /// `None` iff `value` holds the claim; otherwise why not.
    SenseRefusal refusal = SenseRefusal::NoClaim;
    SenseAuthorship by{};
    /// The claim, by value; empty on a refusal. Optional because every `Value` has a schema and
    /// there is no blank one.
    std::optional<Value> value{};

    /// True iff a claim was read. A stale office claim is still a claim, so this stays true
    /// for it (see `SenseAuthorship::office_claim_is_stale`).
    explicit operator bool() const noexcept { return refusal == SenseRefusal::None; }
};

/// The result of claiming: `accepted` is the verdict, `why` the refusal when false, and
/// `revision` the claim's revision under its key when accepted.
struct SenseClaimResult {
    bool accepted = false;
    SenseRefusal why = SenseRefusal::None;
    std::uint64_t revision = 0;

    explicit operator bool() const noexcept { return accepted; }
};

// ---- Joint publication of latest claims ------------------------------------------------
// An operation over several claim keys, for owners whose facts must change together at one
// observable boundary. An operator holding a host-minted `JointAuthority` begins it, binding
// each key's exact claimant and revision; each claimant offers its key's next value from its
// own delivery; commit publishes every value in one step, running no participant code
// between two. Each claimant is then shown its value before its next delivery or snapshot,
// and the outcome is recorded (`JointApplication`); a claimant that failed is held. A record
// is kept until its operator releases it or the operator's life or incarnation changes.
// Bounded; no queue, answer, retry, timeout or durability; an operation aborts when a bound
// participant changes or a claimant claims ordinarily over a bound key.
// SENSE-06, SENSE-07; docs/reference/joint-publication.md

/// One latest-claim key as an operator names it: a claimant and a declared shape. Personal keys
/// only (docs/reference/joint-publication.md#what-is-not-here). The claimant may be named by
/// `role` instead, with `claimant` invalid: `begin` binds the role's holder at that moment, so
/// the role moving afterwards does not move the operation. A key naming both is refused.
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

/// Why a joint-publication verb did not do what it was asked; each names a different fix.
enum class JointRefusal : std::uint8_t {
    None = 0,
    /// Not called from inside a live delivery of the weave that presented it.
    NoLiveDelivery,
    /// The authority was not issued by this Loom, or its board is gone.
    ForeignAuthority,
    /// The caller is not the exact operator (id, life, incarnation) the authority names, or the
    /// operation is another operator's; refused before the record is touched.
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

/// An operation's state. `Preparing` is the only live one. A terminal record stays readable in
/// its slot until its operator releases it or is replaced, removed or dead; only then is the
/// slot reused, and `Exhausted` is the answer when none is free.
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

/// What became of a published value at its claimant. A commit publishes to every reader at once;
/// each claimant is then shown its value once, before its next delivery or snapshot, and the
/// outcome is kept on the claim record and the operation, so it outlives the claimant.
enum class JointApplication : std::uint8_t {
    /// Nothing is owed: an ordinary claim, or an operation that has not committed.
    None = 0,
    /// Published, and the claimant has not been shown it yet.
    Pending,
    /// Shown, and the claimant's hook completed.
    Applied,
    /// Shown, and the hook did not complete: a native throw, or a non-OK status across the seam.
    /// The claimant is held until reloaded or removed.
    Failed,
    /// The claimant was removed before it could be shown.
    Lost,
    /// Shown, and the claimant answered that it does not apply this value: it keeps state of its
    /// own and re-claims it at its next delivery. Functioning, not held, nothing retried.
    Declined,
};

const char* name_of(JointApplication a) noexcept;

struct JointStatus {
    JointStatus() = default;
    JointStatus(JointState s, JointRefusal r) : state(s), reason(r) {}

    JointState state = JointState::Missing;
    JointRefusal reason = JointRefusal::None;
    /// After a commit, the application over every bound claimant: Failed if any failed, else
    /// Lost, else Declined, else Pending, else Applied; None before a commit. `failed` names the
    /// claimant a Failed, Lost or Declined result is about, `failed_role` the office it was bound
    /// through, if any.
    JointApplication application = JointApplication::None;
    WeaveId failed{};
    std::string failed_role;
};

/// The right to coordinate joint publications, minted by the host
/// (`Switchboard::mint_joint_authority`) for one operator weave over a ceiling of roles: only
/// claims whose claimants hold one of them at `begin` may be bound. Every verb checks that the
/// presenter is the exact operator it names and that this Loom issued it; an operation id in a
/// payload is never authority.
///
/// It names a participant, not an address: it expires when the operator's life or incarnation
/// changes (a swap, a reload, a revival, a removal), and a successor holding a copy meets
/// `NotOperator` until the host mints again. It reaches only its holder's own operations:
/// another operator's id is refused `NotOperator` before that record is touched.
/// docs/reference/joint-publication.md#authority; SENSE-07.
class JointAuthority {
public:
    JointAuthority() = default;
    JointAuthority(const JointAuthority&) = default;
    JointAuthority& operator=(const JointAuthority&) = default;
    JointAuthority(JointAuthority&&) = default;
    JointAuthority& operator=(JointAuthority&&) = default;

    /// Does this name an operator? False for a default one and for one minted for an absent or
    /// dead operator. It does not promise the operator is still that life and incarnation; only
    /// the issuing Switchboard says that, at the moment of use.
    bool valid() const noexcept { return operator_.valid(); }
    WeaveId operator_id() const noexcept { return operator_; }
    /// The exact life and incarnation this was minted for, so a host can check it still names
    /// the operator it is about to hand it to.
    std::uint64_t operator_life() const noexcept { return life_; }
    std::uint64_t operator_incarnation() const noexcept { return incarnation_; }
    const std::vector<std::string>& ceiling() const noexcept { return ceiling_; }

private:
    friend class Switchboard;
    JointAuthority(std::weak_ptr<const LoomIdentity> issuer, WeaveId op, std::uint64_t life,
                   std::uint64_t incarnation, std::vector<std::string> ceiling)
        : issuer_(std::move(issuer)), operator_(op), life_(life), incarnation_(incarnation),
          ceiling_(std::move(ceiling)) {}

    /// Weak, like every authority's issuer: it does not keep its board alive, and one from a
    /// dead board never validates against a later one.
    std::weak_ptr<const LoomIdentity> issuer_;
    /// The operator's id, and its life and incarnation when minted, as a `ParticipantRef`
    /// would carry them; that type sits above this header.
    WeaveId operator_{};
    std::uint64_t life_ = 0;
    std::uint64_t incarnation_ = 0;
    std::vector<std::string> ceiling_;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SENSE_HPP
