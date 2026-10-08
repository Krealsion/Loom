// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: a std::optional member of every kind, through every derivation.
#include "members_common.hpp"

namespace {
struct Every {
    std::int64_t id = 0;
    std::optional<std::int64_t> i;
    std::optional<double> f;
    std::optional<std::string> t;
    std::optional<bool> b;
    std::optional<loom::Bytes> y;
    std::optional<optional_members::Inner> m;
    std::optional<std::vector<std::int64_t>> l;
    std::optional<std::vector<optional_members::Inner>> lm;
    bool optional = false; // a member named `optional` is an ordinary required Bool
    ZEN_SHAPE(Every, 1, ZEN_FIELD(id), ZEN_EXPOSE(i), ZEN_FIELD(f), ZEN_FIELD(t), ZEN_HIDE(b),
              ZEN_FIELD(y), ZEN_FIELD(m), ZEN_FIELD(l), ZEN_FIELD(lm), ZEN_FIELD(optional));
};
} // namespace

loom::Value optional_members_witness(const loom::Value& v) {
    (void)loom::access_of<Every>();
    return optional_members::instantiate<Every>(loom::to_value(loom::from_value<Every>(v)));
}
