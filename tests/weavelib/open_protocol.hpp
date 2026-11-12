// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#pragma once

// The shapes a loaded weave reading an open field is spoken to in: a note holding a value of any
// registered shape, and the order to send one on. `Moved` is a shape the reader declares;
// `Strange` is one it does not, which another participant makes registered on the bus.

#include <zen/weave.hpp>

namespace open_test {
struct Moved {
    std::int64_t x = 0;
    ZEN_SHAPE(Moved, 1, ZEN_FIELD(x));
};
struct Strange {
    std::int64_t y = 0;
    ZEN_SHAPE(Strange, 1, ZEN_FIELD(y));
};
struct Note {
    loom::Any item;
    ZEN_SHAPE(Note, 1, ZEN_FIELD(item));
};
/// Send `target` a Note holding Moved{x}.
struct Pass {
    std::int64_t target = 0;
    std::int64_t x = 0;
    ZEN_SHAPE(Pass, 1, ZEN_FIELD(target), ZEN_FIELD(x));
};
} // namespace open_test
