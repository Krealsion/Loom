// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_SHAPE_HPP
#define ZEN_WEAVE_SHAPE_HPP

// Schema-from-struct: write a shape once as a plain C++ struct and derive the rest. It adds no
// schema type, value type or validator: schemas are built through SchemaBuilder (so a derived
// schema has the same content id as the hand-built one), and struct and Value convert at the
// edges, the Value going to the same admit.
//
//   struct Ping {
//       std::int64_t seq;
//       ZEN_SHAPE(Ping, /*version=*/1, ZEN_FIELD(seq));
//   };
//
// The ZEN_FIELD list names each member once more, to capture its name as a string; that
// repetition is confined to zen_fields(). docs/guides/writing-a-weave.md

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace loom {

/// A type carrying a ZEN_SHAPE registration.
template <class T>
concept Shape = requires {
    { T::zen_version } -> std::convertible_to<std::uint32_t>;
    { T::zen_name } -> std::convertible_to<const char*>;
    T::zen_fields();
};

// Forward declarations break the mutual recursion among the derivations below
// (a Message field's type/conversion refers back to these for the nested shape).
template <class T>
std::shared_ptr<const loom::Schema> schema_of();
template <class T>
loom::Value to_value(const T& obj);
template <Shape T>
T from_value(const loom::Value& v);

/// The two access-tag bits a field registration may carry: metadata beside the shape, never
/// part of it.
namespace access {
inline constexpr std::uint8_t kNone = 0;
inline constexpr std::uint8_t kExpose = 1; ///< opt IN to write (manipulation)
inline constexpr std::uint8_t kHide = 2;   ///< opt OUT of raw read (value is message-only)
} // namespace access

/// One registered field: its wire name, a pointer to the member, and its access tags. The tags
/// are invisible to build_schema(), so a tagged struct has the same content id as an untagged
/// one: they govern access to live state, not wire identity.
template <class C, class M>
struct FieldEntry {
    const char* name;
    M C::*ptr;
    std::uint8_t access = access::kNone;
    using member_type = M;
    using class_type = C;
};

template <class C, class M>
constexpr FieldEntry<C, M> field_entry(const char* name, M C::*ptr,
                                       std::uint8_t access_bits = access::kNone) {
    return FieldEntry<C, M>{name, ptr, access_bits};
}

// ---- Kind deduced from the C++ member type --------------------------------

template <class M>
struct type_ref_for {
    static loom::TypeRef get() {
        static_assert(Shape<M>,
                      "loom: unsupported field type; use a supported scalar "
                      "(std::int64_t/double/std::string/bool), loom::Bytes, std::vector<T>, "
                      "or a registered ZEN_SHAPE struct");
        return loom::type_message(schema_of<M>());
    }
};
template <>
struct type_ref_for<std::int64_t> {
    static loom::TypeRef get() { return loom::type_of(loom::Kind::Int); }
};
template <>
struct type_ref_for<double> {
    static loom::TypeRef get() { return loom::type_of(loom::Kind::Float); }
};
template <>
struct type_ref_for<std::string> {
    static loom::TypeRef get() { return loom::type_of(loom::Kind::Text); }
};
template <>
struct type_ref_for<bool> {
    static loom::TypeRef get() { return loom::type_of(loom::Kind::Bool); }
};
// loom::Bytes is std::vector<std::uint8_t>; this full specialization wins over the
// std::vector<T> partial below, so a byte vector is Bytes, not List<Int>.
template <>
struct type_ref_for<loom::Bytes> {
    static loom::TypeRef get() { return loom::type_of(loom::Kind::Bytes); }
};
template <class T>
struct type_ref_for<std::vector<T>> {
    static loom::TypeRef get() { return loom::type_list(type_ref_for<T>::get()); }
};

// ---- struct member <-> Cell -----------------------------------------------

// Forward declarations of the recursive (List / Message) overloads, so a List of
// Messages resolves regardless of definition order (the shapes may live in a
// namespace ADL would not reach).
template <class T>
loom::Cell to_cell(const std::vector<T>& v);
template <Shape U>
loom::Cell to_cell(const U& u);
template <class T>
void from_cell(std::vector<T>& d, const loom::Cell& c);
template <Shape U>
void from_cell(U& d, const loom::Cell& c);

inline loom::Cell to_cell(std::int64_t v) { return loom::Cell::integer(v); }
inline loom::Cell to_cell(double v) { return loom::Cell::real(v); }
inline loom::Cell to_cell(const std::string& v) { return loom::Cell::text(v); }
inline loom::Cell to_cell(bool v) { return loom::Cell::boolean(v); }
inline loom::Cell to_cell(const loom::Bytes& v) { return loom::Cell::bytes(v); }
template <class T>
loom::Cell to_cell(const std::vector<T>& v) {
    loom::Cell::Array arr;
    arr.reserve(v.size());
    for (const auto& e : v) {
        arr.push_back(to_cell(e));
    }
    return loom::Cell::list(std::move(arr));
}
template <Shape U>
loom::Cell to_cell(const U& u) {
    return loom::Cell::message(to_value(u));
}

inline void from_cell(std::int64_t& d, const loom::Cell& c) { d = c.as_int(); }
inline void from_cell(double& d, const loom::Cell& c) { d = c.as_float(); }
inline void from_cell(std::string& d, const loom::Cell& c) { d = c.as_text(); }
inline void from_cell(bool& d, const loom::Cell& c) { d = c.as_bool(); }
inline void from_cell(loom::Bytes& d, const loom::Cell& c) { d = c.as_bytes(); }
template <class T>
void from_cell(std::vector<T>& d, const loom::Cell& c) {
    const loom::Cell::Array& arr = c.as_list();
    d.clear();
    d.reserve(arr.size());
    for (const loom::Cell& e : arr) {
        T tmp{};
        from_cell(tmp, e);
        d.push_back(std::move(tmp));
    }
}
template <Shape U>
void from_cell(U& d, const loom::Cell& c) {
    d = from_value<U>(*c.as_message());
}

// ---- the derivations -------------------------------------------------------

template <class T>
std::shared_ptr<const loom::Schema> build_schema() {
    static_assert(Shape<T>, "loom: type is not a ZEN_SHAPE (no zen_fields/zen_version)");
    loom::SchemaBuilder builder(T::zen_name, T::zen_version);
    std::apply(
        [&](auto&&... fe) {
            (builder.add(loom::Field{fe.name,
                                    type_ref_for<typename std::decay_t<decltype(fe)>::member_type>::get(),
                                    /*required=*/true}),
             ...);
        },
        T::zen_fields());
    return builder.build();
}

/// The canonical schema for a shape, built once. Two structurally-identical
/// shapes (struct-derived or hand-built) share this content-id and door.
template <class T>
std::shared_ptr<const loom::Schema> schema_of() {
    static const std::shared_ptr<const loom::Schema> s = build_schema<T>();
    return s;
}

/// Convert a struct to a Value claiming its derived schema. Adds no validation;
/// the caller hands the Value to the same admit.
template <class T>
loom::Value to_value(const T& obj) {
    static_assert(Shape<T>, "loom: to_value requires a ZEN_SHAPE struct");
    loom::Value v(schema_of<T>());
    std::apply([&](auto&&... fe) { (v.set(fe.name, to_cell(obj.*(fe.ptr))), ...); }, T::zen_fields());
    return v;
}

/// Convert an already-gated Value to its struct. Precondition: `v` has passed
/// the gate against schema_of<T>() (every required field present and well-typed).
template <Shape T>
T from_value(const loom::Value& v) {
    T obj{};
    std::apply([&](auto&&... fe) { (from_cell(obj.*(fe.ptr), *v.get(fe.name)), ...); },
               T::zen_fields());
    return obj;
}

// ---- the access model (ZEN_EXPOSE / ZEN_HIDE) -------------------------------
// With no tag a field can be read by a poke and not written. ZEN_EXPOSE opts in to writing;
// ZEN_HIDE opts out of raw reading, so the value is reached only by asking the weave. Each works
// on one field or, bare in the state struct, on all of them (OR'd onto every field). The tags
// govern values only: a field's existence, name, type and tags are always visible, and
// access_of<T>() returns every field. docs/guides/diagnostics.md#5-live-inspection

/// The whole-state tag bits of a shape (kNone if untagged). The flag must be a constant
/// expression that is true, not merely a member that exists, so a hand-written
/// `zen_expose_all = false`, or a field named `zen_expose_all`, never exposes every field.
template <class T>
constexpr std::uint8_t shape_access_bits() {
    std::uint8_t bits = access::kNone;
    if constexpr (requires { requires T::zen_expose_all; }) {
        bits |= access::kExpose;
    }
    if constexpr (requires { requires T::zen_hide_all; }) {
        bits |= access::kHide;
    }
    return bits;
}

/// True iff the shape carries a whole-state tag; used to refuse a bare ZEN_EXPOSE(); or
/// ZEN_HIDE(); placed in the weave class instead of the state struct.
template <class T>
constexpr bool has_whole_state_tag() {
    return (requires { requires T::zen_expose_all; }) || (requires { requires T::zen_hide_all; });
}

/// One field's access record, always visible.
struct FieldAccess {
    std::string name;
    loom::TypeRef type;
    bool writable = false; ///< ZEN_EXPOSE (field- or whole-state scope): manipulable
    bool hidden = false;   ///< ZEN_HIDE (field- or whole-state scope): value is message-only
};

/// The full access table of a shape: every field, tagged or not, with its tags. Nothing filters
/// it.
template <Shape T>
std::vector<FieldAccess> access_of() {
    std::vector<FieldAccess> out;
    constexpr std::uint8_t shape_bits = shape_access_bits<T>();
    std::apply(
        [&](auto&&... fe) {
            (out.push_back(FieldAccess{
                 fe.name,
                 type_ref_for<typename std::decay_t<decltype(fe)>::member_type>::get(),
                 ((fe.access | shape_bits) & access::kExpose) != 0,
                 ((fe.access | shape_bits) & access::kHide) != 0}),
             ...);
        },
        T::zen_fields());
    return out;
}

} // namespace loom

/// Register a member of the enclosing ZEN_SHAPE struct (names it once more).
#define ZEN_FIELD(member) ::loom::field_entry(#member, &ZenSelf::member)

// The two access tags each have two scopes, chosen by whether an argument is given:
//   field scope, in place of ZEN_FIELD in the registration list:
//       ZEN_SHAPE(S, 1, ZEN_EXPOSE(rate), ZEN_HIDE(raw_total), ZEN_FIELD(label));
//   whole-state scope, a bare line inside the state struct (the ZEN_SHAPE type, not the weave
//   class, where it would do nothing; WeaveBase refuses that at compile time):
//       ZEN_EXPOSE();   // every field manipulable
//       ZEN_HIDE();     // every field's value message-only
#define ZEN_DETAIL_CAT(a, b) a##b

/// Opt a field (or, bare, every field of the weave's state) IN to write.
#define ZEN_EXPOSE(...) ZEN_DETAIL_CAT(ZEN_DETAIL_EXPOSE_, __VA_OPT__(FIELD))(__VA_ARGS__)
#define ZEN_DETAIL_EXPOSE_FIELD(member)                                                            \
    ::loom::field_entry(#member, &ZenSelf::member, ::loom::access::kExpose)
#define ZEN_DETAIL_EXPOSE_()                                                                        \
    static constexpr bool zen_expose_all = true;                                                    \
    static_assert(true, "") /* swallow the trailing semicolon */

/// Opt a field (or, bare, every field of the weave's state) OUT of raw read.
#define ZEN_HIDE(...) ZEN_DETAIL_CAT(ZEN_DETAIL_HIDE_, __VA_OPT__(FIELD))(__VA_ARGS__)
#define ZEN_DETAIL_HIDE_FIELD(member)                                                              \
    ::loom::field_entry(#member, &ZenSelf::member, ::loom::access::kHide)
#define ZEN_DETAIL_HIDE_()                                                                          \
    static constexpr bool zen_hide_all = true;                                                      \
    static_assert(true, "") /* swallow the trailing semicolon */

/// Declare a struct as a Zen shape. The version is required and part of the identity: a shape
/// is never changed in place, and a new version is a new content id.
#define ZEN_SHAPE(ShapeName, ShapeVersion, ...)                                                    \
    using ZenSelf = ShapeName;                                                                      \
    static constexpr const char* zen_name = #ShapeName;                                             \
    static constexpr ::std::uint32_t zen_version = (ShapeVersion);                                  \
    static auto zen_fields() { return ::std::make_tuple(__VA_ARGS__); }                             \
    static_assert(true, "") /* swallow the trailing semicolon */

#endif // ZEN_WEAVE_SHAPE_HPP
