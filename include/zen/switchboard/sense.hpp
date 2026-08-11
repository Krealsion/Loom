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
#include <optional>
#include <string>

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

} // namespace loom

#endif // ZEN_SWITCHBOARD_SENSE_HPP
