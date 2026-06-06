#ifndef ZEN_DETAIL_JSON_HPP
#define ZEN_DETAIL_JSON_HPP

// Internal to zen-core. A small, hardened JSON reader/writer. Zen owns its wire
// format end to end (no third-party serializer), and this is the only code that
// touches raw external bytes, so it is written to be total: every input,
// hostile or truncated, yields either a parse tree or a clean error — never a
// crash, an overread, or unbounded recursion.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zen::detail {

/// A parsed JSON node. Numbers keep their raw token text so numeric conversion
/// (and its precision policy) is decided later, by the schema-guided decoder.
struct JsonValue {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string text; ///< decoded characters (String) or raw token (Number)
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    bool is(Type t) const noexcept { return type == t; }
    /// Object member by key, or nullptr. Keys are unique (parser rejects dups).
    const JsonValue* find(std::string_view key) const noexcept;
};

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

} // namespace zen::detail

#endif // ZEN_DETAIL_JSON_HPP
