// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_POKE_HPP
#define ZEN_WEAVE_POKE_HPP

// The poke protocol: inspect and change a weave's state live, by message, enforced by the
// target's own construction layer. Every woven Weave answers five substrate doors
//   zen.PokeDescribe     -> zen.PokeStructure   (every field, with its tags)
//   zen.PokeRead v1      -> zen.Result   | zen.Refused   (one scalar field, by name)
//   zen.PokeRead v2      -> zen.PokeTree | zen.Refused   (a place by its path, every leaf below)
//   zen.PokeWrite        -> zen.Ack      | zen.Refused
//   zen.PokeResetState   -> zen.Ack      | zen.Refused
// from its state shape's access model (ZEN_EXPOSE / ZEN_HIDE, shape.hpp). A poke is an ordinary
// gated message from an ordinary participant, and cannot reach what a weave did not expose.
// docs/guides/diagnostics.md#5-live-inspection
//
// zen.PokeDescribe lists every field, whatever its tags: ZEN_HIDE gates a value, never its
// existence. Every request not performed is answered with a zen.Refused and its reason.
// Values cross as text, parsed against the field's declared kind at the target, so a bad
// literal is refused; version 1's read and the write take only scalar fields (Int, Float, Text,
// Bool), and every field still appears in the structure. Version 2's read reaches a list, a
// nested shape, a union and an enumeration too, and writes nothing (the nested read, below). An
// optional field (a std::optional member) is its kind's field: a read of one holding no value is
// refused as absent, a write gives it a value, and no literal makes it absent again
// (zen.PokeResetState restores the default-constructed state).

#include <zen/serialize.hpp>
#include <zen/weave/shape.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <zen/switchboard/grant.hpp>

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
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
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(); }
};

/// Ask a weave for one field's raw value (refused if the field is hidden).
struct PokeRead {
    std::string field;
    using ZenSelf = PokeRead;
    static constexpr const char* zen_name = "zen.PokeRead";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
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
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(field), ZEN_FIELD(value)); }
};

/// Ask a weave to restore its default-constructed state. Reset rewrites every
/// field, so it is refused unless every field is writable.
struct PokeResetState {
    using ZenSelf = PokeResetState;
    static constexpr const char* zen_name = "zen.PokeResetState";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
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
    template <int ZenLazy = 0>
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
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(state_schema), ZEN_FIELD(state_version),
                               ZEN_FIELD(fields));
    }
};

/// Ask a weave for the place `path` names inside its state, and every readable leaf below it:
/// zen.PokeRead at version 2, beside version 1's one scalar field by name. Each step is a
/// field's name, a list's index in decimal, or the name of the case a union holds; an optional
/// member that holds a value is stepped through as that value. A field's name never begins with
/// a digit, so a step is an index exactly when it is all digits. The empty path is the whole
/// state. The steps are a list rather than one spelling, so no step is escaped or parsed at the
/// target, and a case named by a shape's dotted name is one step.
struct PokeReadPath {
    std::vector<std::string> path;
    using ZenSelf = PokeReadPath;
    static constexpr const char* zen_name = "zen.PokeRead";
    static constexpr std::uint32_t zen_version = 2;
    template <int ZenLazy = 0>
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(path)); }
};

/// One leaf of a tree read: where it is (its path, spelled as poke_path_text spells one), its
/// kind as zen.PokeStructure spells a type, and its value as text, the literal version 1 reads.
/// A list's own entry carries its length as its value. `present` is false for an optional that
/// holds no value, whose `value` is then empty.
struct PokeEntry {
    std::string path;
    std::string type;
    std::string value;
    bool present = true;
    using ZenSelf = PokeEntry;
    static constexpr const char* zen_name = "zen.PokeEntry";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(path), ZEN_FIELD(type), ZEN_FIELD(value),
                               ZEN_FIELD(present));
    }
};

/// The answer to zen.PokeRead version 2: the place read and its kind, every readable leaf below
/// it in field order, and the path of each hidden field below it. Nothing of a withheld field's
/// value is said, and the answer is never partial: a read it cannot answer whole is refused.
struct PokeTree {
    std::string path;
    std::string type;
    std::vector<PokeEntry> entries;
    std::vector<std::string> withheld;
    using ZenSelf = PokeTree;
    static constexpr const char* zen_name = "zen.PokeTree";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(path), ZEN_FIELD(type), ZEN_FIELD(entries),
                               ZEN_FIELD(withheld));
    }
};

/// The most steps one path may take, a schema descriptor's own depth (`kMaxTypeDepth`), so no
/// state's place lies deeper than a path can reach.
inline constexpr std::size_t kMaxPokePathSteps = 64;

/// The most entries and withheld paths one tree read answers with together; past it the read is
/// refused, naming the count, and the reader reads a narrower path. An entry is five cells of
/// the answer and a withheld path one, so the largest answer stays inside the gate's decode
/// budget, `kMaxDecodedCells`, and no answer is refused `TooLarge` on its way to its reader.
inline constexpr std::size_t kMaxPokeTreeEntries = 8192;
static_assert(kMaxPokeTreeEntries * 5 + 16 <= kMaxDecodedCells,
              "loom: the largest tree read must cross the gate whole");

// The replies zen.Result, zen.Ack and zen.Refused are in standard_shapes.hpp.

/// True for the request shapes the construction layer answers; WeaveBase refuses at compile time
/// to let a weaver Accept<> them, so an answered structure can be trusted.
template <class T>
inline constexpr bool is_poke_protocol_shape =
    std::is_same_v<T, PokeDescribe> || std::is_same_v<T, PokeRead> ||
    std::is_same_v<T, PokeReadPath> || std::is_same_v<T, PokeWrite> ||
    std::is_same_v<T, PokeResetState>;

/// The five doors every woven Weave adds to its accept-set: the structure, the read at version
/// 1 and at version 2, the write and the reset.
inline std::vector<std::shared_ptr<const Schema>> poke_door_schemas() {
    return {schema_of<PokeDescribe>(), schema_of<PokeRead>(), schema_of<PokeReadPath>(),
            schema_of<PokeWrite>(), schema_of<PokeResetState>()};
}

/// The five answer shapes: the structure, the tree and the three standard replies.
inline std::vector<std::shared_ptr<const Schema>> poke_answer_schemas() {
    return {schema_of<PokeStructure>(), schema_of<PokeTree>(), schema_of<Result>(),
            schema_of<Ack>(), schema_of<Refused>()};
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
    case Kind::Union:
        return t.message ? t.message->name() : name_of(t.kind);
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
    using V = typename member_field<M>::type;
    if (field != fe.name) {
        return false;
    }
    const std::uint8_t bits = static_cast<std::uint8_t>(fe.access | shape_bits);
    if ((bits & access::kHide) != 0) {
        out = Refused{"field '" + std::string(field) +
                      "' is hidden (ZEN_HIDE): its value is message-only — ask the weave "
                      "through its own interface"};
    } else if constexpr (is_poke_scalar<V>) {
        if constexpr (member_field<M>::optional) {
            if ((state.*(fe.ptr)).has_value()) {
                out = Result{poke_render(*(state.*(fe.ptr)))};
            } else {
                out = Refused{"field '" + std::string(field) + "' is optional and absent"};
            }
        } else {
            out = Result{poke_render(state.*(fe.ptr))};
        }
    } else {
        out = Refused{"field '" + std::string(field) + "' has kind " +
                      poke_type_name(type_ref_for<V>::get()) +
                      " — only scalar fields are message-readable"};
    }
    return true;
}

template <class State, class C, class M>
bool poke_write_field(State& state, const FieldEntry<C, M>& fe, std::uint8_t shape_bits,
                      std::string_view field, std::string_view value,
                      std::variant<Ack, Refused>& out) {
    using V = typename member_field<M>::type;
    if (field != fe.name) {
        return false;
    }
    const std::uint8_t bits = static_cast<std::uint8_t>(fe.access | shape_bits);
    if ((bits & access::kExpose) == 0) {
        out = Refused{"field '" + std::string(field) +
                      "' is not exposed (ZEN_EXPOSE opts a field into manipulation)"};
    } else if constexpr (is_poke_scalar<V>) {
        V parsed{};
        if (poke_parse(value, parsed)) {
            state.*(fe.ptr) = std::move(parsed);
            out = Ack{};
        } else {
            out = Refused{"field '" + std::string(field) + "': value \"" + std::string(value) +
                          "\" does not parse as " + poke_type_name(type_ref_for<V>::get())};
        }
    } else {
        out = Refused{"field '" + std::string(field) + "' has kind " +
                      poke_type_name(type_ref_for<V>::get()) +
                      " — only scalar fields are message-writable"};
    }
    return true;
}

} // namespace detail

/// Read one field's value under the access model: any scalar field not hidden (the default);
/// a hidden field's value is refused, and so is an optional field holding no value, as absent.
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
/// parsed against the field's declared kind, and an optional field takes it as its value.
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

// ---- the nested read: a path into the state, and every readable leaf below it ------------
// zen.PokeRead version 2 (PokeReadPath) is answered by a zen.PokeTree: every leaf below the
// place its path names, each by its path, its kind and its value as text; a list's length as an
// entry of its own, before its elements; an absent optional as an entry not present; a union as
// the name of the case it holds, then that case's leaves under its name; an enumeration as its
// spelling; Bytes as lowercase hex; a loom::Any as its value's own JSON envelope, since its
// shape is its value's and not the state's. Each field ZEN_HIDE holds back is named in
// `withheld`, and nothing of its value is said. A path that names a hidden field or steps
// through one, steps into an absent optional, indexes past a list's end, or steps below a leaf
// is refused, naming the step. No nested write exists: version 1 of zen.PokeWrite still writes
// one exposed scalar field by name.

/// A path's steps as one spelling, `run.layouts[2].desk.name`: a step of digits is an index in
/// brackets, and every other step follows a dot.
inline std::string poke_path_text(const std::vector<std::string>& steps) {
    std::string out;
    for (const std::string& s : steps) {
        const bool index = !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
        if (index) {
            out += "[" + s + "]";
        } else {
            if (!out.empty()) {
                out += ".";
            }
            out += s;
        }
    }
    return out;
}

/// The steps a spelling names, the inverse of poke_path_text: `run.layouts[2].desk` is `run`,
/// `layouts`, `2`, `desk`, and the empty spelling is the whole state. It is the console's and
/// the Poke weave's, for a person who types a place; a step whose own name holds a dot or a
/// bracket, such as a case named by a dotted shape name, cannot be spelled, and is read with
/// PokeReadPath's steps instead. A spelling that names no steps cleanly (an empty step, a bracket
/// that does not hold an index, too many steps) is refused, saying where.
inline std::variant<std::vector<std::string>, Refused> poke_path_steps(std::string_view spelling) {
    std::vector<std::string> steps;
    std::size_t i = 0;
    const auto refused = [&](const std::string& why) {
        return Refused{"the path '" + std::string(spelling) + "' " + why};
    };
    while (i < spelling.size()) {
        if (spelling[i] == '[') {
            const std::size_t close = spelling.find(']', i);
            if (close == std::string_view::npos) {
                return refused("opens a bracket at " + std::to_string(i) + " and never closes it");
            }
            const std::string_view index = spelling.substr(i + 1, close - i - 1);
            if (index.empty() || index.find_first_not_of("0123456789") != std::string_view::npos) {
                return refused("holds '" + std::string(index) + "' in brackets at " +
                               std::to_string(i) + ", which is not an index");
            }
            steps.emplace_back(index);
            i = close + 1;
        } else {
            if (spelling[i] == '.') {
                if (steps.empty()) {
                    return refused("begins with a dot");
                }
                ++i;
            } else if (!steps.empty()) {
                return refused("has a step at " + std::to_string(i) +
                               " that follows no dot or bracket");
            }
            const std::size_t end = spelling.find_first_of(".[]", i);
            const std::string_view name =
                spelling.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
            if (name.empty()) {
                return refused("has an empty step at " + std::to_string(i));
            }
            steps.emplace_back(name);
            i += name.size();
            if (i < spelling.size() && spelling[i] == ']') {
                return refused("closes a bracket at " + std::to_string(i) + " it never opened");
            }
        }
        if (steps.size() > kMaxPokePathSteps) {
            return refused("takes more than " + std::to_string(kMaxPokePathSteps) +
                           " steps (kMaxPokePathSteps)");
        }
    }
    return steps;
}

namespace detail {

template <class V>
struct is_list : std::false_type {};
template <class E>
struct is_list<std::vector<E>> : std::true_type {};

struct TreeWalk {
    PokeTree tree;
    std::optional<std::string> refusal;
};

inline std::string child_path(const std::string& at, std::string_view field) {
    return at.empty() ? std::string(field) : at + "." + std::string(field);
}

/// Refuses the walk once its entries and withheld paths together would pass the bound, so the
/// answer is whole or refused, never cut short.
inline bool room_for_one_more(TreeWalk& w) {
    if (w.refusal) {
        return false;
    }
    if (w.tree.entries.size() + w.tree.withheld.size() >= kMaxPokeTreeEntries) {
        w.refusal = "the state at '" + w.tree.path + "' holds more than " +
                    std::to_string(kMaxPokeTreeEntries) +
                    " leaves (kMaxPokeTreeEntries): read a narrower path";
        return false;
    }
    return true;
}

inline void push_entry(TreeWalk& w, PokeEntry e) {
    if (room_for_one_more(w)) {
        w.tree.entries.push_back(std::move(e));
    }
}

inline void push_withheld(TreeWalk& w, std::string path) {
    if (room_for_one_more(w)) {
        w.tree.withheld.push_back(std::move(path));
    }
}

template <class V>
std::string kind_name() {
    return poke_type_name(type_ref_for<V>::get());
}

// The walks below step through a union's cases by recursion over the case's index, and through
// a record's fields by a fold over a function's calls, never by a fold over lambdas: GCC 11,
// Loom's oldest compiler, fails on a pack of lambdas expanded inside a template.

template <class V>
void emit_tree(const V& v, const std::string& at, TreeWalk& w);

template <class V>
void descend_tree(const V& v, const std::vector<std::string>& steps, std::size_t at_step,
                  const std::string& at, TreeWalk& w);

/// A member, as its registration says it: an optional holding nothing is an entry not present.
template <class M>
void emit_member(const M& m, const std::string& at, TreeWalk& w) {
    if constexpr (member_field<M>::optional) {
        using V = typename member_field<M>::type;
        if (m.has_value()) {
            emit_tree<V>(*m, at, w);
        } else {
            push_entry(w, PokeEntry{at, kind_name<V>(), "", false});
        }
    } else {
        emit_tree<M>(m, at, w);
    }
}

/// One field of a record: its leaves, or its path withheld when ZEN_HIDE holds it back. Always
/// false, so a fold over every field visits each.
template <class V, class FE>
bool emit_field(const V& v, const FE& fe, std::uint8_t shape_bits, const std::string& at,
                TreeWalk& w) {
    const std::string here = child_path(at, fe.name);
    if (((fe.access | shape_bits) & access::kHide) != 0) {
        push_withheld(w, here);
    } else {
        emit_member(v.*(fe.ptr), here, w);
    }
    return false;
}

/// The case a union holds, as its entry, then what that case carries under its name.
template <class V, std::size_t I = 0>
void emit_union(const V& v, const std::string& at, TreeWalk& w) {
    using Cases = union_variant_t<V>;
    if constexpr (I < std::variant_size_v<Cases>) {
        const auto& held = v.*(V::zen_union());
        if (held.index() != I) {
            emit_union<V, I + 1>(v, at, w);
            return;
        }
        push_entry(w, PokeEntry{at, kind_name<V>(), case_name<V, I>(), true});
        using A = std::variant_alternative_t<I, Cases>;
        if constexpr (!std::is_same_v<A, std::monostate>) {
            emit_tree<A>(std::get<I>(held), child_path(at, case_name<V, I>()), w);
        }
    }
}

template <class V>
void emit_tree(const V& v, const std::string& at, TreeWalk& w) {
    if (w.refusal) {
        return;
    }
    if constexpr (is_poke_scalar<V>) {
        push_entry(w, PokeEntry{at, kind_name<V>(), poke_render(v), true});
    } else if constexpr (std::is_same_v<V, Bytes>) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(v.size() * 2);
        for (const std::uint8_t b : v) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 0xF]);
        }
        push_entry(w, PokeEntry{at, kind_name<V>(), std::move(hex), true});
    } else if constexpr (is_list<V>::value) {
        push_entry(w, PokeEntry{at, kind_name<V>(), std::to_string(v.size()), true});
        for (std::size_t i = 0; i < v.size() && !w.refusal; ++i) {
            emit_tree(v[i], at + "[" + std::to_string(i) + "]", w);
        }
    } else if constexpr (Enumeration<V>) {
        const auto info = zen_enum_info(static_cast<V*>(nullptr));
        const std::size_t i = enum_index(v);
        push_entry(w, PokeEntry{at, kind_name<V>(),
                                i < info.spellings.size() ? std::string(info.spellings[i]) : "",
                                true});
    } else if constexpr (UnionShape<V>) {
        emit_union<V>(v, at, w);
    } else if constexpr (RecordShape<V>) {
        constexpr std::uint8_t shape_bits = shape_access_bits<V>();
        std::apply(
            [&](const auto&... fe) { (void)(emit_field(v, fe, shape_bits, at, w) || ...); },
            V::zen_fields());
    } else {
        // loom::Any: its value's shape is its own, not this state's; said as its JSON envelope.
        push_entry(w, PokeEntry{at, kind_name<V>(),
                                v.has_value() ? compat::serialize(*v.value()) : "", v.has_value()});
    }
}

inline bool poke_index(std::string_view step, std::size_t& out) {
    if (step.empty() || step.find_first_not_of("0123456789") != std::string_view::npos) {
        return false;
    }
    const std::from_chars_result r = std::from_chars(step.data(), step.data() + step.size(), out);
    return r.ec == std::errc{} && r.ptr == step.data() + step.size();
}

template <class M>
void descend_member(const M& m, const std::vector<std::string>& steps, std::size_t at_step,
                    const std::string& at, TreeWalk& w) {
    if constexpr (member_field<M>::optional) {
        if (!m.has_value()) {
            w.refusal = "'" + at + "' is optional and absent";
            return;
        }
        descend_tree<typename member_field<M>::type>(*m, steps, at_step, at, w);
    } else {
        descend_tree<M>(m, steps, at_step, at, w);
    }
}

/// The field a step names, stepped into, or refused when ZEN_HIDE holds it back. True once the
/// step has named a field, so the fold stops there.
template <class V, class FE>
bool descend_field(const V& v, const FE& fe, std::uint8_t shape_bits,
                   const std::vector<std::string>& steps, std::size_t at_step,
                   const std::string& at, TreeWalk& w) {
    if (steps[at_step] != fe.name) {
        return false;
    }
    const std::string here = child_path(at, fe.name);
    if (((fe.access | shape_bits) & access::kHide) != 0) {
        w.refusal = "field '" + here +
                    "' is hidden (ZEN_HIDE): its value is message-only — ask the weave through "
                    "its own interface";
    } else {
        descend_member(v.*(fe.ptr), steps, at_step + 1, here, w);
    }
    return true;
}

/// The case a step names, stepped into when the union holds it, else refused.
template <class V, std::size_t I = 0>
void descend_union(const V& v, const std::vector<std::string>& steps, std::size_t at_step,
                   const std::string& at, TreeWalk& w) {
    using Cases = union_variant_t<V>;
    const std::string& step = steps[at_step];
    if constexpr (I == std::variant_size_v<Cases>) {
        w.refusal = "'" + at + "' has no case '" + step + "'";
    } else {
        if (step != case_name<V, I>()) {
            descend_union<V, I + 1>(v, steps, at_step, at, w);
            return;
        }
        using A = std::variant_alternative_t<I, Cases>;
        const auto& held = v.*(V::zen_union());
        if (held.index() != I) {
            w.refusal = "'" + at + "' holds another case than '" + step + "'";
        } else if constexpr (std::is_same_v<A, std::monostate>) {
            w.refusal = "'" + child_path(at, step) + "' carries nothing below it";
        } else {
            descend_tree<A>(std::get<I>(held), steps, at_step + 1, child_path(at, step), w);
        }
    }
}

template <class V>
void descend_tree(const V& v, const std::vector<std::string>& steps, std::size_t at_step,
                  const std::string& at, TreeWalk& w) {
    if (at_step == steps.size()) {
        w.tree.type = kind_name<V>();
        emit_tree(v, at, w);
        return;
    }
    const std::string& step = steps[at_step];
    if constexpr (is_list<V>::value && !std::is_same_v<V, Bytes>) {
        std::size_t i = 0;
        if (!poke_index(step, i)) {
            w.refusal = "'" + at + "' is a list: step '" + step + "' is not an index";
        } else if (i >= v.size()) {
            w.refusal = "'" + at + "' holds " + std::to_string(v.size()) + " elements: index " +
                        step + " is past its end";
        } else {
            descend_tree(v[i], steps, at_step + 1, at + "[" + step + "]", w);
        }
    } else if constexpr (UnionShape<V>) {
        descend_union<V>(v, steps, at_step, at, w);
    } else if constexpr (RecordShape<V>) {
        constexpr std::uint8_t shape_bits = shape_access_bits<V>();
        const bool found = std::apply(
            [&](const auto&... fe) {
                return (descend_field(v, fe, shape_bits, steps, at_step, at, w) || ...);
            },
            V::zen_fields());
        if (!found) {
            w.refusal = "no field '" + step + "' in '" + (at.empty() ? "the state" : at) +
                        "' — zen.PokeDescribe lists the structure";
        }
    } else {
        w.refusal = "'" + at + "' is " + kind_name<V>() + ": nothing is below it";
    }
}

} // namespace detail

/// Read the state at `path`, and every readable leaf below it, under the access model: a hidden
/// field is never said, at the path or below it, and no answer passes kMaxPokeTreeEntries.
template <Shape State>
std::variant<PokeTree, Refused> poke_read_tree(const State& state,
                                               const std::vector<std::string>& path) {
    if (path.size() > kMaxPokePathSteps) {
        return Refused{"a path of " + std::to_string(path.size()) + " steps: the most is " +
                       std::to_string(kMaxPokePathSteps) + " (kMaxPokePathSteps)"};
    }
    detail::TreeWalk w;
    w.tree.path = poke_path_text(path);
    detail::descend_tree(state, path, 0, "", w);
    if (w.refusal) {
        return Refused{*w.refusal};
    }
    return std::move(w.tree);
}

} // namespace loom

#endif // ZEN_WEAVE_POKE_HPP
