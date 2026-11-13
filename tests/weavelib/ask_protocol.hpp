// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_WEAVELIB_ASK_PROTOCOL_HPP
#define ZEN_TESTS_WEAVELIB_ASK_PROTOCOL_HPP

// The asker probe's vocabulary (ask_probe.cpp), shared with the suites that drive it. Every field
// is fixed-width or text, so the native and the wasm32 builds name the same shapes.

#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace asking {

/// Tell the probe to ask: `form` 0 asks `target`, 1 asks `role`, 2 asks `target` as `as_role`,
/// 3 asks `role` as `as_role`; the question carries `n` and the ask `correlation`.
struct AskKick {
    std::int64_t form = 0;
    std::int64_t target = 0;
    std::string role;
    std::string as_role;
    std::int64_t n = 0;
    std::int64_t correlation = 0;
    ZEN_SHAPE(AskKick, 1, ZEN_FIELD(form), ZEN_FIELD(target), ZEN_FIELD(role), ZEN_FIELD(as_role),
              ZEN_FIELD(n), ZEN_FIELD(correlation));
};

/// The probe's question, and the answer a respondent gives it.
struct AskQuestion {
    std::int64_t n = 0;
    ZEN_SHAPE(AskQuestion, 1, ZEN_FIELD(n));
};
struct AskReply {
    std::int64_t n = 0;
    ZEN_SHAPE(AskReply, 1, ZEN_FIELD(n));
};

/// What the probe holds: its last ask's ticket (or the refusal that queued nothing), and of every
/// `zen.AskEnded` it was told, how many and the last one's words. A notice whose provenance is not
/// Loom's is counted apart and read no further.
struct AskHeard {
    std::int64_t ticket = 0;
    std::string refusal;
    std::int64_t endings = 0;
    std::int64_t forged = 0;
    std::string stage;
    std::string reason;
    std::int64_t ended_attempt = 0;
    std::int64_t correlation = 0;
    std::string shape;
    std::string role;
    std::int64_t target = 0;
    std::int64_t answers = 0;
    ZEN_SHAPE(AskHeard, 1, ZEN_FIELD(ticket), ZEN_FIELD(refusal), ZEN_FIELD(endings),
              ZEN_FIELD(forged), ZEN_FIELD(stage), ZEN_FIELD(reason), ZEN_FIELD(ended_attempt),
              ZEN_FIELD(correlation), ZEN_FIELD(shape), ZEN_FIELD(role), ZEN_FIELD(target),
              ZEN_FIELD(answers));
};

} // namespace asking

#endif // ZEN_TESTS_WEAVELIB_ASK_PROTOCOL_HPP
