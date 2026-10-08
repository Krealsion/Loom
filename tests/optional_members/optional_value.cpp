// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: an optional's value that is itself optional, in a woven state.
#include "members_common.hpp"

namespace {
struct Twice {
    std::optional<std::optional<std::int64_t>> x;
    ZEN_SHAPE(Twice, 1, ZEN_FIELD(x));
};
} // namespace

loom::Value optional_members_witness(const loom::Value& v) {
    return optional_members::instantiate<Twice>(v);
}
