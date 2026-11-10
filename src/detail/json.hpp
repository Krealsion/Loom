// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_DETAIL_JSON_HPP
#define ZEN_DETAIL_JSON_HPP

// Internal to loom. A small, hardened JSON reader/writer. Loom owns its wire
// format end to end (no third-party serializer), and this is the only code that
// touches raw external bytes, so it is written to be total: every input,
// hostile or truncated, yields either a parse tree or a clean error — never a
// crash, an overread, or unbounded recursion.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom::detail {

/// A parsed JSON node. Numbers keep their raw token text so numeric conversion
/// (and its precision policy) is decided later, by the schema-guided decoder.
struct JsonValue {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    // Completed below. std::pair needs a complete value type, so it cannot be this
    // node while the node is being defined, and Clang rejects that pair. Copying
    // and destroying the vector are defined once the element type exists.
    struct Member;

    Type type = Type::Null;
    bool boolean = false;
    std::string text; ///< decoded characters (String) or raw token (Number)
    std::vector<JsonValue> items;
    std::vector<Member> members; ///< object members, key then value, in source order

    bool is(Type t) const noexcept { return type == t; }
    /// Object member by key, or nullptr. Keys are unique (parser rejects dups).
    const JsonValue* find(std::string_view key) const noexcept;

    JsonValue() noexcept;
    ~JsonValue();
    JsonValue(const JsonValue&);
    JsonValue(JsonValue&&) noexcept;
    JsonValue& operator=(const JsonValue&);
    JsonValue& operator=(JsonValue&&) noexcept;
};

/// One object member: `first` is the key and `second` the value, in that order.
struct JsonValue::Member {
    std::string first;
    JsonValue second;
};

inline JsonValue::JsonValue() noexcept = default;
inline JsonValue::~JsonValue() = default;
inline JsonValue::JsonValue(const JsonValue&) = default;
inline JsonValue::JsonValue(JsonValue&&) noexcept = default;
inline JsonValue& JsonValue::operator=(const JsonValue&) = default;
inline JsonValue& JsonValue::operator=(JsonValue&&) noexcept = default;

struct JsonParse {
    bool ok = false;
    JsonValue value;
    std::string error; ///< human reason, when !ok
};

/// Parse a complete JSON document. Nesting deeper than `max_depth` is rejected
/// rather than recursed, bounding stack use on adversarial input.
JsonParse parse_json(std::string_view input, int max_depth = 64);

/// Append `s` as a quoted, escaped JSON string (including the surrounding
/// quotes) to `out`.
void json_quote(std::string_view s, std::string& out);

/// True iff `s` is well-formed UTF-8.
bool valid_utf8(std::string_view s) noexcept;

} // namespace loom::detail

#endif // ZEN_DETAIL_JSON_HPP
