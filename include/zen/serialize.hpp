// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SERIALIZE_HPP
#define ZEN_SERIALIZE_HPP

#include <zen/admission.hpp>
#include <zen/gate.hpp> // Report
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace loom {

/// The total decoded structure one top-level decode may materialise, shared by every message
/// and list nested in it: the allowance admit() spends, for native and compat bytes alike. The
/// unit is one cell slot, one per declared field of every record entered, one per union entered
/// and one per list element; payload bytes are not counted. The live gate counts a value the same
/// way and refuses one past it (TooLarge), so no live value is larger than a decode admits. A decode totalling exactly this many cells is
/// accepted and one more is refused with MalformedBytes. Host-owned: no caller and nothing on
/// the wire widens it; 65,536 cells is about 6 MiB. See docs/reference/bounds.md.
inline constexpr std::uint64_t kMaxDecodedCells = 1u << 16; // cells in one decode

class Registry;
class Unverified;

// Forward-declared so it can be granted friendship below (it constructs an
// Unverified just like the native parse()).
namespace compat {
Unverified parse(std::string_view bytes) noexcept;
}

/// Serialize a value to Zen's native canonical binary format. The output begins
/// with a self-describing header (magic, format version, schema name, schema
/// version, and a MANDATORY content id) so a reader can challenge it without a
/// schema, followed by a positional, schema-guided body. The encoding is
/// canonical: a given Value has exactly one byte representation (declared field
/// order, minimal varints, normalized NaN, no slack), so native bytes are
/// content-addressable.
///
/// Requires a schema-conforming value (binary is positional and untagged). It
/// throws std::invalid_argument if a field's cell does not match its declared
/// kind, if a present Message is null, or if the schema name exceeds 65535
/// bytes. Validate via admit() first if unsure. The byte container is a
/// std::string holding raw bytes (it may contain embedded NULs).
std::string serialize(const Value& value);

/// A value parsed from bytes but NOT yet validated. By construction it is not a
/// Value and exposes no field data: it reveals only its *claim* (the schema name
/// and version it says it is). The sole path from here to a usable Value is
/// admit(). Bytes that are hostile, truncated, or random yield an Unverified
/// that admit() refuses cleanly — never a crash, a leak, or a trusted value.
class Unverified {
public:
    Unverified() = default;

    /// False if the bytes were not even a well-formed Zen envelope.
    bool well_formed() const noexcept;
    /// The schema name the bytes claim (empty if not well-formed).
    const std::string& claimed_name() const noexcept;
    /// The schema version the bytes claim (0 if not well-formed).
    std::uint32_t claimed_version() const noexcept;

    /// Opaque parsed state; its definition lives in the implementation. Named
    /// here only so the implementation's helpers can refer to it.
    struct Impl;

private:
    std::shared_ptr<const Impl> impl_;

    friend Unverified parse(std::string_view bytes) noexcept;
    friend Unverified compat::parse(std::string_view bytes) noexcept;
    friend Admission admit(const Unverified&, std::shared_ptr<const Schema>, Report);
    friend Admission admit(const Unverified&, std::shared_ptr<const Schema>, const Registry&,
                           Report);
    friend Admission admit(const Unverified&, const Registry&, Report);
};

/// Parse native binary bytes into an Unverified. Never throws and never crashes,
/// whatever the input. It reads only the header (no schema needed) and keeps the
/// body opaque until admit() supplies the door. Malformed or truncated bytes —
/// bad magic, unknown format version, a header missing its content id, anything —
/// yield an Unverified whose well_formed() is false and whose admit() refuses
/// with MalformedBytes.
Unverified parse(std::string_view bytes) noexcept;

/// The persistence gate. Decodes the claimed payload under `door` and runs the
/// SAME structural validator as the live bus path (see <zen/gate.hpp>). The
/// claim must match `door` (identity); the decoded value must conform
/// (structure). `door` is taken by shared owner so the admitted Value's schema
/// can never dangle. Unknown encodings and malformed fields are refused, not
/// trusted. A value an Any field holds names a shape this overload cannot resolve, so it is
/// refused with UnknownSchema; the next overload resolves it.
Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door,
                Report report = Report::FirstError);

/// As above, resolving each value an Any field holds against `open`: decoded under the
/// definition `open` holds for the name and version it names, refused UnknownSchema when `open`
/// holds none and SchemaMismatch when the bytes carry another content id.
Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door,
                const Registry& open, Report report = Report::FirstError);

/// As above, but resolve the claimed schema against a registry, which also resolves every
/// Any field. A claim naming a schema the registry does not hold is refused with UnknownSchema.
Admission admit(const Unverified& unverified, const Registry& registry,
                Report report = Report::FirstError);

/// The compatibility and debug codec: Zen's self-describing JSON text format. Inspectable and
/// human-readable, useful for debugging, tools and peers not written in C++, but not
/// byte-canonical and larger than the native binary. It funnels through the same gate:
/// compat::parse yields an ordinary Unverified that the same admit() overloads validate.
///
/// Under the strict-core policy the JSON decoder rejects any field the door does
/// not declare (ErrorKind::UnknownField) — there is no partial acceptance and no
/// silent drop in any format. The native form is positional and names no field: a value written
/// against a shape with another field list carries another content id, refused SchemaMismatch.
/// A union is an object with one member, its case (`{"moved": {...}}`), or a case carrying
/// nothing as its name alone (`"refused"`); a case it does not declare is UnknownField, several
/// are SeveralCases and none NoCase. An Any field's value is an object naming its shape:
/// `{"schema", "version", "content_id", "fields"}`.
namespace compat {

/// Serialize a value to JSON text. Total for any in-memory Value.
std::string serialize(const Value& value);

/// Parse JSON text into an Unverified. Never throws; malformed input yields a
/// not-well-formed Unverified, exactly like the native parse().
Unverified parse(std::string_view bytes) noexcept;

} // namespace compat

} // namespace loom

#endif // ZEN_SERIALIZE_HPP
