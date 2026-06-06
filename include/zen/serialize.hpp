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

/// Serialize a value to Zen's own self-describing JSON text. The output carries
/// the value's schema identity (name, version, content id) in a header so a
/// reader can challenge it. Encoding is lossless: Int as a decimal string (no
/// 2^53 loss), Float as a shortest round-trip number with NaN/Infinity tokens,
/// Bytes as base64, nested messages and lists structurally. Total: any
/// in-memory Value serializes.
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
    friend Admission admit(const Unverified&, std::shared_ptr<const Schema>, Report);
    friend Admission admit(const Unverified&, const Registry&, Report);
};

/// Parse bytes into an Unverified. Never throws and never crashes, whatever the
/// input. Malformed bytes yield an Unverified whose well_formed() is false and
/// whose admit() refuses with MalformedBytes.
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

} // namespace zen

#endif // ZEN_SERIALIZE_HPP
