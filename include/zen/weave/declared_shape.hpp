// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_DECLARED_SHAPE_HPP
#define ZEN_WEAVE_DECLARED_SHAPE_HPP

// One shape named by its name and version, as a weave declares it: the row the answers about a
// participant's declarations carry (`RoleInfo` version 3's `emitted`, zen/kernel/control.hpp) and
// the participants claim's `accepts` and `emits` (zen/host/participants.hpp). A name, never the
// shape's description: on one bus a name and a version are one definition, since the agreement
// wall refuses a second.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>

namespace loom {

/// One shape by its name and version, as a weave declares it.
struct DeclaredShape {
    std::string name;
    std::int64_t version = 0;
    ZEN_SHAPE(DeclaredShape, 1, ZEN_FIELD(name), ZEN_FIELD(version));
};

} // namespace loom

#endif // ZEN_WEAVE_DECLARED_SHAPE_HPP
