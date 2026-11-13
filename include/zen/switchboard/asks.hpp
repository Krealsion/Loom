// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_SWITCHBOARD_ASKS_HPP
#define ZEN_SWITCHBOARD_ASKS_HPP

// The bus's record of an ask and of the answers it keeps: a unit the Switchboard consults, which
// no weave sees. An ask is an addressed send its asker sent through the ask door; Loom follows it
// with the records it already keeps -- the envelope while it is queued, the answer right while the
// handler runs, the kept answer after -- to its answer or to one `zen.AskEnded`.
// ANS-02, ANS-08; docs/laws/answer-authority-laws.md

#include <zen/switchboard/bus.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loom {

/// What Loom keeps of one ask while it is open, on each record that carries it in turn. Absent
/// (attempt 0) on a send that is not an ask. The asker's life and incarnation are the ones that
/// authored it: the ending is told to them and to nothing that stands at the address later.
struct AskFacts {
    std::uint64_t attempt = 0; ///< the ask's queued sequence; 0 = not an ask
    WeaveId asker{};
    std::uint64_t asker_life = 0;
    std::uint64_t asker_incarnation = 0;
    WeaveId target{};  ///< as addressed; invalid for a role address
    std::string role;  ///< the role as addressed, never its resolved holder
    std::string shape; ///< the ask's own shape
    std::uint32_t version = 0;
    std::uint64_t correlation = 0; ///< the ask's own, on its ending's delivery

    bool present() const noexcept { return attempt != 0; }
};

/// The facts of the ask `msg` is, queued as `attempt` by its sender at `life` and `incarnation`,
/// addressed to `target` or, when `role` is not empty, to `role`.
AskFacts ask_facts(const Message& msg, std::uint64_t attempt, std::uint64_t life,
                   std::uint64_t incarnation, WeaveId target, const std::string& role);

/// Each way an ask ends without its answer, as `zen.AskEnded` says it: the stage is the group,
/// and the order is `zen/weave/ask_ended.hpp`'s.
enum class AskEnding : std::uint8_t {
    // queued: no handler received it
    CapabilityDenied,
    NoSuchTarget,
    TargetUnavailable,
    NotAccepted,
    GateRefused,
    ApplicationFailed,
    AskerSealed,
    // delivered: a handler returned without spending or keeping the right
    Unanswered,
    HandlerFailed,
    // deferred: the kept right ended unspent
    Released,
    RespondentReplaced,
    RespondentRemoved,
    RespondentDied,
    RespondentRetired,
    // answered: the answer was refused
    AnswerRefused,
};

/// The ending a refusal of the ask's own envelope is, or false for a refusal no ask is told:
/// the asker's own life ended, or the refusal is not one of the safe pre-handler reasons.
bool ask_ending_of(RefusalReason reason, AskEnding& ending) noexcept;

/// `zen.AskEnded` for `ask`, ended `ending`: nothing the floor keeps out (no payload, no reply
/// address, no resolved holder, no gate detail).
Value ask_ended_value(const AskFacts& ask, AskEnding ending);

/// `zen.DispatchRefused` for the plain send queued as `attempt` to `target` or `role`, refused
/// for `told`, or nothing when `told` is not one of the six pre-handler reasons it says (MSG-12):
/// the sibling of an ask's `queued` ending, in the same words.
std::optional<Value> dispatch_refused_value(std::uint64_t attempt, WeaveId target,
                                            const std::string& role, const std::string& shape,
                                            std::uint32_t version, RefusalReason told);

/// One answer right a respondent kept (ANS-02), naming both participants' incarnations: a code
/// reload keeps the id, and the right belongs to the code that earned it.
struct KeptAnswer {
    std::uint64_t token = 0; ///< 0 = a free slot
    WeaveId requester{};
    std::uint64_t requester_incarnation = 0;
    std::uint64_t requester_life = 0; ///< the requester's life when it asked
    WeaveId respondent{};
    std::uint64_t respondent_incarnation = 0;
    std::uint64_t correlation = 0;
    /// The preparation ask this right answers (its `TxnId`'s value), if any, so a deferred
    /// readiness proves what an immediate one does.
    std::uint64_t preparation = 0;
    /// The ask this right answers, when it came through the ask door.
    AskFacts ask{};
};

/// The kept answers, and their two bounds: one Loom's and one respondent's. A slot is reclaimed
/// when the right is spent or released, or when either participant ends; the asks whose right
/// ended with the respondent are handed back, so the bus can tell each asker.
class KeptAnswers {
public:
    /// Why no slot was taken: this respondent holds its bound, or the Loom holds its own.
    enum class Full : std::uint8_t { No, Respondent, Loom };

    KeptAnswers(std::size_t loom_bound, std::size_t respondent_bound) noexcept
        : loom_bound_(loom_bound), respondent_bound_(respondent_bound) {}

    /// The record `token` names, or null.
    KeptAnswer* find(std::uint64_t token) noexcept;

    /// Keep `right` (its token ignored) in a free slot, and return the token minted for it, or 0
    /// with `full` saying which bound stood in the way: its respondent's, or the Loom's.
    std::uint64_t take(KeptAnswer right, Full& full);

    /// How many rights `respondent` keeps now.
    std::size_t held_by(WeaveId respondent) const noexcept;

    /// Free every right `id` is a party to, at any incarnation: the end of a life (ANS-04).
    /// Returns the asks whose respondent it was; a right it asked for ends with nobody to tell.
    std::vector<AskFacts> end_life(WeaveId id);

    /// Free every right `id` is a party to at an incarnation other than `now`: new code behind
    /// the id (`swap_state`). Returns the asks whose respondent it was.
    std::vector<AskFacts> end_incarnation(WeaveId id, std::uint64_t now);

    /// Free every right `respondent` keeps at `incarnation`: it was retired, and a retired
    /// weave's answer cannot leave. Returns the asks among them.
    std::vector<AskFacts> end_respondent(WeaveId respondent, std::uint64_t incarnation);

    /// Free `rec`, handing back what it carried.
    KeptAnswer release(KeptAnswer& rec) noexcept;

    /// What presenting `token` as `respondent` comes to, `taken` holding what the record held.
    /// Every term is a different attack: the record exists, unspent and unreleased; the speaker
    /// is the bound respondent (not a successor, the role's current holder or a copier of public
    /// values) at the incarnation that kept it, else `NotYours` and the record stays; and the
    /// requester still exists at the incarnation that asked, else `RequesterGone`, over either way
    /// and freed with nobody told, so a successor at the same id is never answered. `Spent` frees
    /// the record before any answer is queued.
    enum class Spend : std::uint8_t { Spent, NotYours, RequesterGone };
    template <class IncarnationOf>
    Spend spend(std::uint64_t token, WeaveId respondent, IncarnationOf incarnation_of,
                KeptAnswer& taken) {
        KeptAnswer* rec = find(token);
        if (rec == nullptr || rec->respondent != respondent ||
            incarnation_of(respondent) != rec->respondent_incarnation) {
            taken = rec == nullptr ? KeptAnswer{} : *rec;
            return Spend::NotYours;
        }
        const bool gone = incarnation_of(rec->requester) != rec->requester_incarnation;
        taken = release(*rec);
        return gone ? Spend::RequesterGone : Spend::Spent;
    }

private:
    template <class Ends>
    std::vector<AskFacts> end_where(WeaveId respondent_side, Ends ends);

    std::vector<KeptAnswer> slots_;
    std::size_t loom_bound_;
    std::size_t respondent_bound_;
    /// Never reused, and unguarded against exhaustion: process-local, never revived, and
    /// advancing by one per deferral.
    std::uint64_t next_token_ = 1;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_ASKS_HPP
