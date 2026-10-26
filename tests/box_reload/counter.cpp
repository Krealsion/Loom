// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The counter: one weave with state, built from this one source as a native library and as a
// WebAssembly module (loom_wasm_weave), for the box reload demo (main.cpp). Each answer says
// which build gave it. COUNTER_REACHES_FOR_A_FILE builds a module that imports WASI's
// `path_open`, which a box never grants: the module the demo's refused reload offers.

#include "counter_shapes.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

using namespace loom;
using namespace box_reload;

namespace {

#if defined(__wasm__)
constexpr const char* kBuild = "wasm32";
#else
constexpr const char* kBuild = "native";
#endif

#if defined(__wasm__) && defined(COUNTER_REACHES_FOR_A_FILE)
extern "C" __attribute__((import_module("wasi_snapshot_preview1"), import_name("path_open")))
std::int32_t counter_path_open(std::int32_t);
#endif

class Counter final
    : public WeaveBase<Counter, CounterState, Accept<CountAsk>, Emit<CountAnswer>> {
public:
    void on(const CountAsk& ask, Mail& mail) {
        ++state_.count;
        state_.sum += ask.seq;
#if defined(__wasm__) && defined(COUNTER_REACHES_FOR_A_FILE)
        if (ask.seq < 0) {
            (void)counter_path_open(0);
        }
#endif
        (void)mail.answer(CountAnswer{ask.seq, state_.count, state_.sum, kBuild});
    }
};

} // namespace

ZEN_EXPORT_WEAVE(Counter)
