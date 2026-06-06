#include "detail/json.hpp"

namespace zen::detail {

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    for (const auto& m : members) {
        if (m.first == key) {
            return &m.second;
        }
    }
    return nullptr;
}

namespace {

void encode_utf8(std::uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view in, int max_depth) : s_(in), n_(in.size()), max_depth_(max_depth) {}

    JsonParse run() {
        skip_ws();
        JsonValue v;
        if (!parse_value(v, 0)) {
            return fail();
        }
        skip_ws();
        if (i_ != n_) {
            err_ = "trailing characters after JSON value";
            return fail();
        }
        JsonParse r;
        r.ok = true;
        r.value = std::move(v);
        return r;
    }

private:
    std::string_view s_;
    std::size_t n_;
    std::size_t i_ = 0;
    int max_depth_;
    std::string err_;

    JsonParse fail() {
        JsonParse r;
        r.ok = false;
        r.error = err_.empty() ? "malformed JSON" : err_;
        return r;
    }

    char peek() const { return i_ < n_ ? s_[i_] : '\0'; }

    void skip_ws() {
        while (i_ < n_) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, int depth) {
        skip_ws();
        if (i_ >= n_) {
            err_ = "unexpected end of input";
            return false;
        }
        const char c = s_[i_];
        switch (c) {
        case '{':
            return parse_object(out, depth);
        case '[':
            return parse_array(out, depth);
        case '"':
            out.type = JsonValue::Type::String;
            return parse_string(out.text);
        case 't':
        case 'f':
            return parse_bool(out);
        case 'n':
            return parse_null(out);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parse_number(out);
            }
            err_ = "unexpected character";
            return false;
        }
    }

    bool enter(int depth) {
        if (depth + 1 > max_depth_) {
            err_ = "maximum nesting depth exceeded";
            return false;
        }
        return true;
    }

    bool parse_object(JsonValue& out, int depth) {
        if (!enter(depth)) {
            return false;
        }
        ++i_; // consume '{'
        out.type = JsonValue::Type::Object;
        skip_ws();
        if (peek() == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') {
                err_ = "expected string key in object";
                return false;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            for (const auto& m : out.members) {
                if (m.first == key) {
                    err_ = "duplicate object key";
                    return false;
                }
            }
            skip_ws();
            if (peek() != ':') {
                err_ = "expected ':' in object";
                return false;
            }
            ++i_; // consume ':'
            JsonValue val;
            if (!parse_value(val, depth + 1)) {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(val));
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == '}') {
                ++i_;
                return true;
            }
            err_ = "expected ',' or '}' in object";
            return false;
        }
    }

    bool parse_array(JsonValue& out, int depth) {
        if (!enter(depth)) {
            return false;
        }
        ++i_; // consume '['
        out.type = JsonValue::Type::Array;
        skip_ws();
        if (peek() == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            JsonValue val;
            if (!parse_value(val, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(val));
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == ']') {
                ++i_;
                return true;
            }
            err_ = "expected ',' or ']' in array";
            return false;
        }
    }

    bool hex_digit(char c, std::uint32_t& out) {
        if (c >= '0' && c <= '9') {
            out = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            out = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            out = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        return true;
    }

    bool parse_hex4(std::uint32_t& cp) {
        if (i_ + 4 > n_) {
            err_ = "truncated \\u escape";
            return false;
        }
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            std::uint32_t d = 0;
            if (!hex_digit(s_[i_ + static_cast<std::size_t>(k)], d)) {
                err_ = "invalid \\u escape";
                return false;
            }
            cp = (cp << 4) | d;
        }
        i_ += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        ++i_; // consume opening quote
        out.clear();
        for (;;) {
            if (i_ >= n_) {
                err_ = "unterminated string";
                return false;
            }
            const auto c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c == '\\') {
                ++i_;
                if (i_ >= n_) {
                    err_ = "unterminated escape";
                    return false;
                }
                const char e = s_[i_++];
                switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate; require a trailing low surrogate.
                        if (i_ + 2 > n_ || s_[i_] != '\\' || s_[i_ + 1] != 'u') {
                            err_ = "unpaired high surrogate";
                            return false;
                        }
                        i_ += 2;
                        std::uint32_t lo = 0;
                        if (!parse_hex4(lo)) {
                            return false;
                        }
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            err_ = "invalid low surrogate";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        err_ = "unexpected low surrogate";
                        return false;
                    }
                    encode_utf8(cp, out);
                    break;
                }
                default:
                    err_ = "invalid escape";
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                err_ = "control character in string";
                return false;
            }
            out.push_back(static_cast<char>(c));
            ++i_;
        }
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = i_;
        if (peek() == '-') {
            ++i_;
        }
        if (i_ >= n_) {
            err_ = "malformed number";
            return false;
        }
        if (s_[i_] == '0') {
            ++i_; // a leading zero must stand alone (no 0123)
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
            }
        } else {
            err_ = "malformed number";
            return false;
        }
        if (i_ < n_ && s_[i_] == '.') {
            ++i_;
            if (i_ >= n_ || s_[i_] < '0' || s_[i_] > '9') {
                err_ = "malformed fraction";
                return false;
            }
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
            }
        }
        if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }
            if (i_ >= n_ || s_[i_] < '0' || s_[i_] > '9') {
                err_ = "malformed exponent";
                return false;
            }
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
            }
        }
        out.type = JsonValue::Type::Number;
        out.text.assign(s_.substr(start, i_ - start));
        return true;
    }

    bool match_literal(std::string_view lit) {
        if (s_.substr(i_, lit.size()) != lit) {
            err_ = "invalid literal";
            return false;
        }
        i_ += lit.size();
        return true;
    }

    bool parse_bool(JsonValue& out) {
        if (peek() == 't') {
            if (!match_literal("true")) {
                return false;
            }
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (!match_literal("false")) {
            return false;
        }
        out.type = JsonValue::Type::Bool;
        out.boolean = false;
        return true;
    }

    bool parse_null(JsonValue& out) {
        if (!match_literal("null")) {
            return false;
        }
        out.type = JsonValue::Type::Null;
        return true;
    }
};

} // namespace

JsonParse parse_json(std::string_view input, int max_depth) {
    Parser p(input, max_depth);
    return p.run();
}

void json_quote(std::string_view s, std::string& out) {
    out.push_back('"');
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0xF]);
                out.push_back(kHex[c & 0xF]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

bool valid_utf8(std::string_view s) noexcept {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            lo = 0x80;
            hi = 0x7FF;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
            lo = 0x800;
            hi = 0xFFFF;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
            lo = 0x10000;
            hi = 0x10FFFF;
        } else {
            return false;
        }
        if (i + 1 + extra > n) {
            return false; // truncated multi-byte sequence
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < lo || cp > hi) {
            return false; // overlong or out of range
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false; // surrogate halves are not valid scalar values
        }
        i += 1 + extra;
    }
    return true;
}

} // namespace zen::detail
