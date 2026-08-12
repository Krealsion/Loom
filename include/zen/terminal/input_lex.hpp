// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TERMINAL_INPUT_LEX_HPP
#define ZEN_TERMINAL_INPUT_LEX_HPP

// Text -> structured-Arg lexing for the TEXT frontends. This is the ONLY job of a text skin:
// turn a typed line into the structured Arg/Ref/FieldValue types the composer resolves. The
// lexers live here (header-only) so EVERY text frontend — the plain console, the full-screen
// TUI, the terminal, and the tests — shares ONE copy; a divergent lexer would mean the smoke
// test proves the wrong parser. No terminal/rendering dependency: pure string -> data.
//
// It depends on the COMPOSER and on the PARTICIPANT SURFACE, not on the console: the
// Arg/Ref/FieldValue vocabulary belongs to <zen/terminal/composer.hpp> and `Address` to
// <zen/terminal/session.hpp>, and a terminal frontend needs this lexing without needing a
// console engine, a tap, or a Switchboard. Neither include brings one: `Address` is a plain
// value type with three named constructors, and the session header holds no Switchboard
// either (that is host wiring, in <zen/host/terminal_wiring.hpp>).

#include <zen/terminal/composer.hpp>
#include <zen/terminal/session.hpp>

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

/// A WHOLE UNSIGNED DECIMAL NUMBER, or a refusal.
///
/// IT REFUSES A SIGN, and that is a repair rather than a nicety (WT-1). `std::stoull` accepts
/// a leading `-` and returns the WRAPPED value, so `-1` parsed as 18446744073709551615 and
/// every caller believed it: `#-1` addressed a weave that cannot exist, and `await -1` was
/// eighteen quintillion turns of the host loop. A `std::uint64_t` has no way to SAY negative,
/// so the only honest answer to one is no -- and refusing is the safe direction to move a
/// parser every frontend already tests the bool of.
///
/// `is_int` / `is_float` below deliberately keep their signs. A VALUE may be negative; a
/// count, a version and a weave id may not, and that is the whole difference.
inline bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s[0] < '0' || s[0] > '9') {
        return false; // a sign, a space, or anything else that is not a digit
    }
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

/// WHERE A TYPED LINE IS ADDRESSED — `#12` one weave, `@office` whichever weave holds it at
/// delivery, `*` publish. There is deliberately no fourth form and no default: an unaddressed
/// send is not a mode, it is a mistake, and `Address` itself has no way to spell one.
///
/// IT LIVES HERE, BESIDE `lex_arg`, BECAUSE IT IS THE OTHER HALF OF ONE GRAMMAR. A terminal
/// command line is an address and then values; the value half has been shared by every text
/// frontend in this tree since TERM-0, while the address half sat in one REPL's anonymous
/// namespace. A second presentation of the same core -- Zengine's Workshop overlay is the
/// first -- would then have had to re-author the syntax, and two authors of one grammar is
/// two grammars: the day `#12` grew a second form, only one of them would learn it.
///
/// Returns false for anything else, INCLUDING a bare number: `12` is not an address, because
/// the sigil is what says which of the three kinds this is, and guessing "probably a weave
/// id" is exactly the default this vocabulary refuses to have.
inline bool parse_address(const std::string& text, Address& out) {
    if (text == "*") {
        out = Address::to_all();
        return true;
    }
    if (text.size() > 1 && text[0] == '@') {
        out = Address::to_role(text.substr(1));
        return true;
    }
    std::uint64_t id = 0;
    if (text.size() > 1 && text[0] == '#' && parse_u64(text.substr(1), id)) {
        out = Address::to_weave(WeaveId{id});
        return true;
    }
    return false;
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

#endif // ZEN_TERMINAL_INPUT_LEX_HPP
