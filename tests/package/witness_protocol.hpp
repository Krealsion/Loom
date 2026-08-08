// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The vocabulary the stranger's host and the stranger's weave share.
//
// It is a header a CONSUMER writes, using only what find_package(loom) installed,
// and it is deliberately the shape that broke first under MSVC: ZEN_SHAPE with both
// field-scope access tags, which is where __VA_OPT__ dispatch lives.

#ifndef ZEN_PACKAGE_WITNESS_PROTOCOL_HPP
#define ZEN_PACKAGE_WITNESS_PROTOCOL_HPP

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>

namespace witness {

struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq));
};

struct Pong {
    std::int64_t seq;
    ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq));
};

// The weave's state, carrying BOTH field-scope tags. This shape crosses the C ABI in
// both directions (describe/snapshot/revive), so the macro surface under test is the
// one a real loadable artifact actually uses -- not a compile-only specimen.
struct Tally {
    std::int64_t handled;
    std::int64_t raw_total;
    std::string label;
    ZEN_SHAPE(Tally, 1, ZEN_EXPOSE(handled), ZEN_HIDE(raw_total), ZEN_FIELD(label));
};

} // namespace witness

#endif // ZEN_PACKAGE_WITNESS_PROTOCOL_HPP
