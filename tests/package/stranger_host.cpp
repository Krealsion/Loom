// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE REAL KERNEL PATH, DRIVEN BY A STRANGER.
//
// Not a custom test loader: loom::Kernel is the actual mechanism, so what is proven
// here is the mechanism a host really uses -- LoadLibrary/dlopen, the "zen_weave_abi"
// lookup by exact name, the descriptor handshake, a live delivery in BOTH directions
// across the C ABI, and unload.
//
// The reply is what makes this more than a load test. A weave that loads but whose
// outbound host callbacks are broken would still pass "it loaded"; only a Pong
// arriving back at a native collector proves the seam carries traffic both ways.

#include "witness_protocol.hpp"

#include <zen/gate.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %-6s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

// A native collector: the reply has to land somewhere real.
struct Seen {
    std::int64_t count;
    std::int64_t last_seq;
    ZEN_SHAPE(Seen, 1, ZEN_FIELD(count), ZEN_FIELD(last_seq));
};

class Collector : public loom::WeaveBase<Collector, Seen, loom::Accept<witness::Pong>> {
public:
    void on(const witness::Pong& p, loom::Mail&) {
        ++state_.count;
        state_.last_seq = p.seq;
    }
};

// Read a live weave's state back through the public path -- snapshot bytes, then the
// same gate every boundary uses. No back channel, and no raw pointer held across a
// bus that owns its participants.
template <class Shape>
Shape state_of(loom::Switchboard& bus, loom::WeaveId id, bool& ok) {
    const std::string bytes = bus.snapshot_bytes(id);
    loom::Unverified u = loom::parse(bytes);
    loom::Admission a = loom::admit(u, loom::schema_of<Shape>());
    ok = a.ok();
    if (!ok) {
        return Shape{};
    }
    return loom::from_value<Shape>(std::move(a).value());
}

} // namespace

int main() {
    std::puts("package witness: real Kernel load/exercise/unload");
    std::printf("  containment: %s\n", loom::Kernel::containment_note());

    loom::Switchboard bus;
    loom::Kernel kernel(bus);

    const loom::WeaveId collector_id = loom::mount<Collector>(bus);

    // ---- load, through the real Kernel ------------------------------------------
    const loom::LoadResult loaded = kernel.load("witness", ZEN_WITNESS_WEAVE);
    check(loaded.ok, "Kernel::load found and accepted the artifact" +
                         (loaded.ok ? std::string() : " -- " + loaded.error));
    if (!loaded.ok) {
        std::puts("package witness: kernel path FAILED");
        return 1;
    }

    // ---- a live delivery, and a reply back across the seam ------------------------
    bus.send(loaded.id,
             loom::Message(loom::to_value(witness::Ping{41}), loom::WeaveId{}, collector_id));
    bus.pump();

    bool seen_ok = false;
    const Seen seen = state_of<Seen>(bus, collector_id, seen_ok);
    check(seen_ok, "the collector's state passes the same gate every boundary uses");
    check(seen.count == 1, "the loaded weave handled a Ping and its reply reached a native weave");
    check(seen.last_seq == 41, "the Pong carried the payload the stranger's weave sent (seq 41)");

    // ---- its own state crossed the ABI too ---------------------------------------
    bool tally_ok = false;
    const witness::Tally tally = state_of<witness::Tally>(bus, loaded.id, tally_ok);
    check(tally_ok, "the loaded weave produced a gate-passing snapshot across the seam");
    check(tally.handled == 1, "the weave's own tally says handled == 1");
    check(tally.raw_total == 41,
          "a ZEN_HIDE field still round-trips as state (hiding governs access, not the wire)");
    check(tally.label == "stranger", "a std::string field crossed the C ABI intact");

    // ---- unload ------------------------------------------------------------------
    check(kernel.unload("witness"), "Kernel::unload released the artifact");
    check(!kernel.unload("witness"), "a second unload is honestly refused (nothing left to release)");

    if (failures != 0) {
        std::printf("package witness: kernel path FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: kernel path PASSED");
    return 0;
}
