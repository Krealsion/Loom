// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_WEAVELIB_BOX_PROBE_SHAPES_HPP
#define ZEN_TESTS_WEAVELIB_BOX_PROBE_SHAPES_HPP

// The box probe's vocabulary (box_probe.cpp), shared with the suite that drives it
// (tests/test_box.cpp).

#include <zen/weave.hpp>

#include <cstdint>

namespace box_probe {

/// An ask. A non-negative `seq` is answered; the negative ones below are the probe's hostile
/// acts, which only its WebAssembly build performs.
struct BoxPing {
    std::int64_t seq = 0;
    ZEN_SHAPE(BoxPing, 1, ZEN_FIELD(seq));
};

/// The answer: the ask's `seq`, the deliveries this weave has counted, and whether a request
/// for memory past its box was refused inside it.
struct BoxPong {
    std::int64_t seq = 0;
    std::int64_t count = 0;
    bool memory_refused = false;
    ZEN_SHAPE(BoxPong, 1, ZEN_FIELD(seq), ZEN_FIELD(count), ZEN_FIELD(memory_refused));
};

struct BoxProbeState {
    std::int64_t count = 0;
    std::int64_t answered = 0;
    ZEN_SHAPE(BoxProbeState, 1, ZEN_FIELD(count), ZEN_FIELD(answered));
};

constexpr std::int64_t kTrap = -1;  ///< executes `unreachable`
constexpr std::int64_t kSpin = -2;  ///< never returns
constexpr std::int64_t kGrow = -3;  ///< asks for `kGrowBytes` and answers whether it was refused
constexpr std::int64_t kForge = -4; ///< names bytes outside its own memory to the answer door
constexpr std::int64_t kReach = -5; ///< calls the import a variant adds (BOX_PROBE_IMPORT_*)

constexpr std::uint64_t kGrowBytes = std::uint64_t{512} << 20;

/// The reload budget its lifecycle policy names: distinct from every default, so a box that
/// read another number would quarantine at another trap.
constexpr std::int64_t kMaxReloads = 2;

} // namespace box_probe

#endif // ZEN_TESTS_WEAVELIB_BOX_PROBE_SHAPES_HPP
