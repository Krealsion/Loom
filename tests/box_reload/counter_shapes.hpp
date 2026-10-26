// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_BOX_RELOAD_COUNTER_SHAPES_HPP
#define ZEN_TESTS_BOX_RELOAD_COUNTER_SHAPES_HPP

// The counter's vocabulary (counter.cpp), shared with the host that drives it (main.cpp). Every
// field is fixed-width or text, so the native and the wasm32 builds name the same shapes.

#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace box_reload {

struct CountAsk {
    std::int64_t seq = 0;
    ZEN_SHAPE(CountAsk, 1, ZEN_FIELD(seq));
};

/// The answer to one ask: its `seq`, the asks this weave has counted and their sum, and which
/// build of the weave answered (`native` or `wasm32`).
struct CountAnswer {
    std::int64_t seq = 0;
    std::int64_t count = 0;
    std::int64_t sum = 0;
    std::string build;
    ZEN_SHAPE(CountAnswer, 1, ZEN_FIELD(seq), ZEN_FIELD(count), ZEN_FIELD(sum), ZEN_FIELD(build));
};

struct CounterState {
    std::int64_t count = 0;
    std::int64_t sum = 0;
    ZEN_SHAPE(CounterState, 1, ZEN_FIELD(count), ZEN_FIELD(sum));
};

} // namespace box_reload

#endif // ZEN_TESTS_BOX_RELOAD_COUNTER_SHAPES_HPP
