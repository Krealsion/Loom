#ifndef ZEN_KIND_HPP
#define ZEN_KIND_HPP

#include <cstdint>

namespace zen {

/// The primitive grammar of Zen values. This set is a permanent commitment:
/// every wire format and every schema ever published is expressed in these
/// seven kinds. Additions are possible (append-only); removals and meaning
/// changes are not.
///
///   Int     64-bit signed integer
///   Float   IEEE-754 binary64 (double)
///   Text    UTF-8 string
///   Bool    true / false
///   Bytes   opaque octet sequence (escape hatch; use sparingly)
///   Message nested value of a named schema
///   List    homogeneous sequence of a single element type
enum class Kind : std::uint8_t {
    Int = 0,
    Float = 1,
    Text = 2,
    Bool = 3,
    Bytes = 4,
    Message = 5,
    List = 6,
};

/// Stable spelling of a kind, for diagnostics and the wire format.
/// These spellings are part of the public contract; do not rename them.
constexpr const char* name_of(Kind k) noexcept {
    switch (k) {
    case Kind::Int:
        return "Int";
    case Kind::Float:
        return "Float";
    case Kind::Text:
        return "Text";
    case Kind::Bool:
        return "Bool";
    case Kind::Bytes:
        return "Bytes";
    case Kind::Message:
        return "Message";
    case Kind::List:
        return "List";
    }
    return "?";
}

} // namespace zen

#endif // ZEN_KIND_HPP
