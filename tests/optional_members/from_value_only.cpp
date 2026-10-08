// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: from_value alone, the one derivation that never builds the schema.
#include "members_common.hpp"

namespace {
struct Read {
    std::optional<std::vector<optional_members::Inner>> rows;
    ZEN_SHAPE(Read, 1, ZEN_FIELD(rows));
};
} // namespace

std::size_t optional_members_witness(const loom::Value& v) {
    const Read r = loom::from_value<Read>(v);
    return r.rows ? r.rows->size() : 0;
}
