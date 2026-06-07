#ifndef ZEN_SERIALIZE_HPP
#define ZEN_SERIALIZE_HPP

#include <zen/admission.hpp>
#include <zen/gate.hpp> // Report
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace zen {

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
/// trusted.
Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door,
                Report report = Report::FirstError);

/// As above, but resolve the claimed schema against a registry. A claim naming a
/// schema the registry does not hold is refused with UnknownSchema.
Admission admit(const Unverified& unverified, const Registry& registry,
                Report report = Report::FirstError);

/// The compatibility / debug codec: Zen's original self-describing JSON text
/// format, demoted from native. It is inspectable and human-readable, useful for
/// debugging and for tools, but it is lossy of byte-canonicality and larger than
/// the native binary. It funnels through the exact same gate: compat::parse
/// yields a normal Unverified that the same admit() overloads validate.
///
/// Under the strict-core policy the JSON decoder rejects any field the door does
/// not declare (ErrorKind::UnknownField) — there is no partial acceptance and no
/// silent drop in any format.
namespace compat {

/// Serialize a value to JSON text (the former native format). Total for any
/// in-memory Value.
std::string serialize(const Value& value);

/// Parse JSON text into an Unverified. Never throws; malformed input yields a
/// not-well-formed Unverified, exactly like the native parse().
Unverified parse(std::string_view bytes) noexcept;

} // namespace compat

} // namespace zen

#endif // ZEN_SERIALIZE_HPP
