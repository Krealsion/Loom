// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_TESTS_CALL_PROTOCOL_HPP
#define ZEN_TESTS_CALL_PROTOCOL_HPP

// The call probe's vocabulary (weavelib/call_probe.cpp, weavelib/call_forge.cpp): the function's
// argument and answer, a knock telling the probe whom to call with what, and what it keeps.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>

namespace calling {

/// The function's argument: two numbers to add.
struct CallAdd {
    std::int64_t a = 0;
    std::int64_t b = 0;
    ZEN_SHAPE(CallAdd, 1, ZEN_FIELD(a), ZEN_FIELD(b));
};

/// Its answer: the sum, and which build of the provider made it.
struct CallSum {
    std::int64_t value = 0;
    std::int64_t by = 0;
    ZEN_SHAPE(CallSum, 1, ZEN_FIELD(value), ZEN_FIELD(by));
};

/// Call `role`'s function with `CallAdd{a, b}`.
struct CallKnock {
    std::string role;
    std::int64_t a = 0;
    std::int64_t b = 0;
    ZEN_SHAPE(CallKnock, 1, ZEN_FIELD(role), ZEN_FIELD(a), ZEN_FIELD(b));
};

/// Read, into the state, how many times this image's own function ran in the memory the weave
/// itself runs in.
struct CallPeek {
    std::int64_t n = 0;
    ZEN_SHAPE(CallPeek, 1, ZEN_FIELD(n));
};

/// `a` that makes the probe's function throw.
inline constexpr std::int64_t kCallFails = -1;

/// The probe's state: the last call's end as the caller held it, and the last peek.
struct CallHeard {
    std::int64_t knocks = 0;
    std::int64_t refusal = 0; ///< the `loom::CallRefusal`, as a number
    std::int64_t value = 0;
    std::int64_t by = 0;
    std::string words;
    std::int64_t provided_here = 0;
    ZEN_SHAPE(CallHeard, 1, ZEN_FIELD(knocks), ZEN_FIELD(refusal), ZEN_FIELD(value), ZEN_FIELD(by),
              ZEN_FIELD(words), ZEN_FIELD(provided_here));
};

} // namespace calling

#endif // ZEN_TESTS_CALL_PROTOCOL_HPP
