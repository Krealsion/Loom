// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_POKE_WEAVE_HPP
#define ZEN_WEAVE_POKE_WEAVE_HPP

// The Poke weave: the participant a weaver drives to inspect and change other weaves live.
// An ordinary woven Weave with no special powers: an accept-set, and the grant mount<PokeWeave>
// derives from its Emit<...>. It reaches other weaves only by sending them the poke protocol,
// which each target's construction layer enforces (poke.hpp), so it cannot reach past what a
// weave exposed. docs/guides/diagnostics.md#5-live-inspection
//
//   zen.PokeInspect{target}            -> forwards zen.PokeDescribe
//   zen.PokeGet{target, field}         -> forwards zen.PokeRead version 1
//   zen.PokeGetTree{target, path}      -> forwards zen.PokeRead version 2, the path's steps
//   zen.PokeSet{target, field, value}  -> forwards zen.PokeWrite
//   zen.PokeReset{target}              -> forwards zen.PokeResetState
//
// The target's answer is relayed to the asker with its own correlation (relay.hpp). A forward
// never answered (no such target, a raw Weave with no poke doors, a target not granted its
// answers) stays pending, bounded by kMaxRelayPending, oldest shed; the refusal is on the bus
// tap, and this weave does not accept dispatch-refusal notices.

#include <zen/weave/poke.hpp>
#include <zen/weave/relay.hpp>
#include <zen/weave/weave.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace loom {

// ---- the weaver command shapes --------------------------------------------
// A command names a third-party target; a protocol message arriving at a weave means "you".

/// Inspect a weave's structure: every field, with its tags.
struct PokeInspect {
    std::int64_t target = 0;
    using ZenSelf = PokeInspect;
    static constexpr const char* zen_name = "zen.PokeInspect";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target)); }
};

/// Read one field's raw value from a weave.
struct PokeGet {
    std::int64_t target = 0;
    std::string field;
    using ZenSelf = PokeGet;
    static constexpr const char* zen_name = "zen.PokeGet";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target), ZEN_FIELD(field)); }
};

/// Read the place a path names inside a weave's state, and every readable leaf below it. `path`
/// is the spelling a person types, `run.layouts[0].desk` (poke_path_steps), and the empty path
/// is the whole state; a spelling that names no steps cleanly is refused here, by this weave,
/// and nothing is forwarded. The answer is the target's zen.PokeTree, relayed.
struct PokeGetTree {
    std::int64_t target = 0;
    std::string path;
    using ZenSelf = PokeGetTree;
    static constexpr const char* zen_name = "zen.PokeGetTree";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target), ZEN_FIELD(path)); }
};

/// Set one field on a weave; the value is a text literal the target parses against the field's
/// declared kind.
struct PokeSet {
    std::int64_t target = 0;
    std::string field;
    std::string value;
    using ZenSelf = PokeSet;
    static constexpr const char* zen_name = "zen.PokeSet";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
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
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(target)); }
};

/// The poke participant. Mount it like any weave, `loom::mount<PokeWeave>(bus)`, and drive it by
/// message; its state is the relay bookkeeping and nothing else.
class PokeWeave : public WeaveBase<PokeWeave, RelayState,
                                   Accept<PokeInspect, PokeGet, PokeGetTree, PokeSet, PokeReset,
                                          PokeStructure, PokeTree, Result, Ack, Refused>,
                                   Emit<PokeDescribe, PokeRead, PokeReadPath, PokeWrite,
                                        PokeResetState, PokeStructure, PokeTree, Result, Ack,
                                        Refused>> {
public:
    void on(const PokeInspect& c, Mail& mail) { forward(mail, state_, c.target, PokeDescribe{}); }
    void on(const PokeGet& c, Mail& mail) { forward(mail, state_, c.target, PokeRead{c.field}); }
    void on(const PokeGetTree& c, Mail& mail) {
        std::variant<std::vector<std::string>, Refused> steps = poke_path_steps(c.path);
        if (const Refused* refused = std::get_if<Refused>(&steps)) {
            // Told to whoever a forward would have answered, as the target's refusal would be.
            const WeaveId asker = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
            if (!asker.valid()) {
                return;
            }
            if (asker == mail.sender()) {
                (void)mail.answer(*refused);
            } else {
                mail.send(asker, *refused, mail.correlation());
            }
            return;
        }
        forward(mail, state_, c.target,
                PokeReadPath{std::move(std::get<std::vector<std::string>>(steps))});
    }
    void on(const PokeSet& c, Mail& mail) {
        forward(mail, state_, c.target, PokeWrite{c.field, c.value});
    }
    void on(const PokeReset& c, Mail& mail) { forward(mail, state_, c.target, PokeResetState{}); }

    void on(const PokeStructure& a, Mail& mail) { relay(mail, state_, a); }
    void on(const PokeTree& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Result& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Ack& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Refused& a, Mail& mail) { relay(mail, state_, a); }
};

} // namespace loom

#endif // ZEN_WEAVE_POKE_WEAVE_HPP
