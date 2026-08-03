// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_RELAY_HPP
#define ZEN_WEAVE_RELAY_HPP

// The request/reply relay pattern, expressed once: "forward this request to a
// named target, and relay its answer back to whoever asked me — matched by
// correlation." Any weave that fronts a protocol for an operator (the Poke
// weave is the living consumer) repeats the same dance: stamp a fresh sequence
// number on the forward, remember who asked under it, and when an answer
// carrying that sequence number comes back FROM THE FORWARDED-TO TARGET, send
// it on with the asker's original correlation.
//
// The helper carries the least the pattern needs — the correlation bookkeeping
// (RelayState) and the two moves (forward / relay) — and nothing more: no
// expected-reply-type registry (the target answers what it answers; a refusal
// can answer any request), no timeouts (send-fate observability is a named
// seam, not built), no knobs. The maker's Accept<...>/Emit<...> declarations
// stay where they were, fully visible: the helper removes bookkeeping, never
// the contract.
//
// RelayState is honest weave state: an ordinary shape that snapshots, revives,
// and is itself poke-inspectable (default access: readable, not writable).

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

/// Pending forwards are bounded; the oldest is shed when full (its answer, if
/// it ever comes, is then treated as unsolicited and dropped).
inline constexpr std::size_t kMaxRelayPending = 64;

/// Forward `req` to `target` on behalf of an asker captured EARLIER. The
/// explicit form, for a multi-stage orchestration: when the answer that will
/// finally satisfy the asker is triggered by some *other* weave's message, the
/// inbound Mail no longer describes the asker, so the caller supplies the asker
/// and correlation it recorded when the request first arrived. The
/// routing-metadata-not-payload discipline is unchanged — it simply moves to
/// wherever the caller first captured them.
///
/// Also allocates the sequence number, so a caller running a chain of its own
/// can draw correlations from this one counter and never collide with a relay.
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

/// Forward `req` to `target`, remembering who asked so the answer can be
/// relayed back. The asker is taken from the inbound command's routing
/// metadata (reply_to if given, else the bus-stamped sender) — never from its
/// payload. A fire-and-forget command (no asker) forwards nothing: there is
/// no one to answer.
template <class Req>
void forward(Mail& mail, RelayState& s, std::int64_t target, const Req& req) {
    const WeaveId asker = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
    (void)forward_for(mail, s, target, req, asker, mail.correlation());
}

/// Relay `answer` to the asker of the pending forward it correlates to,
/// restoring the asker's original correlation. The answer must come from the
/// weave that was actually forwarded to: the sender is bus-stamped (a
/// participant cannot claim another's identity), so a third party can emit an
/// answer-shaped message but cannot speak AS the target — a forged or
/// unsolicited or stale answer is dropped, not relayed.
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
