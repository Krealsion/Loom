// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// optional_members witness: a field type asked for std::optional directly, outside a member.
#include "members_common.hpp"

loom::TypeRef optional_members_witness() {
    return loom::type_ref_for<std::optional<std::int64_t>>::get();
}
