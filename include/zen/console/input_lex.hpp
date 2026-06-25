#ifndef ZEN_CONSOLE_INPUT_LEX_HPP
#define ZEN_CONSOLE_INPUT_LEX_HPP

// Text -> structured-Arg lexing for the console's text frontends. This is the ONLY job of a
// text skin: turn a typed line into the engine's structured Arg/Ref/FieldValue types, which
// the engine then resolves and gate-sends. The lexers live here (header-only, in the engine
// library's namespace) so EVERY frontend — the plain terminal, the full-screen TUI, and the
// tests — shares ONE copy; a divergent lexer would mean the smoke test proves the wrong
// parser. No terminal/rendering dependency: pure string -> data.

#include <zen/console/console.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace loom {

/// A lexed token. `quoted` is set if the token contained a double-quote — the quote-for-text
/// rule: a numeric-looking string in quotes (`"5"`) lexes to Text, the bareword (`5`) to Int.
struct Token {
    std::string text;
    bool quoted = false;
};

/// Quote-respecting tokenizer: split on whitespace, but keep whitespace inside "double quotes"
/// together as one token. The quote characters themselves are dropped from the token text.
inline std::vector<Token> tokenize(const std::string& line) {
    std::vector<Token> out;
    std::string cur;
    bool in_quote = false, started = false, quoted = false;
    auto flush = [&]() {
        if (started) {
            out.push_back({cur, quoted});
            cur.clear();
            quoted = false;
            started = false;
        }
    };
    for (char ch : line) {
        if (ch == '"') {
            in_quote = !in_quote;
            quoted = true;
            started = true; // even "" is a (empty Text) token
            continue;
        }
        if (!in_quote && (ch == ' ' || ch == '\t')) {
            flush();
            continue;
        }
        cur.push_back(ch);
        started = true;
    }
    flush();
    return out;
}

inline bool parse_u64(const std::string& s, std::uint64_t& out) {
    try {
        std::size_t pos = 0;
        const unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = static_cast<std::uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool is_int(const std::string& s) {
    try {
        std::size_t pos = 0;
        (void)std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

inline bool is_float(const std::string& s) {
    if (s.find('.') == std::string::npos) {
        return false; // require a dot, so "5" stays Int and "5.0" is Float
    }
    try {
        std::size_t pos = 0;
        (void)std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

/// Lex one value string to its narrowest type. Quoted -> always Text. Unquoted: `$mN.field`
/// -> Reference; true/false -> Bool; an integer -> Int; a decimal -> Float; else Text. (The
/// engine, not this, decides which field the value fills and type-checks it.)
inline std::variant<FieldValue, Ref> lex_value(const std::string& s, bool quoted) {
    if (!quoted && !s.empty() && s[0] == '$') {
        const std::string body = s.substr(1);
        const std::size_t dot = body.find('.');
        if (dot == std::string::npos) {
            return Ref{body, ""}; // malformed ($m1 with no field) — the engine errors cleanly
        }
        return Ref{body.substr(0, dot), body.substr(dot + 1)};
    }
    if (!quoted) {
        if (s == "true") {
            return FieldValue{true};
        }
        if (s == "false") {
            return FieldValue{false};
        }
        if (is_int(s)) {
            return FieldValue{static_cast<std::int64_t>(std::stoll(s))};
        }
        if (is_float(s)) {
            return FieldValue{std::stod(s)};
        }
    }
    return FieldValue{s}; // quoted, or a non-numeric bareword -> Text
}

/// Lex one token into an Arg: `field=value` is a named arg (the name is a bareword before the
/// first '='); anything else is a bare (positional/type-directed) literal or reference.
inline Arg lex_arg(const Token& t) {
    Arg a;
    const std::string& s = t.text;
    const std::size_t eq = s.find('=');
    if (eq != std::string::npos && eq > 0) {
        const std::string name = s.substr(0, eq);
        if (name.find(' ') == std::string::npos && name[0] != '$') {
            a.name = name;
            a.value = lex_value(s.substr(eq + 1), t.quoted);
            return a;
        }
    }
    a.value = lex_value(s, t.quoted);
    return a;
}

} // namespace loom

#endif // ZEN_CONSOLE_INPUT_LEX_HPP
