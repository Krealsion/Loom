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

namespace zen {

class Value;

/// Opaque octet sequence backing the Bytes kind.
using Bytes = std::vector<std::uint8_t>;

/// One field's datum. A cell holds exactly one of the seven kinds. Message and
/// List are held behind indirection so the type is finite and recursive: a
/// message can carry a message, a list can carry lists or messages, and the one
/// boundary rule recurses to any depth.
///
/// Nested messages are held by shared_ptr; copying a Value therefore shares its
/// sub-values. Values are intended to be treated as immutable after they pass
/// the gate — do not mutate a sub-value that may be shared.
class Cell {
public:
    using Array = std::vector<Cell>;
    using Storage = std::variant<std::int64_t, double, std::string, bool, Bytes,
                                 std::shared_ptr<Value>, Array>;

    static Cell integer(std::int64_t v);
    static Cell real(double v);
    static Cell text(std::string v);
    static Cell boolean(bool v);
    static Cell bytes(Bytes v);
    static Cell message(Value v);
    static Cell message(std::shared_ptr<Value> v);
    static Cell list(Array v);

    /// The kind of value this cell actually holds.
    Kind kind() const noexcept;

    bool is(Kind k) const noexcept { return kind() == k; }

    // Typed access. Precondition: the cell holds the matching kind; otherwise
    // std::bad_variant_access is thrown. Check kind() first on untrusted cells.
    std::int64_t as_int() const;
    double as_float() const;
    const std::string& as_text() const;
    bool as_bool() const;
    const Bytes& as_bytes() const;
    const std::shared_ptr<Value>& as_message() const;
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
class Value {
public:
    /// Build an empty value claiming `schema`. All fields start absent.
    explicit Value(std::shared_ptr<const Schema> schema);

    const Schema& schema() const noexcept { return *schema_; }
    const std::shared_ptr<const Schema>& schema_ptr() const noexcept { return schema_; }

    /// Set a declared field. Returns *this for chaining.
    /// Throws std::out_of_range if `field` is not declared by the schema — a
    /// value cannot hold a field its claimed shape does not name.
    Value& set(std::string_view field, Cell value);

    /// The datum for a field, or nullptr if the field is absent.
    const Cell* get(std::string_view field) const noexcept;
    bool has(std::string_view field) const noexcept;

    /// Number of fields declared by the schema (present or not).
    std::size_t field_count() const noexcept { return slots_.size(); }
    /// The datum in slot i (aligned to schema().fields()[i]), or nullptr.
    const Cell* at(std::size_t i) const noexcept;

private:
    std::shared_ptr<const Schema> schema_;
    std::vector<std::optional<Cell>> slots_; ///< aligned 1:1 with schema_->fields()
};

/// Source of cells for blind construction. Given a field descriptor, return its
/// datum, or std::nullopt to leave the field absent (e.g. an optional field).
using CellSource = std::function<std::optional<Cell>(const Field&)>;

/// Build a value for a schema discovered at runtime, with no compiled knowledge
/// of its type: walk the schema's fields in order and fill each from `source`.
/// This is the in-engine console's path — the act a static-struct-only design
/// cannot perform. The result is unvalidated; pass it through the gate.
Value construct_blind(std::shared_ptr<const Schema> schema, const CellSource& source);

} // namespace zen

#endif // ZEN_VALUE_HPP
