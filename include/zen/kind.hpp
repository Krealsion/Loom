// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KIND_HPP
#define ZEN_KIND_HPP

#include <cstdint>

namespace loom {

/// The primitive grammar of Zen values. This set is a permanent commitment:
/// every wire format and every schema ever published is expressed in these
/// nine kinds. Additions are possible (append-only); removals and meaning
/// changes are not.
///
///   Int     64-bit signed integer
///   Float   IEEE-754 binary64 (double)
///   Text    UTF-8 string
///   Bool    true / false
///   Bytes   opaque octet sequence (escape hatch; use sparingly)
///   Message nested value of a named record shape
///   List    homogeneous sequence of a single element type
///   Union   nested value of a named union shape: exactly one of its cases
///   Any     nested value of any shape the registry holds, named by the value itself
///
/// A reader that switches over Kind decides what each kind means to it; a switch with no
/// default case fails to compile where a kind is added and that reader has not decided.
/// docs/reference/values-and-admission.md#unions-and-the-open-field
enum class Kind : std::uint8_t {
    Int = 0,
    Float = 1,
    Text = 2,
    Bool = 3,
    Bytes = 4,
    Message = 5,
    List = 6,
    Union = 7,
    Any = 8,
};

/// Defined where Kind has Union and Any, so code built against more than one Loom can ask.
#define LOOM_KIND_UNION 1

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
    case Kind::Union:
        return "Union";
    case Kind::Any:
        return "Any";
    }
    return "?";
}

} // namespace loom

#endif // ZEN_KIND_HPP
