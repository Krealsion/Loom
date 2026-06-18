#ifndef ZEN_AUTHOR_SHAPE_HPP
#define ZEN_AUTHOR_SHAPE_HPP

// Schema-from-struct: write a shape once as a plain C++ struct, derive the rest.
// This is pure sugar over zen-core — it adds no schema type, no value type, and
// no validator. It emits schemas only through SchemaBuilder (so a derived schema
// is byte-for-byte identical, same content-id, to the hand-built equivalent) and
// converts struct <-> Value at the edges, handing Values to the same admit.
//
// An author writes the struct's real members plus one in-class line:
//
//   struct Ping {
//       std::int64_t seq;
//       ZEN_SHAPE(Ping, /*version=*/1, ZEN_FIELD(seq));
//   };
//
// The field-registration block (the ZEN_FIELD list) names each member exactly
// once more, so the macro can locate it and capture its name as a string. That
// name-restatement is precisely and only what C++26 reflection will later remove
// — it is confined to zen_fields(), and nothing downstream depends on how that
// tuple is produced.

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace zen::author {

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
std::shared_ptr<const zen::Schema> schema_of();
template <class T>
zen::Value to_value(const T& obj);
template <Shape T>
T from_value(const zen::Value& v);

/// One registered field: its wire name and a pointer to the member.
template <class C, class M>
struct FieldEntry {
    const char* name;
    M C::*ptr;
    using member_type = M;
    using class_type = C;
};

template <class C, class M>
constexpr FieldEntry<C, M> field_entry(const char* name, M C::*ptr) {
    return FieldEntry<C, M>{name, ptr};
}

// ---- Kind deduced from the C++ member type --------------------------------

template <class M>
struct type_ref_for {
    static zen::TypeRef get() {
        static_assert(Shape<M>,
                      "zen::author: unsupported field type; use a supported scalar "
                      "(std::int64_t/double/std::string/bool), zen::Bytes, std::vector<T>, "
                      "or a registered ZEN_SHAPE struct");
        return zen::type_message(schema_of<M>());
    }
};
template <>
struct type_ref_for<std::int64_t> {
    static zen::TypeRef get() { return zen::type_of(zen::Kind::Int); }
};
template <>
struct type_ref_for<double> {
    static zen::TypeRef get() { return zen::type_of(zen::Kind::Float); }
};
template <>
struct type_ref_for<std::string> {
    static zen::TypeRef get() { return zen::type_of(zen::Kind::Text); }
};
template <>
struct type_ref_for<bool> {
    static zen::TypeRef get() { return zen::type_of(zen::Kind::Bool); }
};
// zen::Bytes is std::vector<std::uint8_t>; this full specialization wins over the
// std::vector<T> partial below, so a byte vector is Bytes, not List<Int>.
template <>
struct type_ref_for<zen::Bytes> {
    static zen::TypeRef get() { return zen::type_of(zen::Kind::Bytes); }
};
template <class T>
struct type_ref_for<std::vector<T>> {
    static zen::TypeRef get() { return zen::type_list(type_ref_for<T>::get()); }
};

// ---- struct member <-> Cell -----------------------------------------------

// Forward declarations of the recursive (List / Message) overloads, so a List of
// Messages resolves regardless of definition order (the shapes may live in a
// namespace ADL would not reach).
template <class T>
zen::Cell to_cell(const std::vector<T>& v);
template <Shape U>
zen::Cell to_cell(const U& u);
template <class T>
void from_cell(std::vector<T>& d, const zen::Cell& c);
template <Shape U>
void from_cell(U& d, const zen::Cell& c);

inline zen::Cell to_cell(std::int64_t v) { return zen::Cell::integer(v); }
inline zen::Cell to_cell(double v) { return zen::Cell::real(v); }
inline zen::Cell to_cell(const std::string& v) { return zen::Cell::text(v); }
inline zen::Cell to_cell(bool v) { return zen::Cell::boolean(v); }
inline zen::Cell to_cell(const zen::Bytes& v) { return zen::Cell::bytes(v); }
template <class T>
zen::Cell to_cell(const std::vector<T>& v) {
    zen::Cell::Array arr;
    arr.reserve(v.size());
    for (const auto& e : v) {
        arr.push_back(to_cell(e));
    }
    return zen::Cell::list(std::move(arr));
}
template <Shape U>
zen::Cell to_cell(const U& u) {
    return zen::Cell::message(to_value(u));
}

inline void from_cell(std::int64_t& d, const zen::Cell& c) { d = c.as_int(); }
inline void from_cell(double& d, const zen::Cell& c) { d = c.as_float(); }
inline void from_cell(std::string& d, const zen::Cell& c) { d = c.as_text(); }
inline void from_cell(bool& d, const zen::Cell& c) { d = c.as_bool(); }
inline void from_cell(zen::Bytes& d, const zen::Cell& c) { d = c.as_bytes(); }
template <class T>
void from_cell(std::vector<T>& d, const zen::Cell& c) {
    const zen::Cell::Array& arr = c.as_list();
    d.clear();
    d.reserve(arr.size());
    for (const zen::Cell& e : arr) {
        T tmp{};
        from_cell(tmp, e);
        d.push_back(std::move(tmp));
    }
}
template <Shape U>
void from_cell(U& d, const zen::Cell& c) {
    d = from_value<U>(*c.as_message());
}

// ---- the derivations -------------------------------------------------------

template <class T>
std::shared_ptr<const zen::Schema> build_schema() {
    static_assert(Shape<T>, "zen::author: type is not a ZEN_SHAPE (no zen_fields/zen_version)");
    zen::SchemaBuilder builder(T::zen_name, T::zen_version);
    std::apply(
        [&](auto&&... fe) {
            (builder.add(zen::Field{fe.name,
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
std::shared_ptr<const zen::Schema> schema_of() {
    static const std::shared_ptr<const zen::Schema> s = build_schema<T>();
    return s;
}

/// Convert a struct to a Value claiming its derived schema. Adds no validation;
/// the caller hands the Value to the same admit.
template <class T>
zen::Value to_value(const T& obj) {
    static_assert(Shape<T>, "zen::author: to_value requires a ZEN_SHAPE struct");
    zen::Value v(schema_of<T>());
    std::apply([&](auto&&... fe) { (v.set(fe.name, to_cell(obj.*(fe.ptr))), ...); }, T::zen_fields());
    return v;
}

/// Convert an already-gated Value to its struct. Precondition: `v` has passed
/// the gate against schema_of<T>() (every required field present and well-typed).
template <Shape T>
T from_value(const zen::Value& v) {
    T obj{};
    std::apply([&](auto&&... fe) { (from_cell(obj.*(fe.ptr), *v.get(fe.name)), ...); },
               T::zen_fields());
    return obj;
}

} // namespace zen::author

/// Register a member of the enclosing ZEN_SHAPE struct (names it once more).
#define ZEN_FIELD(member) ::zen::author::field_entry(#member, &ZenSelf::member)

/// Declare a struct as a Zen shape. The version is REQUIRED and becomes part of
/// the identity: there is no way to evolve a shape in place — a new version is a
/// new, distinct content-id by construction. The whole migration chain is keyed
/// on these stable versions, so omitting one fails to compile.
#define ZEN_SHAPE(ShapeName, ShapeVersion, ...)                                                    \
    using ZenSelf = ShapeName;                                                                      \
    static constexpr const char* zen_name = #ShapeName;                                             \
    static constexpr ::std::uint32_t zen_version = (ShapeVersion);                                  \
    static auto zen_fields() { return ::std::make_tuple(__VA_ARGS__); }                             \
    static_assert(true, "") /* swallow the trailing semicolon */

#endif // ZEN_AUTHOR_SHAPE_HPP
