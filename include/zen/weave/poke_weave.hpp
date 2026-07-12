#ifndef ZEN_WEAVE_POKE_WEAVE_HPP
#define ZEN_WEAVE_POKE_WEAVE_HPP

// The Poke weave: the live-inspect/manipulate participant an operator drives.
//
// It is an ORDINARY woven Weave with no special powers — the most-granted
// participant, never an exception. It holds exactly the two kinds of thing any
// participant can hold: an accept-set (its command and answer doors) and a
// grant (mount<PokeWeave> derives it from the declared Emit<...> like any
// other trusted mount). It inspects and manipulates other weaves ONLY by
// sending them the poke protocol messages; enforcement lives in the TARGET's
// own construction layer (see poke.hpp) — the Poke weave cannot reach past
// any weave boundary, and a weave that didn't expose something cannot be
// poked into it.
//
// The operator (e.g. the console) sends it a command naming a target:
//   zen.PokeInspect{target}            -> forwards zen.PokeDescribe
//   zen.PokeGet{target, field}         -> forwards zen.PokeRead
//   zen.PokeSet{target, field, value}  -> forwards zen.PokeWrite
//   zen.PokeReset{target}              -> forwards zen.PokeResetState
// and the target's answer (zen.PokeStructure / zen.Result / zen.Ack /
// zen.Refused) is relayed back to the original asker with the original
// correlation. The forward/relay dance is the standard request/reply relay
// pattern (relay.hpp); this weave's whole state IS the relay bookkeeping —
// honest bounded state, itself poke-inspectable.
//
// A command whose forward is never answered (no such target, a raw non-woven
// Weave with no poke doors, or a target whose grant denies its answers)
// leaves a pending entry behind; the refusal itself is visible on the bus
// tap. Pending entries are bounded (kMaxRelayPending, oldest shed) — a
// participant cannot observe the fate of its own sends today; that
// observability is a named seam, not built here.

#include <zen/weave/poke.hpp>
#include <zen/weave/relay.hpp>
#include <zen/weave/weave.hpp>

#include <cstdint>
#include <string>
#include <tuple>

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

/// The debugger-as-participant. Mount it like any weave:
///   WeaveId poke = loom::mount<PokeWeave>(bus);
/// and drive it by message (the console composes the zen.Poke* commands).
/// Its state is the relay bookkeeping and nothing else.
class PokeWeave : public WeaveBase<PokeWeave, RelayState,
                                   Accept<PokeInspect, PokeGet, PokeSet, PokeReset, PokeStructure,
                                          Result, Ack, Refused>,
                                   Emit<PokeDescribe, PokeRead, PokeWrite, PokeResetState,
                                        PokeStructure, Result, Ack, Refused>> {
public:
    void on(const PokeInspect& c, Mail& mail) { forward(mail, state_, c.target, PokeDescribe{}); }
    void on(const PokeGet& c, Mail& mail) { forward(mail, state_, c.target, PokeRead{c.field}); }
    void on(const PokeSet& c, Mail& mail) {
        forward(mail, state_, c.target, PokeWrite{c.field, c.value});
    }
    void on(const PokeReset& c, Mail& mail) { forward(mail, state_, c.target, PokeResetState{}); }

    void on(const PokeStructure& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Result& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Ack& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Refused& a, Mail& mail) { relay(mail, state_, a); }
};

} // namespace loom

#endif // ZEN_WEAVE_POKE_WEAVE_HPP
