// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// Scratch probe (READ-ONLY audit): demonstrate that Switchboard::journal_ grows
// without bound — one DeliveryOutcome retained per message ever enqueued, never
// reclaimed — even after the queue is fully drained by pump().
#include <zen/switchboard/switchboard.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdio>
#include <fstream>
#include <string>

static long rss_kb() {
    // VmRSS in kB from /proc/self/status
    std::ifstream f("/proc/self/status");
    std::string k;
    while (f >> k) {
        if (k == "VmRSS:") { long v = 0; f >> v; return v; }
    }
    return -1;
}

int main() {
    using namespace loom;
    auto schema = SchemaBuilder("Probe", 1).field("x", Kind::Int).build();

    Switchboard sb;

    auto send_batch = [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            Value v(schema);
            v.set("x", Cell::integer(static_cast<std::int64_t>(i)));
            // Host root authority: enqueues (journal_.push_back) then pump drains queue_.
            // Target 999 does not exist -> delivered as Refused(NoSuchTarget), recorded in journal_.
            sb.send(WeaveId{999}, Message(std::move(v)));
        }
        sb.pump(); // fully drain queue_; journal_ retains every outcome
    };

    const std::size_t batch = 2'000'000;
    long before = rss_kb();
    send_batch(batch);
    long after1 = rss_kb();
    send_batch(batch);
    long after2 = rss_kb();

    std::printf("queue pending after pump: %zu  (drained)\n", sb.pending());
    std::printf("RSS kB:  start=%ld  after %zu msgs=%ld  after %zu msgs=%ld\n",
                before, batch, after1, 2 * batch, after2);
    std::printf("growth per batch of %zu msgs: batch1=+%ld kB  batch2=+%ld kB\n",
                batch, after1 - before, after2 - after1);
    std::printf("approx bytes/message retained: %.1f\n",
                (double)(after2 - before) * 1024.0 / (double)(2 * batch));

    // Prove retention: the very first delivery's outcome is still queryable
    // (non-Pending) after 4,000,000 later messages.
    DeliveryOutcome first = sb.outcome(Ticket{1});
    std::printf("outcome(seq=1) after %zu msgs: disposition=%d (0=Pending,1=Delivered,2=Refused)\n",
                2 * batch, (int)first.disposition);
    return 0;
}
