// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The vocabulary the stranger's host and the stranger's weave share: a header a CONSUMER
// writes, using only what find_package(loom) installed, in the MSVC-sensitive shape (ZEN_SHAPE
// with both field-scope access tags, where __VA_OPT__ dispatch lives).

#ifndef ZEN_PACKAGE_WITNESS_PROTOCOL_HPP
#define ZEN_PACKAGE_WITNESS_PROTOCOL_HPP

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace witness {

struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq));
};

struct Pong {
    std::int64_t seq;
    ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq));
};

// A NESTED accepted shape, and the component it nests. The weave Accepts `Nested` and NOT
// `Inner`: zen.SchemaDesc names a nested message by (name, version), so a consumer told only
// "you may send me Nested" could not decode it unless the self-description carries its
// DEPENDENCY CLOSURE, not merely its roots, across the installed package and the real C ABI.
struct Inner {
    std::int64_t k;
    ZEN_SHAPE(Inner, 1, ZEN_FIELD(k));
};

struct Nested {
    Inner one;
    std::vector<Inner> many;
    ZEN_SHAPE(Nested, 1, ZEN_FIELD(one), ZEN_FIELD(many));
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
