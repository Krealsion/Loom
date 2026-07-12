#ifndef ZEN_WEAVE_POKE_WEAVE_HPP
#define ZEN_WEAVE_POKE_WEAVE_HPP

// The Poke weave: the live-inspect/manipulate participant an operator drives.
//
// It is an ORDINARY woven Weave with no special powers — the most-granted
// participant, never an exception. It holds exactly the two kinds of thing any
// participant can hold: an accept-set (its command and answer doors) and a
// grant (mount<PokeWeave> derives it from the declared Emit<...> like any
// other trusted mount). It inspects and manipulates other weaves ONLY by
// sending them the zen.Poke* protocol messages; enforcement lives in the
// TARGET's own construction layer (see poke.hpp) — the Poke weave cannot
// reach past any weave boundary, and a weave that didn't expose something
// cannot be poked into it.
//
// The operator (e.g. the console) sends it a command naming a target:
//   zen.PokeInspect{target}            -> forwards zen.PokeDescribe
//   zen.PokeGet{target, field}         -> forwards zen.PokeRead
//   zen.PokeSet{target, field, value}  -> forwards zen.PokeWrite
//   zen.PokeReset{target}              -> forwards zen.PokeResetState
// and the target's answer (zen.PokeStructure/PokeValue/PokeAck/PokeRefused)
// is relayed back to the original asker with the original correlation.
//
// A command whose forward is never answered (no such target, a raw
// non-woven Weave with no poke doors, or a target whose grant denies its
// answers) leaves a pending entry behind; the refusal itself is visible on
// the bus tap. Pending entries are bounded (oldest shed) — a participant
// cannot observe the fate of its own sends today; that observability is a
// named seam, not built here.

#include <zen/weave/poke.hpp>
#include <zen/weave/weave.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

// ---- the operator command shapes --------------------------------------------
// Distinct from the protocol shapes: a command NAMES a third-party target; a
// protocol message arriving AT a weave always means "you".

/// Inspect a weave's structure (every field + tag-state).
struct PokeInspect {
    std::int64_t target = 0;
    using ZenSelf = PokeInspect;
    static constexpr const char* zen_name = "zen.PokeInspect";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target)); }
};

/// Read one field's raw value from a weave.
struct PokeGet {
    std::int64_t target = 0;
    std::string field;
    using ZenSelf = PokeGet;
    static constexpr const char* zen_name = "zen.PokeGet";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target), ZEN_FIELD(field)); }
};

/// Set one field on a weave (the value is a text literal, parsed by the
/// target against the field's declared kind).
struct PokeSet {
    std::int64_t target = 0;
    std::string field;
    std::string value;
    using ZenSelf = PokeSet;
    static constexpr const char* zen_name = "zen.PokeSet";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(target), ZEN_FIELD(field), ZEN_FIELD(value));
    }
};

/// Reset a weave to its default-constructed state.
struct PokeReset {
    std::int64_t target = 0;
    using ZenSelf = PokeReset;
    static constexpr const char* zen_name = "zen.PokeReset";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target)); }
};

// ---- the Poke weave's own state ----------------------------------------------

/// One in-flight poke: which request (seq stamps the forward's correlation),
/// at which target, for which asker/correlation. Honest state: it snapshots
/// and revives like any weave state — and, being state, it is itself
/// poke-inspectable (default access: readable, not writable).
struct PokePendingEntry {
    std::int64_t seq = 0;
    std::int64_t target = 0;
    std::int64_t asker = 0;
    std::int64_t corr = 0;
    using ZenSelf = PokePendingEntry;
    static constexpr const char* zen_name = "zen.PokePending";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(seq), ZEN_FIELD(target), ZEN_FIELD(asker),
                               ZEN_FIELD(corr));
    }
};

struct PokeWeaveState {
    std::int64_t next_seq = 0;
    std::vector<PokePendingEntry> pending;
    using ZenSelf = PokeWeaveState;
    static constexpr const char* zen_name = "zen.PokeWeaveState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(next_seq), ZEN_FIELD(pending)); }
};

/// The debugger-as-participant. Mount it like any weave:
///   WeaveId poke = loom::mount<PokeWeave>(bus);
/// and drive it by message (the console composes the zen.Poke* commands).
class PokeWeave : public WeaveBase<PokeWeave, PokeWeaveState,
                                   Accept<PokeInspect, PokeGet, PokeSet, PokeReset, PokeStructure,
                                          PokeValue, PokeAck, PokeRefused>,
                                   Emit<PokeDescribe, PokeRead, PokeWrite, PokeResetState,
                                        PokeStructure, PokeValue, PokeAck, PokeRefused>> {
public:
    /// Pending pokes are bounded; the oldest is shed when full (its answer,
    /// if it ever comes, is then treated as unsolicited and dropped).
    static constexpr std::size_t kMaxPending = 64;

    void on(const PokeInspect& c, Mail& mail) { forward(mail, c.target, PokeDescribe{}); }
    void on(const PokeGet& c, Mail& mail) { forward(mail, c.target, PokeRead{c.field}); }
    void on(const PokeSet& c, Mail& mail) { forward(mail, c.target, PokeWrite{c.field, c.value}); }
    void on(const PokeReset& c, Mail& mail) { forward(mail, c.target, PokeResetState{}); }

    void on(const PokeStructure& a, Mail& mail) { relay(mail, a); }
    void on(const PokeValue& a, Mail& mail) { relay(mail, a); }
    void on(const PokeAck& a, Mail& mail) { relay(mail, a); }
    void on(const PokeRefused& a, Mail& mail) { relay(mail, a); }

private:
    template <class Req>
    void forward(Mail& mail, std::int64_t target, const Req& req) {
        // The asker is taken from the command's routing metadata (reply_to if
        // given, else the bus-stamped sender) — never from its payload.
        const WeaveId asker = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
        if (!asker.valid()) {
            return; // a fire-and-forget command has no one to answer
        }
        const std::int64_t seq = ++state_.next_seq;
        if (state_.pending.size() >= kMaxPending) {
            state_.pending.erase(state_.pending.begin());
        }
        state_.pending.push_back(PokePendingEntry{seq, target,
                                                  static_cast<std::int64_t>(asker.value),
                                                  static_cast<std::int64_t>(mail.correlation())});
        mail.send(WeaveId{static_cast<std::uint64_t>(target)}, req,
                  static_cast<std::uint64_t>(seq));
    }

    template <class Answer>
    void relay(Mail& mail, const Answer& answer) {
        for (std::size_t i = 0; i < state_.pending.size(); ++i) {
            const PokePendingEntry& p = state_.pending[i];
            if (static_cast<std::uint64_t>(p.seq) != mail.correlation()) {
                continue;
            }
            // The answer must come from the weave that was actually poked.
            // The sender is bus-stamped (a participant cannot claim another's
            // identity), so a third party cannot speak for the target.
            if (static_cast<std::uint64_t>(p.target) != mail.sender().value) {
                continue;
            }
            const WeaveId asker{static_cast<std::uint64_t>(p.asker)};
            const std::uint64_t corr = static_cast<std::uint64_t>(p.corr);
            state_.pending.erase(state_.pending.begin() + static_cast<std::ptrdiff_t>(i));
            mail.send(asker, answer, corr);
            return;
        }
        // No matching pending poke: unsolicited or stale — not relayed.
    }
};

} // namespace loom

#endif // ZEN_WEAVE_POKE_WEAVE_HPP
