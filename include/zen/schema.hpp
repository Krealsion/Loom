// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SCHEMA_HPP
#define ZEN_SCHEMA_HPP

#include <zen/kind.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

class Schema;

namespace detail {
struct SchemaAccess;
} // namespace detail

/// A stable, deterministic 64-bit identity for a schema's normalized structure.
/// Two schemas with the same name, version, and field shape have the same
/// ContentId on every machine and every run. The gate asks it first, as a cheap
/// "is your claim the shape this door expects", before its field-by-field compare.
using ContentId = std::uint64_t;

/// What a shape's fields are. A record holds any of its fields, each present or absent; a union
/// holds exactly one of them, its case. A union whose cases all carry nothing is an
/// enumeration. docs/reference/values-and-admission.md#unions-and-the-open-field
enum class Form : std::uint8_t {
    Record = 0,
    Union = 1,
};

/// The type of a field, or the element type of a list. Recursive: a list's
/// element may itself be a list (nesting) or a message.
///
/// Invariants (enforced when a Schema is built):
///   - kind == Message  =>  message is a record shape, element == nullptr
///   - kind == Union    =>  message is a union shape, element == nullptr
///   - kind == List     =>  element != nullptr, message == nullptr
///   - otherwise        =>  message == nullptr, element == nullptr (Any names no shape: the
///                          value names its own, and the registry answers for it)
struct TypeRef {
    Kind kind = Kind::Int;
    std::shared_ptr<const Schema> message;  ///< non-null iff kind is Message or Union
    std::shared_ptr<const TypeRef> element; ///< non-null iff kind == List
};

/// Factory for a primitive type (Int, Float, Text, Bool, Bytes, or Any).
TypeRef type_of(Kind k);
/// Factory for a field holding a nested value of `schema`: Kind::Message for a record shape,
/// Kind::Union for a union shape.
TypeRef type_message(std::shared_ptr<const Schema> schema);
/// Factory for a list type with the given element type (may nest).
TypeRef type_list(TypeRef element);
/// Factory for a field holding a value of any shape the registry holds (Kind::Any).
TypeRef type_any();

/// One named slot in a schema: a record's field, or a union's case.
struct Field {
    std::string name;
    TypeRef type;
    bool required = true; ///< a record's field may be absent when false; a union's case is true
    bool carries = true;  ///< false only for a union's case that carries nothing (its type unread)
};

/// A union's case carrying a value of `type`.
Field case_of(std::string name, TypeRef type);
/// A union's case carrying nothing: an enumeration's member.
Field case_bare(std::string name);

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
    /// A shape of either form. A union needs at least one case; each of its cases is required
    /// and carries a value unless `carries` is false. A record's field always carries.
    Schema(Form form, std::string name, std::uint32_t version, std::vector<Field> fields);

    const std::string& name() const noexcept { return name_; }
    std::uint32_t version() const noexcept { return version_; }
    Form form() const noexcept { return form_; }
    bool is_union() const noexcept { return form_ == Form::Union; }
    /// A record's fields, or a union's cases in declared order.
    const std::vector<Field>& fields() const noexcept { return fields_; }
    ContentId content_id() const noexcept { return content_id_; }

    /// Field lookup by name, or nullptr if absent. Linear scan; field counts
    /// are small and this keeps the value model index-free.
    const Field* find(std::string_view field_name) const noexcept;

private:
    // A schema whose content id is the one given rather than the one computed, so that a check
    // which must not trust the id can be shown two definitions sharing it without searching for
    // a real collision. Reached only through `detail::SchemaAccess`, which the suite defines
    // (tests/schema_access.hpp); nothing in the library makes one.
    friend struct detail::SchemaAccess;
    struct GivenContentId {};
    Schema(GivenContentId, std::string name, std::uint32_t version, std::vector<Field> fields,
           ContentId content_id);

    std::string name_;
    std::uint32_t version_;
    Form form_ = Form::Record;
    std::vector<Field> fields_;
    ContentId content_id_;
};

/// True iff the two schemas have the same form and the same fields, in the same order: each
/// field's name, requiredness, whether it carries, and type, a nested shape compared by
/// `same_identity` and a list by its element, however deep. Name and version are not compared.
bool same_structure(const Schema& a, const Schema& b) noexcept;

/// True iff both schemas are one definition: the same name, version and content id, AND the
/// same structure (`same_structure`). The content id is a 64-bit FNV-1a hash of the normalized
/// structure, not a cryptographic one, so it is asked first, as the cheap answer for drifted
/// definitions; equal ids are then confirmed field by field, and two definitions of one (name,
/// version) that share an id but not a structure are told apart. One object is its own
/// definition without either question.
///
/// This is what decides that two definitions agree: the registry's claims and publication, a
/// reload candidate's vocabulary and state shape, and a weave's typed handler selection. Bare
/// `a.content_id() == b.content_id()` is only the drift check, which the gate asks before its
/// own field-by-field compare.
inline bool same_identity(const Schema& a, const Schema& b) noexcept {
    if (&a == &b) {
        return true;
    }
    return a.name() == b.name() && a.version() == b.version() &&
           a.content_id() == b.content_id() && same_structure(a, b);
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
/// Convenience: build an immutable union shape from its cases (`case_of`, `case_bare`).
std::shared_ptr<const Schema> make_union(std::string name, std::uint32_t version,
                                         std::vector<Field> cases);

/// THE COMPONENT CLOSURE OF A SCHEMA — every schema `root` transitively references
/// through its field types (a Message field, a List of Messages, however deep),
/// appended to `out` in POST-ORDER: a component precedes whatever references it,
/// so a reader that must resolve entry N+1 against entries 0..N (a manifest's
/// `referenced` list, a self-description package) needs one forward pass.
/// `root` itself is NOT appended; the caller lists roots where roots belong.
///
/// DEDUPLICATED BY IDENTITY, NEVER BY NAME ALONE. A component already present in
/// `out` with the same (name, version) AND the same content-id is not appended
/// again — two roots sharing one `Pos v1` carry it once. A component with the
/// same (name, version) and DIFFERENT content IS appended, so a contradiction
/// inside one declaration (`Box v1 { Part v1 {a} }` beside `Box2 v1 { Part v1
/// {a, b} }`) survives to meet whatever agreement check reads the result: a
/// `Registry` claim refuses it with `SchemaConflict`, and a manifest carries both
/// definitions for its loader to refuse
/// (docs/decisions/declared-vocabulary-is-agreed-at-admission.md).
///
/// This is the one traversal every declaration shares — a weave's registration
/// on the bus, a library's manifest, a described accept-set, a consumer's own
/// descriptors — and it lives here, at the layer that owns schemas, so that no
/// layer has to reach up into another's codec to walk a shape it already holds.
///
/// Cycles are impossible by construction: a `Schema` is immutable and built from
/// components that already exist, so nothing can reference itself and the walk is
/// bounded by the schema DAG. WORK IS BOUNDED BY THE CLOSURE, NOT BY THE PATHS
/// THROUGH IT: `out` is the visited set, a component already carried is not
/// descended into again (it was carried after its own components), so each
/// distinct identity is expanded once however many fields reach it. Each
/// reference costs one scan of `out`, so the cost is proportional to the number
/// of message references times the closure's size, never exponential in a
/// shared graph.
void collect_referenced(const Schema& root, std::vector<std::shared_ptr<const Schema>>& out);
/// The same walk from a single type reference (a field's type, a list's element).
void collect_referenced(const TypeRef& type, std::vector<std::shared_ptr<const Schema>>& out);

} // namespace loom

#endif // ZEN_SCHEMA_HPP
