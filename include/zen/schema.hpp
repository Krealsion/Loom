#ifndef ZEN_SCHEMA_HPP
#define ZEN_SCHEMA_HPP

#include <zen/kind.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zen {

class Schema;

/// A stable, deterministic 64-bit identity for a schema's normalized structure.
/// Two schemas with the same name, version, and field shape have the same
/// ContentId on every machine and every run. The gate uses it as a cheap "is
/// your claim the shape this door expects" check instead of a recursive compare.
using ContentId = std::uint64_t;

/// The type of a field, or the element type of a list. Recursive: a list's
/// element may itself be a list (nesting) or a message.
///
/// Invariants (enforced when a Schema is built):
///   - kind == Message  =>  message != nullptr, element == nullptr
///   - kind == List     =>  element != nullptr, message == nullptr
///   - otherwise        =>  message == nullptr, element == nullptr
struct TypeRef {
    Kind kind = Kind::Int;
    std::shared_ptr<const Schema> message;  ///< non-null iff kind == Message
    std::shared_ptr<const TypeRef> element; ///< non-null iff kind == List
};

/// Factory for a primitive type (anything but Message/List).
TypeRef type_of(Kind k);
/// Factory for a nested-message type.
TypeRef type_message(std::shared_ptr<const Schema> schema);
/// Factory for a list type with the given element type (may nest).
TypeRef type_list(TypeRef element);

/// One named slot in a schema.
struct Field {
    std::string name;
    TypeRef type;
    bool required = true;
};

/// A runtime descriptor of a value's shape. Immutable once constructed: a
/// published schema is frozen forever (register a new version to change shape).
/// Construct via SchemaBuilder or the constructor; both compute the content id
/// and reject malformed type references.
class Schema {
public:
    /// Throws std::invalid_argument if any field's TypeRef violates its
    /// invariants (e.g. a Message field with no nested schema) or if two
    /// fields share a name.
    Schema(std::string name, std::uint32_t version, std::vector<Field> fields);

    const std::string& name() const noexcept { return name_; }
    std::uint32_t version() const noexcept { return version_; }
    const std::vector<Field>& fields() const noexcept { return fields_; }
    ContentId content_id() const noexcept { return content_id_; }

    /// Field lookup by name, or nullptr if absent. Linear scan; field counts
    /// are small and this keeps the value model index-free.
    const Field* find(std::string_view field_name) const noexcept;

private:
    std::string name_;
    std::uint32_t version_;
    std::vector<Field> fields_;
    ContentId content_id_;
};

/// True iff both schemas are the *same identity*: same name, same version, and
/// same normalized structure (content id). This is resolvable identity plus
/// structural integrity — it cannot be fooled by a content-id hash collision,
/// nor by an unregistered schema claiming a taken (name, version) with a
/// different shape. Reaching for same_identity is therefore always correct.
///
/// Bare `a.content_id() == b.content_id()` is a different, narrower thing: an
/// integrity/drift check the gate, the wire reader, and the registry use inline,
/// where the (name, version) is already established upstream by door selection.
inline bool same_identity(const Schema& a, const Schema& b) noexcept {
    return a.name() == b.name() && a.version() == b.version() &&
           a.content_id() == b.content_id();
}

/// Fluent construction of a schema. build() returns an immutable shared owner.
class SchemaBuilder {
public:
    SchemaBuilder(std::string name, std::uint32_t version);

    /// Add a primitive field (Kind must not be Message or List; use the
    /// dedicated helpers for those).
    SchemaBuilder& field(std::string name, Kind kind, bool required = true);
    /// Add a nested-message field.
    SchemaBuilder& message(std::string name, std::shared_ptr<const Schema> schema,
                           bool required = true);
    /// Add a list field with the given element type.
    SchemaBuilder& list(std::string name, TypeRef element, bool required = true);
    /// Add a fully general field.
    SchemaBuilder& add(Field f);

    std::shared_ptr<const Schema> build() const;

private:
    std::string name_;
    std::uint32_t version_;
    std::vector<Field> fields_;
};

/// Convenience: build an immutable schema in one call.
std::shared_ptr<const Schema> make_schema(std::string name, std::uint32_t version,
                                          std::vector<Field> fields);

} // namespace zen

#endif // ZEN_SCHEMA_HPP
