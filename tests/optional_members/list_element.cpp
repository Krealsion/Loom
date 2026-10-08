// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: an optional list element, read through from_value alone.
#include "members_common.hpp"

namespace {
struct Read {
    std::vector<std::optional<std::int64_t>> xs;
    ZEN_SHAPE(Read, 1, ZEN_FIELD(xs));
};
} // namespace

std::size_t optional_members_witness(const loom::Value& v) {
    return loom::from_value<Read>(v).xs.size();
}
