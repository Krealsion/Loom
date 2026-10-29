// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The box probe: an ordinary woven weave, built as a WebAssembly module by loom_wasm_weave() for
// the `box` suite (tests/test_box.cpp, tests/test_box_bounds.cpp). It counts and answers; its
// WebAssembly build also does what untrusted code may do to a box, each on one `seq`, and meets
// each of the box's bounds. Two variants add one import a box never grants, so a module can be
// refused for exactly that: BOX_PROBE_IMPORT_PATH_OPEN (a WASI call that opens a file) and
// BOX_PROBE_IMPORT_ENV (a function from module `env`).

#include "box_probe_shapes.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <time.h>

using namespace loom;
using namespace box_probe;

namespace {

#if defined(__wasm__)
// The answer door as the module sees it: untrusted code may call an import with any arguments.
extern "C" __attribute__((import_module("zen_host"), import_name("answer"))) std::int32_t
box_answer_raw(const std::uint8_t* payload, std::size_t len);
// ...and the role door, so a name of any length reaches it as written.
extern "C" __attribute__((import_module("zen_host"), import_name("send_to_role"))) std::int32_t
box_send_to_role_raw(const char* role, std::uint64_t reply_to, std::uint64_t correlation,
                     const std::uint8_t* payload, std::size_t len, std::uint64_t* attempt);

#if defined(BOX_PROBE_IMPORT_PATH_OPEN)
extern "C" __attribute__((import_module("wasi_snapshot_preview1"), import_name("path_open")))
std::int32_t box_reach(std::int32_t);
#elif defined(BOX_PROBE_IMPORT_ENV)
extern "C" __attribute__((import_module("env"), import_name("x"))) std::int32_t
box_reach(std::int32_t);
#endif

volatile std::int64_t g_spin = 0;
char* volatile g_grown = nullptr; // escapes, so the allocation is not elided
#endif

class BoxProbe final : public WeaveBase<BoxProbe, BoxProbeState, Accept<BoxPing>, Emit<BoxPong>> {
public:
    LifecyclePolicy policy_config() const { return LifecyclePolicy{kMaxReloads, true}; }

    void on(const BoxPing& p, Mail& mail) {
        ++state_.count;
        bool memory_refused = false;
        bool clock_refused = false;
#if defined(__wasm__)
        if (p.seq == kTrap) {
            __builtin_trap();
        }
        if (p.seq == kSpin) {
            for (;;) {
                g_spin = g_spin + 1;
            }
        }
        if (p.seq == kForge) {
            (void)box_answer_raw(
                reinterpret_cast<const std::uint8_t*>(std::uintptr_t{0xFFFFFF00u}), 4096);
        }
        if (p.seq == kGrow) {
            try {
                char* grown = new char[static_cast<std::size_t>(kGrowBytes)];
                grown[0] = 'x';
                g_grown = grown;
            } catch (const std::bad_alloc&) {
                memory_refused = true;
            }
        }
        if (p.seq == kLog) {
            const std::string bytes(static_cast<std::size_t>(p.size), 'x');
            (void)std::fwrite(bytes.data(), 1, bytes.size(), stderr);
        }
        if (p.seq == kName) {
            const std::string role(static_cast<std::size_t>(p.size), 'r');
            (void)box_send_to_role_raw(role.c_str(), 0, 0, nullptr, 0, nullptr);
        }
        if (p.seq == kExit) {
            std::exit(kExitStatus);
        }
        if (p.seq == kClock) {
            timespec now{};
            clock_refused = clock_gettime(CLOCK_REALTIME, &now) != 0;
        }
#if defined(BOX_PROBE_IMPORT_PATH_OPEN) || defined(BOX_PROBE_IMPORT_ENV)
        if (p.seq == kReach) {
            (void)box_reach(0);
        }
#endif
#endif
        if (mail.answer(BoxPong{p.seq, state_.count, memory_refused, clock_refused}).valid()) {
            ++state_.answered;
        }
    }
};

} // namespace

ZEN_EXPORT_WEAVE(BoxProbe)
