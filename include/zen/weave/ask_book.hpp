// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_ASK_BOOK_HPP
#define ZEN_WEAVE_ASK_BOOK_HPP

// The asker keeps the book: one participant's record of the conversations it is still waiting
// on, which of them an arriving answer settles, and what it is waiting on, legibly.
// docs/reference/messaging.md#the-askers-own-book
//
// It is not a transport (it sends nothing and holds no Bus), not a protocol (it names no shape;
// what an answer means is the consumer's), not authority (the Grant decides who may speak, and
// `Mail::answers_ask` is a stronger fact an owner may also require), and not a future (no wait,
// timeout or callback). "Outstanding" means only "I asked, and no answer has settled it":
// nothing about whether the respondent received it or will reply.
//
// Settlement is a pair, and neither half is enough. The correlation says which conversation;
// it is a number any sender may choose, and identifies without authenticating (ANS-05). The
// bus-stamped sender says who spoke; the respondent itself may speak about another
// conversation. `loom::relay` (relay.hpp) is the same rule for a middleman relaying somebody
// else's answer.
//
// An ask to an office is answered by whoever holds it at delivery, unknown when asking:
// `open_to_role` says so, and its record constrains the correlation only.

#include <zen/switchboard/message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loom {

/// One conversation this asker is still waiting on: `id` is the small local number a person
/// types, `correlation` the number the answer carries back, `respondent` who may settle it,
/// `role` the office addressed when the respondent could not be known, and `shape`/`version`
/// what was asked. Whether it went to an office is derived from `role`, not stored.
struct PendingAsk {
    std::uint64_t id = 0;
    std::uint64_t correlation = 0;
    /// The weave whose answer settles this, compared with Loom's stamp; invalid when the ask went
    /// to an office.
    WeaveId respondent{};
    /// The office this ask was addressed to; empty for a directed one.
    std::string role;
    std::string shape;
    std::uint32_t version = 0;
    std::uint64_t attempt = 0; ///< queued send identity, explicitly bound by its author

    /// Was this ask addressed to an office rather than to one exact weave?
    bool to_role() const noexcept { return !role.empty(); }
};

/// What opening a conversation produced: the correlation the request must carry and the local
/// id to hold it by. At capacity the new ask is refused and every open one is kept.
struct AskOpened {
    bool ok = false;
    std::uint64_t id = 0;
    std::uint64_t correlation = 0;

    explicit operator bool() const noexcept { return ok; }
};

/// One asker's open conversations: ordinary state, held by the asking weave or its host. There
/// is no registry: two askers own two books that know nothing of each other.
class AskBook {
public:
    /// The bound is the owner's, with no default: a terminal, a load adapter and a test fixture
    /// each choose their own. Zero is refused.
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

    /// Every open conversation, in opening order.
    const std::vector<PendingAsk>& entries() const noexcept { return open_; }

    /// Is this ask still open? False for one settled, forgotten or never opened; the caller's own
    /// record says which.
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

    /// Open a conversation with one exact weave. The returned correlation is what the request
    /// must carry; nothing is sent. At capacity the new ask is refused before anything is
    /// recorded, and every open one is kept: unlike `loom::relay`, an asker never sheds its own
    /// record. An invalid `respondent` is refused, never read as "anybody"; use `open_to_role`.
    AskOpened open(WeaveId respondent, std::string shape = std::string(),
                   std::uint32_t version = 0) {
        if (!respondent.valid()) {
            return AskOpened{};
        }
        return record(respondent, std::string(), std::move(shape), version);
    }

    /// Open a conversation with whoever holds `role` at delivery. The respondent cannot be known
    /// now, so settlement is the correlation and a bus-stamped author, nothing narrower. An owner
    /// needing more requires Loom's answer provenance (`Mail::answers_ask`) as well.
    AskOpened open_to_role(std::string role, std::string shape = std::string(),
                           std::uint32_t version = 0) {
        if (role.empty()) {
            return AskOpened{};
        }
        return record(WeaveId{}, std::move(role), std::move(shape), version);
    }

    /// A number from this asker's correlation sequence for a message that is not an ask, so the
    /// participant has one sequence: a second counter could stamp a send with a number an open
    /// conversation is using, and its answer would settle that conversation.
    std::uint64_t mint_correlation() { return mint(); }

    /// Bind a record once to its actual queued attempt; this authenticates nothing. Zero,
    /// rebinding, and one attempt on two records are refused.
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

    /// Recognition only: the caller authenticates the notice and compares its authored facts
    /// before forgetting. Zero or ambiguous attempts match nothing.
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

    /// Which of my conversations does this arrival settle? nullptr for none. Read-only:
    /// recognizing and closing are two acts, and the consumer chooses when to close. The
    /// pointer is a view that `settle` and `forget` invalidate, as are `find`'s and `entries`';
    /// read what you need first, or use `settle`, which returns the record by value.
    const PendingAsk* match(std::uint64_t correlation, WeaveId from) const noexcept {
        for (const PendingAsk& p : open_) {
            if (matches(p, correlation, from)) {
                return &p;
            }
        }
        return nullptr;
    }

    /// Is `id` the conversation this arrival settles?
    bool is_settled_by(std::uint64_t id, std::uint64_t correlation, WeaveId from) const noexcept {
        const PendingAsk* p = find(id);
        return p != nullptr && matches(*p, correlation, from);
    }

    /// Close the conversation this arrival settles, and return the closed record by value:
    ///
    ///     if (const std::optional<loom::PendingAsk> mine = book.settle(corr, from)) {
    ///         // this arrival closed mine->id, and the record is still readable here
    ///     }
    ///
    /// A settled conversation is gone, so a duplicate or late copy of the answer settles nothing.
    std::optional<PendingAsk> settle(std::uint64_t correlation, WeaveId from) {
        for (std::size_t i = 0; i < open_.size(); ++i) {
            if (matches(open_[i], correlation, from)) {
                return take(i);
            }
        }
        return std::nullopt;
    }

    /// Stop tracking one conversation, locally only. Loom has no cancellation vocabulary, so the
    /// far end is told nothing and its answer may still arrive; a late answer to a forgotten ask
    /// matches nothing. Named `forget`, not `cancel`, because nothing is cancelled.
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
    /// The settlement rule, in one place.
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

    /// Never zero, and never a number an open conversation in this book is using, even across
    /// the counter wrapping (the skip is bounded by the capacity). It promises nothing about
    /// other askers' numbers, about settled or forgotten conversations, or about secrecy: a
    /// correlation is guessable and never a credential.
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

    /// The local handle, on its own counter: it counts asks only, so it stays a small number a
    /// person can read.
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
