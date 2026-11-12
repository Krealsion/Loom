// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_VALUE_HPP
#define ZEN_VALUE_HPP

#include <zen/kind.hpp>
#include <zen/schema.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace loom {

class Value;

/// Opaque octet sequence backing the Bytes kind.
using Bytes = std::vector<std::uint8_t>;

/// One field's datum: an Int, a Float, a Text, a Bool, Bytes, a nested value or a List. A nested
/// value and a List are held behind indirection so the type is finite and recursive: a message
/// can carry a message, a list can carry lists or messages, and the one boundary rule recurses to
/// any depth.
///
/// A nested value is MADE, NEVER ADOPTED: `message(Value)` takes the value and makes the cell's
/// own node, and no overload takes a pointer, so nothing outside a cell holds a way to change
/// what is inside it. The node is shared by every copy of the cell, which is safe because nothing
/// can change it. A Value is mutable until it is placed in a cell, and never after.
/// docs/reference/values-and-admission.md#a-value-never-changes-once-admitted
class Cell {
public:
    using Array = std::vector<Cell>;
    using Storage = std::variant<std::int64_t, double, std::string, bool, Bytes,
                                 std::shared_ptr<const Value>, Array>;

    static Cell integer(std::int64_t v);
    static Cell real(double v);
    static Cell text(std::string v);
    static Cell boolean(bool v);
    static Cell bytes(Bytes v);
    /// A nested value: a record's (Kind::Message) or a union's (Kind::Union), by its shape's form.
    static Cell message(Value v);
    static Cell list(Array v);

    /// The kind of value this cell actually holds: a nested value is Message or Union by the
    /// form of the shape it claims. A field of Kind::Any holds either.
    Kind kind() const noexcept;

    bool is(Kind k) const noexcept { return kind() == k; }

    // Typed access. Precondition: the cell holds the matching kind; otherwise
    // std::bad_variant_access is thrown. Check kind() first on untrusted cells.
    std::int64_t as_int() const;
    double as_float() const;
    const std::string& as_text() const;
    bool as_bool() const;
    const Bytes& as_bytes() const;
    const std::shared_ptr<const Value>& as_message() const;
    const Array& as_list() const;

    const Storage& storage() const noexcept { return data_; }

private:
    explicit Cell(Storage data) : data_(std::move(data)) {}
    Storage data_;
};

/// A self-describing value: it carries a reference to the schema it *claims* to
/// be, plus a datum for each declared field. Construction does not validate;
/// validation is the gate's job (see <zen/gate.hpp>). A Value always carries a
/// schema — there is no shapeless value.
///
/// A value of a union shape holds at most one case: setting or choosing a case replaces the one
/// held, so a union value never holds two. One that holds none is refused by the gate (NoCase).
class Value {
public:
    /// Build an empty value claiming `schema`. All fields start absent; a union holds no case.
    explicit Value(std::shared_ptr<const Schema> schema);

    const Schema& schema() const noexcept { return *schema_; }
    const std::shared_ptr<const Schema>& schema_ptr() const noexcept { return schema_; }

    /// Set a declared field, or a union's case that carries a value. Returns *this for chaining.
    /// Throws std::out_of_range if `field` is not declared by the schema — a
    /// value cannot hold a field its claimed shape does not name — and std::invalid_argument
    /// for a union's case that carries nothing (`choose` it).
    Value& set(std::string_view field, Cell value);

    /// Hold a union's case that carries nothing. Throws std::out_of_range if the shape is not a
    /// union or declares no such case, and std::invalid_argument if the case carries a value.
    Value& choose(std::string_view case_name);

    /// The datum for a field, or nullptr if the field is absent (a union's case not held, or held
    /// and carrying nothing).
    const Cell* get(std::string_view field) const noexcept;
    /// Whether a field is present, or a union's case is the one held.
    bool has(std::string_view field) const noexcept;

    /// A union's held case, as an index into schema().fields(); nullopt for a record and for a
    /// union holding no case.
    std::optional<std::size_t> chosen() const noexcept;

    /// Number of fields declared by the schema (present or not); a union's number of cases.
    std::size_t field_count() const noexcept { return schema_->fields().size(); }
    /// The datum in slot i (aligned to schema().fields()[i]), or nullptr.
    const Cell* at(std::size_t i) const noexcept;

private:
    static constexpr std::size_t kNoCase = static_cast<std::size_t>(-1);

    std::size_t index_of(std::string_view field) const;

    std::shared_ptr<const Schema> schema_;
    /// A record's slots, aligned 1:1 with schema_->fields(); a union's one slot, its case's datum.
    std::vector<std::optional<Cell>> slots_;
    std::size_t case_ = kNoCase; ///< a union's held case
};

/// Source of cells for blind construction. Given a field descriptor, return its
/// datum, or std::nullopt to leave the field absent (e.g. an optional field).
using CellSource = std::function<std::optional<Cell>(const Field&)>;

/// Build a value for a schema discovered at runtime, with no compiled knowledge
/// of its type: walk the schema's fields in order and fill each from `source`.
/// This is the in-engine console's path — the act a static-struct-only design
/// cannot perform. The result is unvalidated; pass it through the gate.
///
/// A union is asked the same way, case by case in declared order, and holds the first case the
/// source answers: a cell for a case that carries one, any cell for a case that carries nothing.
/// A source that answers none leaves it holding no case, which the gate refuses.
Value construct_blind(std::shared_ptr<const Schema> schema, const CellSource& source);

} // namespace loom

#endif // ZEN_VALUE_HPP
