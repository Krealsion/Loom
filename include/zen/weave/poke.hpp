// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_POKE_HPP
#define ZEN_WEAVE_POKE_HPP

// The poke protocol: inspect and change a weave's state live, by message, enforced by the
// target's own construction layer. Every woven Weave answers four substrate doors
//   zen.PokeDescribe    -> zen.PokeStructure   (every field, with its tags)
//   zen.PokeRead        -> zen.Result | zen.Refused
//   zen.PokeWrite       -> zen.Ack    | zen.Refused
//   zen.PokeResetState  -> zen.Ack    | zen.Refused
// from its state shape's access model (ZEN_EXPOSE / ZEN_HIDE, shape.hpp). A poke is an ordinary
// gated message from an ordinary participant, and cannot reach what a weave did not expose.
// docs/guides/diagnostics.md#5-live-inspection
//
// zen.PokeDescribe lists every field, whatever its tags: ZEN_HIDE gates a value, never its
// existence. Every request not performed is answered with a zen.Refused and its reason.
// Values cross as text, parsed against the field's declared kind at the target, so a bad
// literal is refused; only scalar fields (Int, Float, Text, Bool) are read and written this
// way, and every field still appears in the structure.

#include <zen/weave/shape.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <zen/switchboard/grant.hpp>

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace loom {

// ---- the protocol shapes ----------------------------------------------------
// Registered by hand so the wire names carry the "zen." prefix; a weaver's own struct named
// PokeRead is "PokeRead", with no collision.

/// Ask a weave for its structure: every field's name, type, and tag-state.
struct PokeDescribe {
    using ZenSelf = PokeDescribe;
    static constexpr const char* zen_name = "zen.PokeDescribe";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// Ask a weave for one field's raw value (refused if the field is hidden).
struct PokeRead {
    std::string field;
    using ZenSelf = PokeRead;
    static constexpr const char* zen_name = "zen.PokeRead";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(field)); }
};

/// Ask a weave to set one field (refused unless the field is ZEN_EXPOSEd).
/// `value` is the literal as text, parsed against the field's declared kind.
struct PokeWrite {
    std::string field;
    std::string value;
    using ZenSelf = PokeWrite;
    static constexpr const char* zen_name = "zen.PokeWrite";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(field), ZEN_FIELD(value)); }
};

/// Ask a weave to restore its default-constructed state. Reset rewrites every
/// field, so it is refused unless every field is writable.
struct PokeResetState {
    using ZenSelf = PokeResetState;
    static constexpr const char* zen_name = "zen.PokeResetState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// One field's structure entry, present whether tagged or not; `hidden` and `writable` are its
/// tags, which are themselves visible.
struct PokeFieldInfo {
    std::string name;
    std::string type;
    bool writable = false;
    bool hidden = false;
    using ZenSelf = PokeFieldInfo;
    static constexpr const char* zen_name = "zen.PokeField";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(type), ZEN_FIELD(writable),
                               ZEN_FIELD(hidden));
    }
};

/// The answer to zen.PokeDescribe: the state shape's identity and every field.
struct PokeStructure {
    std::string state_schema;
    std::int64_t state_version = 0;
    std::vector<PokeFieldInfo> fields;
    using ZenSelf = PokeStructure;
    static constexpr const char* zen_name = "zen.PokeStructure";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(state_schema), ZEN_FIELD(state_version),
                               ZEN_FIELD(fields));
    }
};

// The replies zen.Result, zen.Ack and zen.Refused are in standard_shapes.hpp.

/// True for the four request shapes the construction layer answers; WeaveBase refuses at
/// compile time to let a weaver Accept<> them, so an answered structure can be trusted.
template <class T>
inline constexpr bool is_poke_protocol_shape =
    std::is_same_v<T, PokeDescribe> || std::is_same_v<T, PokeRead> ||
    std::is_same_v<T, PokeWrite> || std::is_same_v<T, PokeResetState>;

/// The four doors every woven Weave adds to its accept-set.
inline std::vector<std::shared_ptr<const Schema>> poke_door_schemas() {
    return {schema_of<PokeDescribe>(), schema_of<PokeRead>(), schema_of<PokeWrite>(),
            schema_of<PokeResetState>()};
}

/// The four answer shapes: the structure and the three standard replies.
inline std::vector<std::shared_ptr<const Schema>> poke_answer_schemas() {
    return {schema_of<PokeStructure>(), schema_of<Result>(), schema_of<Ack>(),
            schema_of<Refused>()};
}

/// Allow a Weave's poke answers, which are gated like any send. mount() adds this for a
/// trusted Weave; under mount_granted an ungranted answer is `CapabilityDenied` at delivery.
inline Grant& allow_poke_answers(Grant& grant) {
    for (const auto& s : poke_answer_schemas()) {
        grant.allow_to_any(s->name(), s->version());
    }
    return grant;
}

// ---- value <-> text at the poke boundary ------------------------------------
// Locale-free and round-trip exact (std::to_chars shortest form, std::from_chars full match).

/// The kinds a poke can read and write: Int, Float, Text, Bool.
template <class M>
inline constexpr bool is_poke_scalar =
    std::is_same_v<M, std::int64_t> || std::is_same_v<M, double> ||
    std::is_same_v<M, std::string> || std::is_same_v<M, bool>;

inline std::string poke_render(std::int64_t v) {
    char buf[32];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}
inline std::string poke_render(double v) {
    char buf[64];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}
inline std::string poke_render(bool v) { return v ? "true" : "false"; }
inline std::string poke_render(const std::string& v) { return v; }

inline bool poke_parse(std::string_view text, std::int64_t& out) {
    const char* last = text.data() + text.size();
    const std::from_chars_result r = std::from_chars(text.data(), last, out);
    return r.ec == std::errc{} && r.ptr == last;
}
inline bool poke_parse(std::string_view text, double& out) {
    const char* last = text.data() + text.size();
    const std::from_chars_result r = std::from_chars(text.data(), last, out);
    return r.ec == std::errc{} && r.ptr == last;
}
inline bool poke_parse(std::string_view text, bool& out) {
    if (text == "true") {
        out = true;
        return true;
    }
    if (text == "false") {
        out = false;
        return true;
    }
    return false;
}
inline bool poke_parse(std::string_view text, std::string& out) {
    out.assign(text);
    return true;
}

/// Stable spelling of a field's type for structure/refusal messages.
inline std::string poke_type_name(const TypeRef& t) {
    switch (t.kind) {
    case Kind::Message:
        return t.message ? t.message->name() : "Message";
    case Kind::List:
        return t.element ? "List<" + poke_type_name(*t.element) + ">" : "List";
    default:
        return name_of(t.kind);
    }
}

// ---- the access model, enforced (pure functions over a state struct) --------
// The whole enforcement, testable without a bus; WeaveBase::handle routes requests here.

/// The complete structure of a state shape: its identity and every field with its tags.
/// Nothing filters it.
template <Shape State>
PokeStructure poke_structure() {
    PokeStructure out;
    const std::shared_ptr<const Schema> schema = schema_of<State>();
    out.state_schema = schema->name();
    out.state_version = static_cast<std::int64_t>(schema->version());
    for (const FieldAccess& f : access_of<State>()) {
        out.fields.push_back(PokeFieldInfo{f.name, poke_type_name(f.type), f.writable, f.hidden});
    }
    return out;
}

namespace detail {

// Refusal reasons are self-contained: they name the field and what to do.

template <class State, class C, class M>
bool poke_read_field(const State& state, const FieldEntry<C, M>& fe, std::uint8_t shape_bits,
                     std::string_view field, std::variant<Result, Refused>& out) {
    if (field != fe.name) {
        return false;
    }
    const std::uint8_t bits = static_cast<std::uint8_t>(fe.access | shape_bits);
    if ((bits & access::kHide) != 0) {
        out = Refused{"field '" + std::string(field) +
                      "' is hidden (ZEN_HIDE): its value is message-only — ask the weave "
                      "through its own interface"};
    } else if constexpr (is_poke_scalar<M>) {
        out = Result{poke_render(state.*(fe.ptr))};
    } else {
        out = Refused{"field '" + std::string(field) + "' has kind " +
                      poke_type_name(type_ref_for<M>::get()) +
                      " — only scalar fields are message-readable"};
    }
    return true;
}

template <class State, class C, class M>
bool poke_write_field(State& state, const FieldEntry<C, M>& fe, std::uint8_t shape_bits,
                      std::string_view field, std::string_view value,
                      std::variant<Ack, Refused>& out) {
    if (field != fe.name) {
        return false;
    }
    const std::uint8_t bits = static_cast<std::uint8_t>(fe.access | shape_bits);
    if ((bits & access::kExpose) == 0) {
        out = Refused{"field '" + std::string(field) +
                      "' is not exposed (ZEN_EXPOSE opts a field into manipulation)"};
    } else if constexpr (is_poke_scalar<M>) {
        M parsed{};
        if (poke_parse(value, parsed)) {
            state.*(fe.ptr) = std::move(parsed);
            out = Ack{};
        } else {
            out = Refused{"field '" + std::string(field) + "': value \"" + std::string(value) +
                          "\" does not parse as " + poke_type_name(type_ref_for<M>::get())};
        }
    } else {
        out = Refused{"field '" + std::string(field) + "' has kind " +
                      poke_type_name(type_ref_for<M>::get()) +
                      " — only scalar fields are message-writable"};
    }
    return true;
}

} // namespace detail

/// Read one field's value under the access model: any scalar field not hidden (the default);
/// a hidden field's value is refused.
template <Shape State>
std::variant<Result, Refused> poke_read(const State& state, std::string_view field) {
    std::variant<Result, Refused> result = Refused{
        "no field '" + std::string(field) + "' — zen.PokeDescribe lists the structure"};
    constexpr std::uint8_t shape_bits = shape_access_bits<State>();
    std::apply(
        [&](const auto&... fe) {
            (void)(detail::poke_read_field(state, fe, shape_bits, field, result) || ...);
        },
        State::zen_fields());
    return result;
}

/// Write one field under the access model: only a ZEN_EXPOSEd scalar field; the literal is
/// parsed against the field's declared kind.
template <Shape State>
std::variant<Ack, Refused> poke_write(State& state, std::string_view field,
                                      std::string_view value) {
    std::variant<Ack, Refused> result = Refused{
        "no field '" + std::string(field) + "' — zen.PokeDescribe lists the structure"};
    constexpr std::uint8_t shape_bits = shape_access_bits<State>();
    std::apply(
        [&](const auto&... fe) {
            (void)(detail::poke_write_field(state, fe, shape_bits, field, value, result) || ...);
        },
        State::zen_fields());
    return result;
}

/// Restore the default-constructed state. It rewrites every field, so it needs every field
/// writable (ZEN_EXPOSE); the first that is not names the refusal. Unlike poke_write it has no
/// scalar-only limit, since no value crosses as text: an exposed std::vector can be reset,
/// though not written.
template <Shape State>
std::variant<Ack, Refused> poke_reset(State& state) {
    for (const FieldAccess& f : access_of<State>()) {
        if (!f.writable) {
            // The request has no fields, so the reason names the blocking field.
            return Refused{"field '" + f.name +
                           "' is not exposed — reset rewrites every field, so it requires "
                           "a fully-exposed weave"};
        }
    }
    state = State{};
    return Ack{};
}

} // namespace loom

#endif // ZEN_WEAVE_POKE_HPP
