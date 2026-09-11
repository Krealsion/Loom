// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_ASK_BOOK_HPP
#define ZEN_WEAVE_ASK_BOOK_HPP

// THE ASKER KEEPS THE BOOK — one participant's record of the conversations IT is
// still waiting on.
//
//     the asker owns the record of what it is still asking.
//
// Three versions of this record already existed in this tree, written independently
// and to three different standards: `TerminalSession`'s bounded `pending_` list, a
// one-slot load record in Zengine's plan executor, and the hand-rolled
// correlation-plus-expected-sender pair in every host that commands a load. Each
// had to rediscover the same two facts, and the weakest of them was the one a
// stranger copied out of an example. This file is that record, harvested once.
//
// ---- WHAT IT OWNS ---------------------------------------------------------------
//
//     which of MY conversations are still open
//     which conversation an arriving answer belongs to, if any
//     what I am waiting on, legibly enough to show somebody
//
// ---- WHAT IT DOES NOT OWN, and each absence is deliberate ------------------------
//
//   IT IS NOT A TRANSPORT. It sends nothing, holds no `Bus`, no `Switchboard` and no
//   channel, and has no verb that could author a message. The owner does the asking;
//   this only remembers that it did.
//
//   IT IS NOT A PROTOCOL. No shape is defined here, none is required, and none is
//   understood: `zen.Result`, `zen.Ack` and `zen.Refused` do not appear in this file
//   and neither does any other vocabulary. A conversation may be answered by any
//   shape at all, and what the answer MEANS stays entirely with the consumer — which
//   is the difference between owning a conversation and owning a result.
//
//   IT IS NOT AUTHORITY. Nothing here admits, gates, authenticates or grants. A
//   record saying "I am waiting on weave 7" is a note this participant wrote to
//   itself; the wall deciding who may say anything at all in this host is the Grant,
//   and Loom's own answer provenance is a separate, stronger fact an owner is free to
//   require on top of this one (`Mail::answers_ask`).
//
//   IT IS NOT A FUTURE, A PROMISE, A TASK OR A SCHEDULER. There is no wait, no
//   timeout, no callback, no continuation and no thread. The host still turns the
//   dispatch crank; what this removes is the need to reinvent what SETTLED means.
//
//   IT KNOWS NOTHING ABOUT THE REMOTE OPERATION. "Outstanding" means exactly *I
//   asked, and no answer of mine has settled it*. It does not mean the respondent
//   received the ask, owes an answer, is working on it, or will ever reply — and an
//   ask may stay outstanding forever unless this asker locally stops tracking it.
//   Loom has no unanswerability notice today, and this book does not pretend
//   otherwise.
//
// ---- THE SETTLEMENT RULE, AND WHY IT IS A PAIR ----------------------------------
//
//     correlation        WHICH conversation an arrival names
//     bus-stamped from   WHO is speaking — stamped by Loom, never read from a payload
//
// Neither half is sufficient, and they fail in opposite directions:
//
//   A CORRELATION IDENTIFIES; IT DOES NOT AUTHENTICATE (ANS-05). It is a number a
//   sender chooses. The first conversation of any fresh book is 1 — a number every
//   other asker is equally likely to be using, and one anybody may put on any
//   message. Matching it proves an arrival NAMES this conversation, never that it had
//   any business answering it.
//
//   THE SENDER SAYS WHO SPOKE, NOT WHAT ABOUT. The bus stamps it, so no participant
//   can claim another's identity — but the exact weave this asker is waiting on is
//   perfectly able to say something admissible about a DIFFERENT conversation, which
//   is the shape a stale answer actually takes.
//
// This is the standing obligation `zen/weave/standard_shapes.hpp` states for any
// weave that accepts a standard reply shape, implemented once. `loom::relay`
// (relay.hpp) is the same wall for a MIDDLEMAN relaying somebody else's answer; this
// is the wall for the participant that asked. They are deliberately not one type: a
// relay's record is about the asker it answers FOR, sheds its oldest entry when full,
// and has no conversation of its own to lose.
//
// ---- WHEN THE RESPONDENT IS NOT KNOWABLE ----------------------------------------
//
// An ask addressed to an OFFICE is answered by whoever holds it at delivery, which is
// a fact this asker does not have when it asks. `open_to_role` is that case said out
// loud, and its record constrains the correlation and nothing narrower. There is no
// third door: a respondent is either a weave this asker named or an office it
// addressed, and an invalid `WeaveId` is never quietly accepted as "anybody".

#include <zen/switchboard/message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loom {

/// ONE CONVERSATION THIS ASKER IS STILL WAITING ON.
///
/// Every field answers a question a live consumer asks. `id` is the small stable
/// number a person types and a caller holds; `correlation` is the wire identity Loom
/// echoes back on the answer; `respondent` is who may settle it; `role` is the office
/// addressed when the respondent could not be known; `shape`/`version` are what was
/// asked, which is what "what am I waiting on?" means to a reader.
///
/// THE ADDRESSING KIND IS DERIVED, NOT STORED. A third stored field could disagree
/// with the two it summarizes: `role` is non-empty exactly when the respondent was
/// unknowable, and that is the whole distinction.
struct PendingAsk {
    std::uint64_t id = 0;
    std::uint64_t correlation = 0;
    /// The weave whose answer settles this, or the invalid id when the ask went to an
    /// office. NEVER a payload field: it is compared against Loom's own stamp.
    WeaveId respondent{};
    /// The office this ask was addressed to; empty for a directed one.
    std::string role;
    std::string shape;
    std::uint32_t version = 0;
    std::uint64_t attempt = 0; ///< queued send identity, explicitly bound by its author

    /// Was this ask addressed to an office rather than to one exact weave?
    bool to_role() const noexcept { return !role.empty(); }
};

/// WHAT OPENING A CONVERSATION PRODUCED — the correlation the request must carry, and
/// the local number this asker will hold it by.
///
/// It can fail, and the failure is the point: at capacity the NEW ask is refused and
/// every outstanding one is untouched (see `AskBook::open`).
struct AskOpened {
    bool ok = false;
    std::uint64_t id = 0;
    std::uint64_t correlation = 0;

    explicit operator bool() const noexcept { return ok; }
};

/// ONE ASKER'S OUTSTANDING CONVERSATIONS.
///
/// Ordinary state. Hold one per asker — a member of the weave that asks, or of the
/// host state its handler writes into. THERE IS NO REGISTRY: nothing here is global,
/// nothing is added to the Switchboard, and two askers in one process own two books
/// that have never heard of each other.
class AskBook {
public:
    /// A BOOK IS BOUNDED, AND THE BOUND IS THE OWNER'S TO CHOOSE. There is no default
    /// on purpose: a number invented here would become a Loom law by accident, and a
    /// terminal's eight, a load adapter's four and a fixture's one are three different
    /// product decisions that merely happen to be the same kind of number.
    explicit AskBook(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "loom::AskBook: a book with no room can never hold a conversation; give it the "
                "number of simultaneous asks this owner is willing to track");
        }
    }

    // ---- what is outstanding ------------------------------------------------

    /// Is this asker waiting on anything at all?
    bool awaiting() const noexcept { return !open_.empty(); }
    std::size_t outstanding() const noexcept { return open_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool full() const noexcept { return open_.size() >= capacity_; }

    /// Every open conversation, in the order they were opened. By reference; an owner
    /// that hands it out copies it.
    const std::vector<PendingAsk>& entries() const noexcept { return open_; }

    /// Is THIS ask still outstanding? False for one that settled, one this asker
    /// forgot, and one that never existed — a caller's loop stops on false either way,
    /// and the caller's own record is what says which it was.
    bool waiting_on(std::uint64_t id) const noexcept { return find(id) != nullptr; }

    /// The open conversation with this local id, or nullptr.
    const PendingAsk* find(std::uint64_t id) const noexcept {
        if (id == 0) {
            return nullptr;
        }
        for (const PendingAsk& p : open_) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    }

    // ---- opening ------------------------------------------------------------

    /// OPEN A CONVERSATION WITH ONE EXACT WEAVE. The returned correlation is what the
    /// request must carry; nothing is sent here, and nothing is claimed about whether
    /// the request will arrive.
    ///
    /// REFUSED AT CAPACITY, AND THE OUTSTANDING ONES ARE UNTOUCHED. A new ask must
    /// never displace a conversation somebody is waiting on, so this refuses before it
    /// records anything and the caller authors nothing. It is deliberately not
    /// `loom::relay`'s shed-the-oldest policy: a middleman that loses a forward
    /// disappoints one asker, while an asker that loses its own record forgets what it
    /// is doing.
    ///
    /// An invalid `respondent` is refused rather than read as "anybody" — say
    /// `open_to_role` when that is what you mean.
    AskOpened open(WeaveId respondent, std::string shape = std::string(),
                   std::uint32_t version = 0) {
        if (!respondent.valid()) {
            return AskOpened{};
        }
        return record(respondent, std::string(), std::move(shape), version);
    }

    /// OPEN A CONVERSATION WITH WHOEVER HOLDS `role` AT DELIVERY.
    ///
    /// The respondent is not knowable here, so the record cannot constrain it and says
    /// so: settlement for this ask is the correlation plus a bus-stamped author, and
    /// nothing narrower. An owner that needs the narrower wall has one this type
    /// cannot supply for it — Loom's own answer provenance, which no ordinary enqueue
    /// can set.
    AskOpened open_to_role(std::string role, std::string shape = std::string(),
                           std::uint32_t version = 0) {
        if (role.empty()) {
            return AskOpened{};
        }
        return record(WeaveId{}, std::move(role), std::move(shape), version);
    }

    /// A NUMBER FROM THIS ASKER'S OWN CORRELATION SEQUENCE, for a message that is NOT
    /// an ask.
    ///
    /// It exists so one participant has ONE sequence rather than two. An owner that
    /// kept a second counter beside this book could stamp an ordinary send with a
    /// number an open conversation is already using, and an answer to that send would
    /// then settle the conversation.
    std::uint64_t mint_correlation() { return mint(); }

    /// Bind once to an actual queued attempt. This supplies no authentication.
    /// Zero, rebinding, and sharing an attempt between records are refused.
    bool bind_attempt(std::uint64_t id, std::uint64_t attempt) noexcept {
        if (attempt == 0) { return false; }
        for (const PendingAsk& p : open_) {
            if (p.attempt == attempt) { return false; }
        }
        for (PendingAsk& p : open_) {
            if (p.id == id && p.attempt == 0) { p.attempt = attempt; return true; }
        }
        return false;
    }

    /// Local recognition only: caller must authenticate the notice and compare
    /// its authored facts before forgetting. Zero/ambiguous identities match none.
    const PendingAsk* match_attempt(std::uint64_t attempt) const noexcept {
        if (attempt == 0) { return nullptr; }
        const PendingAsk* found = nullptr;
        for (const PendingAsk& p : open_) {
            if (p.attempt == attempt) {
                if (found != nullptr) { return nullptr; }
                found = &p;
            }
        }
        return found;
    }

    // ---- settling -----------------------------------------------------------

    /// WHICH OF MY CONVERSATIONS DOES THIS ARRIVAL SETTLE? nullptr for none.
    ///
    /// READ-ONLY, ON PURPOSE. Merely looking at a matching-looking message must not
    /// destroy the bookkeeping the consumer is about to need, so recognizing and
    /// closing are two acts and the consumer decides when the second one happens.
    ///
    /// ⚠ THE POINTER IS A VIEW INTO THIS BOOK and does not outlive a change to it —
    /// `settle` and `forget` both move the vector under it. Read what you need before
    /// either, or use `settle`, which hands the closed record back BY VALUE precisely
    /// so a consumer never has to hold one of these across the act. The same applies
    /// to `find` and to `entries`.
    const PendingAsk* match(std::uint64_t correlation, WeaveId from) const noexcept {
        for (const PendingAsk& p : open_) {
            if (matches(p, correlation, from)) {
                return &p;
            }
        }
        return nullptr;
    }

    /// Is `id` the conversation this arrival settles? For a consumer that already
    /// knows which of its asks it is handling.
    bool is_settled_by(std::uint64_t id, std::uint64_t correlation, WeaveId from) const noexcept {
        const PendingAsk* p = find(id);
        return p != nullptr && matches(*p, correlation, from);
    }

    /// CLOSE THE CONVERSATION THIS ARRIVAL SETTLES, and hand back the record that was
    /// closed — so a consumer needs no second lookup and no ordering rule:
    ///
    ///     if (const std::optional<loom::PendingAsk> mine = book.settle(corr, from)) {
    ///         // this arrival closed mine->id, and the record is still readable here
    ///     }
    ///
    /// A settled conversation is gone, which is what makes a duplicate or late copy of
    /// the same answer inert rather than a second settlement.
    std::optional<PendingAsk> settle(std::uint64_t correlation, WeaveId from) {
        for (std::size_t i = 0; i < open_.size(); ++i) {
            if (matches(open_[i], correlation, from)) {
                return take(i);
            }
        }
        return std::nullopt;
    }

    /// STOP TRACKING ONE CONVERSATION — LOCALLY, AND ONLY LOCALLY.
    ///
    /// Loom has no cancellation vocabulary, so nothing at the far end is told
    /// anything: whatever was asked may still be being done, and its answer may still
    /// arrive. What changes is only that this asker no longer recognizes it — a late
    /// answer to a forgotten ask matches nothing, settles nothing, and cannot
    /// resurrect the record. It is named `forget` rather than `cancel` because the
    /// shorter word would be the lie.
    std::optional<PendingAsk> forget(std::uint64_t id) {
        if (id == 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < open_.size(); ++i) {
            if (open_[i].id == id) {
                return take(i);
            }
        }
        return std::nullopt;
    }

private:
    /// THE RULE, IN ONE PLACE. Written once because a wall applied on some paths and
    /// not on others is not a wall.
    static bool matches(const PendingAsk& p, std::uint64_t correlation,
                        WeaveId from) noexcept {
        if (correlation == 0 || correlation != p.correlation) {
            return false; // a different conversation, or none at all
        }
        if (!from.valid()) {
            return false; // nothing without a bus-stamped author settles an ask
        }
        // ...and when this asker named its respondent, that is who may settle it.
        return !p.respondent.valid() || from == p.respondent;
    }

    AskOpened record(WeaveId respondent, std::string role, std::string shape,
                     std::uint32_t version) {
        if (full()) {
            return AskOpened{};
        }
        PendingAsk p;
        p.id = next_id();
        p.correlation = mint();
        p.respondent = respondent;
        p.role = std::move(role);
        p.shape = std::move(shape);
        p.version = version;
        const AskOpened opened{true, p.id, p.correlation};
        open_.push_back(std::move(p));
        return opened;
    }

    std::optional<PendingAsk> take(std::size_t i) {
        PendingAsk closed = std::move(open_[i]);
        open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(i));
        return closed;
    }

    /// MONOTONIC, NEVER ZERO, AND NEVER A NUMBER AN OPEN CONVERSATION IS USING.
    ///
    /// WHAT THIS PROMISES, EXACTLY: no two conversations open in THIS BOOK at the SAME
    /// TIME share a correlation. That is the property settlement needs, and it holds
    /// even across the counter wrapping — the skip below is bounded by the book's
    /// capacity, so it terminates.
    ///
    /// WHAT IT DOES NOT PROMISE: anything about another participant's numbering (two
    /// askers in one process will both open a conversation numbered 1), anything about
    /// conversations already settled or forgotten, and anything whatsoever about
    /// secrecy. A correlation is guessable by design and is never a credential.
    std::uint64_t mint() {
        for (;;) {
            if (++correlation_ == 0) {
                ++correlation_; // 0 is the "no conversation" sentinel, never a number
            }
            bool taken = false;
            for (const PendingAsk& p : open_) {
                if (p.correlation == correlation_) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                return correlation_;
            }
        }
    }

    /// The local handle, on its own counter. It counts ASKS, so it stays the small
    /// number a person reads, while the correlation above counts everything this asker
    /// draws from the book.
    std::uint64_t next_id() {
        for (;;) {
            if (++ask_ == 0) {
                ++ask_;
            }
            if (find(ask_) == nullptr) {
                return ask_;
            }
        }
    }

    std::size_t capacity_;
    std::vector<PendingAsk> open_;
    std::uint64_t correlation_ = 0;
    std::uint64_t ask_ = 0;
};

} // namespace loom

#endif // ZEN_WEAVE_ASK_BOOK_HPP
