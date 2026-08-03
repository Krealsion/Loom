// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// Independent verifier probe for EQD-1: does Switchboard::journal_ grow without
// bound (one retained DeliveryOutcome per enqueued delivery, never reclaimed)?
//
// Strategy: send host-authority messages to a NON-EXISTENT target. Each send()
// -> enqueue_directed -> journal_.push_back; pump() -> deliver_one -> NoSuchTarget
// -> record(Refused). Queue drains fully each time (proving the growth is the
// journal, not the queue). We capture the very first Ticket and re-read its
// outcome after millions of later sends to prove unbounded retention.

#include <zen/switchboard/switchboard.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdio>
#include <fstream>
#include <string>

static long rss_kb() {
    // VmRSS from /proc/self/statm: field 2 = resident pages.
    std::ifstream f("/proc/self/statm");
    long size = 0, resident = 0;
    f >> size >> resident;
    long page_kb = 4; // 4 KiB pages
    return resident * page_kb;
}

int main() {
    using namespace loom;

    auto schema = SchemaBuilder("Probe", 1).field("n", Kind::Int).build();

    Switchboard bus;

    auto make_msg = [&]() {
        Value v(schema);
        v.set("n", Cell::integer(1));
        return Message(std::move(v));
    };

    const WeaveId ghost{999999}; // never registered

    std::printf("sizeof(DeliveryOutcome) = %zu\n", sizeof(loom::DeliveryOutcome));
    std::printf("RSS start: %ld kB\n", rss_kb());

    // Capture the first ticket.
    Ticket first = bus.send(ghost, make_msg());

    const long BATCH = 2000000;
    for (long i = 1; i < BATCH; ++i) {
        bus.send(ghost, make_msg());
    }
    bus.pump();
    std::printf("after batch1 (%ld sends): pending=%zu  RSS=%ld kB\n",
                BATCH, bus.pending(), rss_kb());
    DeliveryOutcome o1 = bus.outcome(first);
    std::printf("outcome(first=seq1) after batch1: disposition=%d reason=%d\n",
                (int)o1.disposition, (int)o1.refusal.reason);

    for (long i = 0; i < BATCH; ++i) {
        bus.send(ghost, make_msg());
    }
    bus.pump();
    std::printf("after batch2 (%ld sends): pending=%zu  RSS=%ld kB\n",
                2 * BATCH, bus.pending(), rss_kb());
    DeliveryOutcome o1b = bus.outcome(first);
    std::printf("outcome(first=seq1) after batch2: disposition=%d reason=%d\n",
                (int)o1b.disposition, (int)o1b.refusal.reason);

    return 0;
}
