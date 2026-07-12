#ifndef ZEN_WEAVE_POKE_HPP
#define ZEN_WEAVE_POKE_HPP

// The poke protocol: live inspect / manipulate, BY MESSAGE, enforced by the
// target's own construction layer.
//
// Every woven Weave (WeaveBase) answers four substrate doors —
//   zen.PokeDescribe    -> zen.PokeStructure   (the full structure + tag-state)
//   zen.PokeRead        -> zen.Result | zen.Refused
//   zen.PokeWrite       -> zen.Ack    | zen.Refused
//   zen.PokeResetState  -> zen.Ack    | zen.Refused
// — from its state shape's declared access model (ZEN_EXPOSE / ZEN_HIDE, see
// shape.hpp). A poke is an ordinary gated message; a poker is an ordinary
// participant; a weave that didn't expose something cannot be poked into it.
// That is the safety property, not a limitation.
//
// The replies are the STANDARD shapes (standard_shapes.hpp), not a poke
// dialect: an ack's correlation already says what was acked, a refusal's
// reason is written self-contained, a result's payload is the image. Only
// zen.PokeStructure stays bespoke — a weave's full structure is genuinely
// protocol-specific; remove its fields and the reader is confused, not merely
// less-informed.
//
// The two honesty properties this header enforces:
//   - NO SECRET STATE: zen.PokeDescribe lists EVERY field — name, type, and
//     tag-state — regardless of tags. ZEN_HIDE gates a value, never existence.
//   - NO SILENT FATE: every read/write/reset that is not performed is answered
//     with a zen.Refused carrying the reason.
//
// Values cross this boundary as text (`zen.Result.value`,
// `zen.PokeWrite.value`), converted against the FIELD'S OWN DECLARED KIND at
// the target — a bad literal is a clean refusal, never a mis-write. Scalar
// fields only this phase (Int/Float/Text/Bool); a non-scalar field is still
// fully visible in the structure, just not message-read/written yet.
//
// The "call a function directly with provided values" debugger power is real
// but deliberately NOT here: it arrives with the auth/identity phase as
// authority + inclusion ("having a, not being a"), never as a privileged
// debugger. Message-poking is *a* poke path, not *the only* one.

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
// Registration blocks are hand-written (not ZEN_SHAPE) so the wire names carry
// the substrate's "zen." prefix, which #ShapeName cannot produce. A maker's
// own struct named e.g. PokeRead derives "PokeRead" — no collision.

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

/// One field's structure entry: always present, tagged or not. `hidden` and
/// `writable` ARE the tag-state — hiding is itself declared, inspectable
/// metadata.
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

/// The answer to zen.PokeDescribe: the state shape's identity and EVERY field.
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

// The reply shapes zen.Result / zen.Ack / zen.Refused live in
// standard_shapes.hpp — they are the shared vocabulary, not a poke dialect.
// (The bespoke zen.PokeValue/PokeAck/PokeRefused this protocol first shipped
// with collapsed into them: their op/field members restated what the reply's
// correlation and the refusal's self-contained reason already carried.)

/// True for the four request shapes the construction layer itself answers.
/// WeaveBase refuses (at compile time) to let a maker Accept<> these: the
/// substrate answers them, and that non-interceptability is what makes an
/// answered structure trustworthy.
template <class T>
inline constexpr bool is_poke_protocol_shape =
    std::is_same_v<T, PokeDescribe> || std::is_same_v<T, PokeRead> ||
    std::is_same_v<T, PokeWrite> || std::is_same_v<T, PokeResetState>;

/// The four doors every woven Weave adds to its accept-set.
inline std::vector<std::shared_ptr<const Schema>> poke_door_schemas() {
    return {schema_of<PokeDescribe>(), schema_of<PokeRead>(), schema_of<PokeWrite>(),
            schema_of<PokeResetState>()};
}

/// The four answer shapes the construction layer emits when poked: the
/// bespoke structure plus the three standard replies.
inline std::vector<std::shared_ptr<const Schema>> poke_answer_schemas() {
    return {schema_of<PokeStructure>(), schema_of<Result>(), schema_of<Ack>(),
            schema_of<Refused>()};
}

/// Allow a Weave's poke ANSWERS. The construction layer does the answering,
/// but the send is gated like any other — the grant stays the host's sole
/// authority. mount() adds this for a trusted Weave; a host using
/// mount_granted decides for itself (an ungranted Weave's answers are
/// CapabilityDenied at delivery, visible on the tap).
inline Grant& allow_poke_answers(Grant& grant) {
    for (const auto& s : poke_answer_schemas()) {
        grant.allow_to_any(s->name(), s->version());
    }
    return grant;
}

// ---- value <-> text at the poke boundary ------------------------------------
// Locale-free, round-trip-exact (std::to_chars shortest form / std::from_chars
// full-match). The target parses against its own declared kind, so a bad
// literal is a clean refusal.

/// The kinds a poke can read/write this phase.
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
// These are the whole enforcement mechanism, bus-free and standalone-testable.
// WeaveBase::handle routes the gated request structs here and sends the answer.

/// The complete structure of a state shape: identity + EVERY field with its
/// tag-state. Nothing filters this — the no-secret-state floor.
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

// The refusal reasons are written self-contained (they name the field and
// what to do about it): with op/field folded into the prose, zen.Refused's one
// reason field carries the complete image on its own.

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
                      " — only scalar fields are message-readable this phase"};
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
                      " — only scalar fields are message-writable this phase"};
    }
    return true;
}

} // namespace detail

/// Read one field's raw value under the access model: any non-hidden scalar
/// field is readable (the default — no tag needed); a hidden field's value is
/// message-only and refused here.
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

/// Write one field under the access model: only a ZEN_EXPOSEd field is
/// manipulable; the text literal is parsed against the field's declared kind.
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

/// Restore the default-constructed state. Reset rewrites every field, so it
/// requires every field to be writable (ZEN_EXPOSEd) — the first unwritable
/// field names the refusal.
///
/// Reset has NO scalar-only guard, unlike poke_write, and that is deliberate,
/// not an oversight: it moves no value across the wire (it default-constructs
/// the whole state locally), so the text-transport limit that forces
/// poke_write to refuse a non-scalar field does not apply. The access decision
/// is identical — a non-scalar field is cleared only if the maker ZEN_EXPOSEd
/// it (opted the whole weave into manipulation); no un-exposed field is ever
/// touched. So an exposed std::vector is reset-clearable though not
/// poke_write-settable: same access model, a more capable transport.
template <Shape State>
std::variant<Ack, Refused> poke_reset(State& state) {
    for (const FieldAccess& f : access_of<State>()) {
        if (!f.writable) {
            // The blocking field is information the REQUEST does not carry
            // (zen.PokeResetState is fieldless), so the reason names it.
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
