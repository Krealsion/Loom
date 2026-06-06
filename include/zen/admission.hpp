#ifndef ZEN_ADMISSION_HPP
#define ZEN_ADMISSION_HPP

#include <zen/value.hpp>

#include <optional>
#include <string>
#include <vector>

namespace zen {

/// A machine-readable classification of why a claimant was refused. Stable:
/// consumers (e.g. the console) may switch on these.
enum class ErrorKind : std::uint8_t {
    None = 0,
    SchemaMismatch, ///< the claimed schema is not the one this door admits
    UnknownSchema,  ///< a deserialized claim names a schema the registry lacks
    MissingField,   ///< a required field is absent
    TypeMismatch,   ///< a field is present but holds the wrong kind
    NullMessage,    ///< a Message field holds a null sub-value
    MalformedBytes, ///< the serialized envelope could not be parsed at all
    MalformedField, ///< a field's bytes could not be decoded (bad base64, non-integer, bad UTF-8)
};

const char* name_of(ErrorKind k) noexcept;

/// A precise, renderable account of a single refusal. `path` is a dotted /
/// indexed field path ("policy.max_reloads", "tags[2]"; empty at top level) so
/// a console can point exactly at the offending datum.
struct Error {
    ErrorKind kind = ErrorKind::None;
    std::string path;     ///< field path to the offense
    std::string expected; ///< what the door wanted (e.g. "Int", "PlayerState v3")
    std::string actual;   ///< what was found (e.g. "Text", "absent")
    std::string detail;   ///< optional extra context

    /// One-line human rendering, e.g. "policy.max_reloads: expected Int, got Text".
    std::string message() const;
};

/// The structured outcome of the gate. On success it owns a usable Value; on
/// failure it carries one or more Errors. Never both. Convertible to bool.
class Admission {
public:
    static Admission accept(Value v);
    static Admission reject(Error e);
    static Admission reject(std::vector<Error> errors);

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    /// The admitted value. Precondition: ok(). Throws std::logic_error otherwise.
    const Value& value() const&;
    /// Move the admitted value out. Precondition: ok().
    Value value()&&;

    const std::vector<Error>& errors() const noexcept { return errors_; }
    /// The first (in fast mode, the only) error. Precondition: !ok().
    const Error& first_error() const;

private:
    Admission() = default;
    std::optional<Value> value_;
    std::vector<Error> errors_;
};

} // namespace zen

#endif // ZEN_ADMISSION_HPP
