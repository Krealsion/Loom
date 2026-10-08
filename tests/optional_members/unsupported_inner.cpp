// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: an optional of a type no field takes.
#include "members_common.hpp"

namespace {
struct Narrow {
    std::optional<int> x;
    ZEN_SHAPE(Narrow, 1, ZEN_FIELD(x));
};
} // namespace

std::shared_ptr<const loom::Schema> optional_members_witness() { return loom::schema_of<Narrow>(); }
