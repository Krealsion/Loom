// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_RELAY_HPP
#define ZEN_WEAVE_RELAY_HPP

// The request/reply relay: forward a request to a named target, and relay its answer back to
// whoever asked, matched by correlation and by the target as bus-stamped sender. For a weave
// fronting a protocol for an operator, such as the poke weave. It keeps only the correlation
// bookkeeping (RelayState) and two moves, forward and relay: no expected-reply registry, no
// timeout. RelayState is ordinary weave state, snapshotted, revived and inspectable.
//
// Not `loom::AskBook` (ask_book.hpp): this is a middleman's record, each entry on behalf of
// somebody else, and it sheds the oldest when full; an asker's book refuses a new conversation
// rather than drop its own. docs/reference/messaging.md#the-askers-own-book

#include <zen/weave/shape.hpp>
#include <zen/weave/weave.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

/// One in-flight request: which forward (seq stamps its correlation), at which
/// target, for which asker/correlation.
struct RelayPending {
    std::int64_t seq = 0;
    std::int64_t target = 0;
    std::int64_t asker = 0;
    std::int64_t corr = 0;
    using ZenSelf = RelayPending;
    static constexpr const char* zen_name = "zen.RelayPending";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(seq), ZEN_FIELD(target), ZEN_FIELD(asker),
                               ZEN_FIELD(corr));
    }
};

/// The relay's bookkeeping: a sequence counter and the bounded pending list.
/// Embed it in (or use it as) the relaying weave's state.
struct RelayState {
    std::int64_t next_seq = 0;
    std::vector<RelayPending> pending;
    using ZenSelf = RelayState;
    static constexpr const char* zen_name = "zen.RelayState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(next_seq), ZEN_FIELD(pending)); }
};

/// Pending forwards are bounded; the oldest is shed when full, and its answer, if it comes, is
/// dropped as unsolicited.
inline constexpr std::size_t kMaxRelayPending = 64;

/// Forward `req` to `target` for an asker captured earlier, for a multi-stage orchestration
/// where the inbound Mail no longer describes the asker. Allocates the sequence number, so a
/// caller's own chain can draw from this counter without colliding with the relay.
template <class Req>
std::int64_t forward_for(Mail& mail, RelayState& s, std::int64_t target, const Req& req,
                         WeaveId asker, std::uint64_t corr) {
    if (!asker.valid()) {
        return 0;
    }
    const std::int64_t seq = ++s.next_seq;
    if (s.pending.size() >= kMaxRelayPending) {
        s.pending.erase(s.pending.begin());
    }
    s.pending.push_back(RelayPending{seq, target, static_cast<std::int64_t>(asker.value),
                                     static_cast<std::int64_t>(corr)});
    mail.send(WeaveId{static_cast<std::uint64_t>(target)}, req,
              static_cast<std::uint64_t>(seq));
    return seq;
}

/// Forward `req` to `target`, remembering who asked: reply_to if given, else the bus-stamped
/// sender, never the payload. A command with no asker forwards nothing.
template <class Req>
void forward(Mail& mail, RelayState& s, std::int64_t target, const Req& req) {
    const WeaveId asker = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
    (void)forward_for(mail, s, target, req, asker, mail.correlation());
}

/// Relay `answer` to the asker of the forward it correlates to, restoring the asker's
/// correlation. It must come from the weave forwarded to, by bus-stamped sender, so a forged,
/// unsolicited or stale answer is dropped.
template <class Answer>
void relay(Mail& mail, RelayState& s, const Answer& answer) {
    for (std::size_t i = 0; i < s.pending.size(); ++i) {
        const RelayPending& p = s.pending[i];
        if (static_cast<std::uint64_t>(p.seq) != mail.correlation()) {
            continue;
        }
        if (static_cast<std::uint64_t>(p.target) != mail.sender().value) {
            continue;
        }
        const WeaveId asker{static_cast<std::uint64_t>(p.asker)};
        const std::uint64_t corr = static_cast<std::uint64_t>(p.corr);
        s.pending.erase(s.pending.begin() + static_cast<std::ptrdiff_t>(i));
        mail.send(asker, answer, corr);
        return;
    }
}

} // namespace loom

#endif // ZEN_WEAVE_RELAY_HPP
