// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// RTH-1 (corrected by RTH-1a) — the host-side recorder as VOLATILE WORKING MEMORY.
//
// The cases below are grouped the way the phases argued: first that the bus
// carries the facts a history needs, then that the recorder keeps them truthfully
// in its three windows, then that it is honest about what it has forgotten. What
// it PERSISTS is no longer a question about this component at all — see the
// `logger` suite, which owns durability and never reads a Recorder.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/history/dump.hpp>
#include <zen/history/recorder.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

Registered reg(Switchboard& bus, std::vector<std::shared_ptr<const Schema>> accept) {
    return register_probe(bus, std::move(accept), 2, true, Grant{}.allow_any());
}

/// The one retained record of a shape, or nullptr. Tests read records, never
/// rendered lines — which is the reader contract this phase owes.
const HistoryRecord* only_of(const std::vector<HistoryRecord>& all, const std::string& shape) {
    const HistoryRecord* found = nullptr;
    for (const HistoryRecord& r : all) {
        if (r.shape == shape) {
            REQUIRE(found == nullptr);
            found = &r;
        }
    }
    return found;
}

std::size_t count_of(const std::vector<HistoryRecord>& all, const std::string& shape) {
    std::size_t n = 0;
    for (const HistoryRecord& r : all) {
        n += r.shape == shape ? std::size_t{1} : std::size_t{0};
    }
    return n;
}

/// An operation's shape, for the async-ancestry case: the payload carries the
/// operation identity, exactly as the Builder's four observations do.
std::shared_ptr<const Schema> output_schema() {
    static const auto s = SchemaBuilder("BuildOutput", 1)
                              .field("op", Kind::Int)
                              .field("line", Kind::Text)
                              .build();
    return s;
}
Value output(std::int64_t op, std::string line) {
    Value v(output_schema());
    v.set("op", Cell::integer(op));
    v.set("line", Cell::text(std::move(line)));
    return v;
}

} // namespace

TEST_SUITE("recorder") {

// ---------------------------------------------------------------------------
// The substrate facts (the tap now carries what a history needs)
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: an ordinary delivery becomes a structured record") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    const Ticket t = bus.send(r.id, Message(ping(7)));
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* p = only_of(all, "Ping");
    REQUIRE(p != nullptr);
    CHECK(p->kind == RecordKind::Delivery);
    CHECK(p->outcome == RecordedOutcome::Delivered);
    CHECK(p->seq == t.seq);
    CHECK(p->target == r.id);
    CHECK(p->shape == "Ping");
    CHECK(p->shape_version == 1);
    CHECK(p->refusal == RefusalReason::None);
    // The payload was taken INSIDE the callback and is bytes, not a pointer.
    CHECK(p->payload == PayloadDisposition::Retained);
    CHECK(p->payload_bytes > 0);
    const PayloadLookup body = rec.payload(p->record_seq);
    CHECK(body.state == PayloadState::Retained);
    CHECK(body.shape == "Ping");
    CHECK(!body.bytes.empty());
}

TEST_CASE("RTH-1: the sender is retained, and it is the bus's stamp") {
    Switchboard bus;
    Recorder rec(bus);
    Registered from = reg(bus, {ping_schema()});
    Registered to = reg(bus, {pong_schema()});
    from.weave->on_handle = [&to](const Message&, Bus& b, ProbeWeave&) {
        b.send(to.id, Message(pong(1)));
    };
    bus.send(from.id, Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Pong");
    REQUIRE(p != nullptr);
    CHECK(p->sender == from.id);
    CHECK(p->target == to.id);
}

TEST_CASE("RTH-1: a refusal becomes a structured record, with its reason and its detail") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});

    SUBCASE("a shape the target does not accept") {
        bus.send(r.id, Message(greet("hi")));
        bus.pump();
        const std::vector<HistoryRecord> all_p = rec.snapshot();
        const HistoryRecord* p = only_of(all_p, "Greet");
        REQUIRE(p != nullptr);
        CHECK(p->outcome == RecordedOutcome::Refused);
        CHECK(p->refusal == RefusalReason::NotAccepted);
        // A refusal's payload is never visible at the tap, and the record says so
        // rather than leaving a reader to infer it from an empty field.
        CHECK(p->payload == PayloadDisposition::None);
        CHECK(rec.payload(p->record_seq).state == PayloadState::Absent);
    }
    SUBCASE("a payload the gate refuses keeps the gate's own account of it") {
        bus.send(r.id, Message(malformed_ping()));
        bus.pump();
        const std::vector<HistoryRecord> all_p = rec.snapshot();
        const HistoryRecord* p = only_of(all_p, "Ping");
        REQUIRE(p != nullptr);
        CHECK(p->outcome == RecordedOutcome::Refused);
        CHECK(p->refusal == RefusalReason::GateRefused);
        // The half the console's tap window drops: WHICH FIELD was wrong.
        CHECK(p->refusal_detail.find("seq") != std::string::npos);
    }
}

TEST_CASE("RTH-1: an ask's correlation survives into history") {
    Switchboard bus;
    Recorder rec(bus);
    Registered asker = reg(bus, {pong_schema()});
    Registered service = reg(bus, {ping_schema()});
    service.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.answer(Message(pong(in.payload.get("seq")->as_int())));
    };
    bus.send_as(asker.id, service.id, Message(ping(5), asker.id, asker.id, /*correlation=*/4242));
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* ask = only_of(all, "Ping");
    const HistoryRecord* answer = only_of(all, "Pong");
    REQUIRE(ask != nullptr);
    REQUIRE(answer != nullptr);
    CHECK(ask->correlation == 4242);
    // Loom copies the correlation into the answer door; history shows the pair.
    CHECK(answer->correlation == 4242);
}

TEST_CASE("RTH-1: the role a message was ADDRESSED to survives, beside the resolved recipient") {
    Switchboard bus;
    Recorder rec(bus);
    auto owned = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        ping_schema()});
    const WeaveId holder = bus.register_weave(std::move(owned), Grant{}.allow_any(), "room.light");
    bus.send_to_role("room.light", Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Ping");
    REQUIRE(p != nullptr);
    CHECK(p->addressed_role == "room.light"); // whose door was knocked on
    CHECK(p->target == holder);               // ...and who answered it
    CHECK(p->authored_role.empty());          // a DIFFERENT question: who spoke as whom
}

// ---------------------------------------------------------------------------
// Dispatch ancestry
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: dispatch ancestry is exact for a synchronous chain") {
    Switchboard bus;
    Recorder rec(bus);
    Registered third = reg(bus, {greet_schema()});
    Registered second = reg(bus, {pong_schema()});
    Registered first = reg(bus, {ping_schema()});
    first.weave->on_handle = [&second](const Message&, Bus& b, ProbeWeave&) {
        b.send(second.id, Message(pong(1)));
    };
    second.weave->on_handle = [&third](const Message&, Bus& b, ProbeWeave&) {
        b.send(third.id, Message(greet("deep")));
    };
    const Ticket t = bus.send(first.id, Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* a = only_of(all, "Ping");
    const HistoryRecord* b2 = only_of(all, "Pong");
    const HistoryRecord* c = only_of(all, "Greet");
    REQUIRE(a != nullptr);
    REQUIRE(b2 != nullptr);
    REQUIRE(c != nullptr);
    // The first message was authored by a host outside any dispatch: no parent.
    CHECK(a->dispatch_parent == 0);
    CHECK(a->seq == t.seq);
    CHECK(b2->dispatch_parent == a->seq);
    CHECK(c->dispatch_parent == b2->seq);
}

TEST_CASE("RTH-1: an async observation names the delivery that DRAINED it, not the request") {
    // ASYNC-1's shape, reduced to its essentials: a runner is asked to start an
    // operation, goes home, and publishes what it saw on a LATER beat. The
    // dispatch parent of that observation is the beat — because that is the
    // truth — and the thing that connects it to the request is the operation
    // identity in the payload, which is a different kind of relation entirely.
    Switchboard bus;
    Recorder rec(bus);
    Registered watcher = reg(bus, {output_schema()});
    Registered runner = reg(bus, {ping_schema(), tick_schema()});
    std::int64_t op = 0;
    runner.weave->on_handle = [&op, &watcher](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Ping") {
            op = 17; // custody begins; nothing is published yet
            return;
        }
        if (op != 0) {
            b.send(watcher.id, Message(output(op, "compiling")));
        }
    };
    const Ticket request = bus.send(runner.id, Message(ping(1)));
    bus.pump();
    const Ticket beat = bus.send(runner.id, Message(tick(1)));
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* out = only_of(all, "BuildOutput");
    REQUIRE(out != nullptr);
    CHECK(out->dispatch_parent == beat.seq);
    CHECK(out->dispatch_parent != request.seq); // the fact this field must not claim

    // ...and the semantic relation is intact, in the payload, where it belongs.
    const PayloadLookup body = rec.payload(out->record_seq);
    REQUIRE(body.state == PayloadState::Retained);
    Unverified u = parse(body.bytes);
    Admission a = admit(u, output_schema());
    REQUIRE(a.ok());
    CHECK(a.value().get("op")->as_int() == 17);
}

// ---------------------------------------------------------------------------
// Exceptional dispatch
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: a handler that throws is a recorded fact, not a silence") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    r.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
        throw std::runtime_error("native handler failure");
    };
    const Ticket t = bus.send(r.id, Message(ping(1)));
    CHECK_THROWS_AS(bus.pump(), std::runtime_error); // still the host's exception

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Ping");
    REQUIRE(p != nullptr);
    CHECK(p->outcome == RecordedOutcome::HandlerFailed);
    // ...and Loom still does not say what the delivery DID (MSG-10).
    CHECK(bus.outcome(t).disposition == Disposition::Pending);
    // The payload the handler choked on is retained, which is the thing a maker
    // wants most on this path.
    CHECK(p->payload == PayloadDisposition::Retained);
}

TEST_CASE("RTH-1: a failed handler is a rare fact and is protected from ordinary traffic") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 4;
    Recorder rec(bus, policy);
    Registered thrower = reg(bus, {greet_schema()});
    Registered quiet = reg(bus, {ping_schema()});
    thrower.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
        throw std::runtime_error("boom");
    };
    bus.send(thrower.id, Message(greet("x")));
    CHECK_THROWS_AS(bus.pump(), std::runtime_error);
    for (int i = 0; i < 40; ++i) {
        bus.send(quiet.id, Message(ping(i)));
    }
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* p = only_of(all, "Greet");
    REQUIRE(p != nullptr); // forty later messages did not evict it
    CHECK(held_in(p->held, Held::Protected));
    CHECK(p->outcome == RecordedOutcome::HandlerFailed);
    CHECK(rec.bounds().forgotten > 0); // ...and the shared window did lose things
}

// ---------------------------------------------------------------------------
// Handler timing
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: history records how long a handler held the one mind") {
    Switchboard bus;
    Recorder rec(bus);
    Registered slow = reg(bus, {ping_schema()});
    slow.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
        // Spin on the same clock the bus measures with, so the assertion is a
        // measurement rather than a race against a scheduler.
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2)) {
        }
    };
    bus.send(slow.id, Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Ping");
    REQUIRE(p != nullptr);
    CHECK(p->handler_elapsed_ns >= 2000000u);
    // A duration, never a time: nothing on the record says WHEN.
    CHECK(p->handler_elapsed_ns < 5000000000u);
}

TEST_CASE("RTH-1: a refusal ran no handler and claims no duration") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(greet("no")));
    bus.pump();
    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Greet");
    REQUIRE(p != nullptr);
    CHECK(p->handler_elapsed_ns == 0);
}

// ---------------------------------------------------------------------------
// Retention: classes, payloads, and the two budgets
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: metadata outlives its payload, and says which") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.payload_byte_budget = 64; // room for roughly one Ping
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema()});
    for (int i = 0; i < 10; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    REQUIRE(count_of(all, "Ping") == 10); // every record is here
    const HistoryRecord& oldest = all.front();
    const HistoryRecord& newest = all.back();
    CHECK(oldest.payload == PayloadDisposition::Retained); // it WAS admitted...
    CHECK(rec.payload(oldest.record_seq).state == PayloadState::Evicted); // ...and let go
    CHECK(rec.payload(newest.record_seq).state == PayloadState::Retained);
    CHECK(rec.bounds().payloads_forgotten > 0);
    CHECK(rec.bounds().forgotten == 0); // no METADATA was forgotten
}

TEST_CASE("RTH-1: a payload over the ceiling leaves its metadata standing") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.max_payload_bytes = 4;
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Ping");
    REQUIRE(p != nullptr);
    CHECK(p->payload == PayloadDisposition::TooLarge);
    CHECK(p->payload_bytes > 4); // the size is known even though the bytes are not kept
    CHECK(rec.payload(p->record_seq).state == PayloadState::Declined);
}

TEST_CASE("RTH-1: a shape declared not-retained is counted, never silently dropped") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.rules.push_back(RetentionRule{"Tick", 0, false, false});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema(), tick_schema()});
    for (int i = 0; i < 5; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.send(r.id, Message(ping(1)));
    bus.pump();

    CHECK(count_of(rec.snapshot(), "Tick") == 0);
    CHECK(count_of(rec.snapshot(), "Ping") == 1);
    CHECK(rec.counters().declined_by_policy == 5);
    bool saw = false;
    for (const ShapeTally& t : rec.tallies()) {
        if (t.shape == "Tick") {
            saw = true;
            CHECK(t.observed == 5);
            CHECK(t.recorded == 0);
            CHECK(t.declined == 5);
        }
    }
    CHECK(saw);
}

TEST_CASE("RTH-1: a dedicated window keeps a shape out of the shared budget") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 8;
    policy.rules.push_back(RetentionRule{"Tick", 32, false, true});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema(), tick_schema()});
    for (int i = 0; i < 20; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.pump();
    bus.send(r.id, Message(ping(1)));
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    CHECK(count_of(all, "Tick") == 20); // its own window held all of them
    CHECK(count_of(all, "Ping") == 1);  // ...and did not cost the shared one its space
    CHECK(rec.bounds().forgotten == 0);
}

TEST_CASE("RTH-1: a shape's payloads can be declined while its metadata is kept") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.rules.push_back(RetentionRule{"Tick", 1, true, false});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {tick_schema()});
    bus.send(r.id, Message(tick(1)));
    bus.pump();

    const std::vector<HistoryRecord> all_p = rec.snapshot();
    const HistoryRecord* p = only_of(all_p, "Tick");
    REQUIRE(p != nullptr);
    CHECK(p->payload == PayloadDisposition::NotRetained);
    CHECK(rec.payload(p->record_seq).state == PayloadState::Declined);
}

// ---------------------------------------------------------------------------
// Bounds and forgetting — the four honest answers
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: forgotten, never-recorded and never-observed are three different answers") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 4;
    policy.rules.push_back(RetentionRule{"Tick", 0, false, false});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema(), tick_schema()});

    const Ticket first = bus.send(r.id, Message(ping(0)));
    const Ticket old_declined = bus.send(r.id, Message(tick(0)));
    bus.pump();
    for (int i = 1; i < 12; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();
    const Ticket declined = bus.send(r.id, Message(tick(1)));
    const Ticket recent = bus.send(r.id, Message(ping(99)));
    bus.pump();
    // Queued and NOT pumped: the bus has issued a seq and nothing has dispatched.
    const Ticket queued = bus.send(r.id, Message(ping(100)));

    CHECK(rec.find(first.seq).horizon == Horizon::Forgotten);
    CHECK(rec.find(declined.seq).horizon == Horizon::NotRecorded);
    // THE LIMIT, STATED. Below the horizon the recorder cannot separate "I let it
    // go" from "I never kept it" — the record that would have said so is the
    // thing that is gone, and remembering every declined seq would be exactly the
    // unbounded growth the window exists to prevent. `Forgotten` is the honest
    // answer there: at or below my horizon, and not here.
    CHECK(rec.find(old_declined.seq).horizon == Horizon::Forgotten);
    CHECK(rec.find(recent.seq).horizon == Horizon::Retained);
    REQUIRE(rec.find(recent.seq).record != nullptr);
    CHECK(rec.find(recent.seq).record->seq == recent.seq);
    CHECK(rec.find(queued.seq).horizon == Horizon::Unobserved);
    CHECK(rec.find(999999).horizon == Horizon::Unobserved);

    // AND THE WORD IS NEVER "QUEUED". The recorder answers about what it saw; the
    // queue is a question it does not claim to answer, and `Pending` — which the
    // Switchboard uses for five different facts at once — appears nowhere in its
    // vocabulary.
    CHECK(std::string(name_of(rec.find(queued.seq).horizon)) == "Unobserved");
    for (const HistoryRecord& h : rec.snapshot()) {
        CHECK(h.outcome != RecordedOutcome::None);
        CHECK(std::string(name_of(h.outcome)) != "Pending");
    }
    // ...and the horizon is a number, not a shrug.
    const RecorderBounds b = rec.bounds();
    CHECK(b.forgotten > 0);
    CHECK(b.forgotten_horizon_seq >= first.seq);
    CHECK(b.oldest_retained_seq > b.forgotten_horizon_seq);
    CHECK(b.newest_observed_seq == recent.seq);
}

// ---------------------------------------------------------------------------
// The structural blacklist
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: recorder-internal machinery never enters the recordable universe") {
    Switchboard bus;
    Recorder rec(bus);
    Registered store = reg(bus, {greet_schema()});   // stands in for the recorder's own storage
    Registered ordinary = reg(bus, {ping_schema()});
    rec.blacklist().declare_participant(store.id);
    rec.blacklist().declare_shape("Tick");

    bus.send(store.id, Message(greet("write")));
    bus.send(ordinary.id, Message(ping(1)));
    bus.send(ordinary.id, Message(tick(1))); // refused shape, but blacklisted by name
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    CHECK(count_of(all, "Greet") == 0);
    CHECK(count_of(all, "Tick") == 0);
    CHECK(count_of(all, "Ping") == 1);
    CHECK(rec.counters().declined_internal == 2);
    // It is NOT a retention filter: nothing was counted as policy-declined, and
    // no tally was even opened for the excluded traffic.
    CHECK(rec.counters().declined_by_policy == 0);
    for (const ShapeTally& t : rec.tallies()) {
        CHECK(t.shape != "Greet");
        CHECK(t.shape != "Tick");
    }
}

// ---------------------------------------------------------------------------
// Policy changes
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: a policy change is remembered once, and nothing is published to remember it") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(ping(1)));
    bus.pump();
    const std::size_t before = rec.snapshot().size();
    const std::uint64_t deliveries_before = rec.counters().observed;

    RecorderPolicy next = rec.policy();
    next.rules.push_back(RetentionRule{"Tick", 0, false, false});
    next.recent_capacity = 64;
    rec.apply_policy(next);

    const std::vector<HistoryRecord> all = rec.snapshot();
    CHECK(all.size() == before + 1);
    const HistoryRecord& note = all.back();
    CHECK(note.kind == RecordKind::RecorderPolicy);
    CHECK(held_in(note.held, Held::Protected));
    CHECK(note.note.find("Tick") != std::string::npos);
    CHECK(note.note.find("last_n=0") != std::string::npos);
    CHECK(note.note.find("recent") != std::string::npos);

    // NO BUS RECURSION. Nothing was sent, so nothing was observed, so the change
    // did not manufacture the traffic a recorder exists to watch.
    CHECK(bus.pending() == 0);
    CHECK(rec.counters().observed == deliveries_before);
    bus.pump();
    CHECK(rec.snapshot().size() == before + 1);
}

TEST_CASE("RTH-1: shrinking a window destroys nothing, and says how much is over the bound") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    for (int i = 0; i < 20; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();
    REQUIRE(count_of(rec.snapshot(), "Ping") == 20);

    RecorderPolicy smaller = rec.policy();
    smaller.recent_capacity = 5;
    rec.apply_policy(smaller);

    // PROSPECTIVE: the act of changing the policy released nothing.
    CHECK(count_of(rec.snapshot(), "Ping") == 20);
    CHECK(rec.bounds().forgotten == 0);
    const std::vector<HistoryRecord> after = rec.snapshot();
    CHECK(after.back().note.find("above the new bound") != std::string::npos);

    // ...and the excess drains as new traffic arrives, rather than in a lump
    // nobody asked for.
    bus.send(r.id, Message(ping(100)));
    bus.pump();
    CHECK(count_of(rec.snapshot(), "Ping") <= 5);
    CHECK(rec.bounds().forgotten > 0);
}

// ---------------------------------------------------------------------------
// Lifecycle facts
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: a participant's death is a protected record, not a delivery") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    bus.kill(r.id);

    const std::vector<HistoryRecord> all = rec.snapshot();
    REQUIRE(all.size() >= 1);
    const HistoryRecord& died = all.back();
    CHECK(died.kind == RecordKind::Lifecycle);
    CHECK(held_in(died.held, Held::Protected));
    CHECK(died.target == r.id);
    CHECK(died.seq == 0); // a lifecycle transition is not a delivery and has no seq
    CHECK(died.outcome == RecordedOutcome::None);
}

// ---------------------------------------------------------------------------
// The reader, and the witness that is deliberately not the reader
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1: the dump renders what the reader returns, and is not the reader") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(ping(1)));
    bus.send(r.id, Message(greet("no")));
    bus.pump();

    std::ostringstream out;
    DumpOptions opts;
    opts.payloads = true;
    dump_history(rec, out, opts);
    const std::string text = out.str();
    CHECK(text.find("retained") != std::string::npos);
    CHECK(text.find("Ping") != std::string::npos);
    CHECK(text.find("NotAccepted") != std::string::npos);
    CHECK(text.find("payload Retained") != std::string::npos);
    // Every fact the dump printed came from a record; the recorder returned no
    // strings to get here.
    CHECK(rec.snapshot().size() == 2);
    const std::vector<HistoryRecord> records = rec.snapshot();
    CHECK(render_record(records.front()).find("Ping") != std::string::npos);
}

// ---------------------------------------------------------------------------
// RTH-1a — the last-call store, the recent FIFO, and the correction between them
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: every observed shape keeps its most recent observation, by default") {
    Switchboard bus;
    Recorder rec(bus);
    REQUIRE(rec.policy().default_last_n == 1);
    Registered r = reg(bus, {ping_schema(), tick_schema()});
    bus.send(r.id, Message(ping(1)));
    bus.send(r.id, Message(tick(7)));
    bus.send(r.id, Message(tick(8)));
    bus.pump();

    const Lookup last_tick = rec.last_of("Tick");
    REQUIRE(last_tick.horizon == Horizon::Retained);
    REQUIRE(last_tick.record != nullptr);
    CHECK(last_tick.record->shape == "Tick");
    // The LAST one, not the first: a one-deep slot answers "what was the last of
    // these", which is a different question from "was there ever one".
    CHECK(rec.last_calls_of("Tick").size() == 1);
    CHECK(rec.last_of("Ping").horizon == Horizon::Retained);
    // A shape nobody has mentioned is Unobserved, and that is not the same word.
    CHECK(rec.last_of("Greet").horizon == Horizon::Unobserved);
    CHECK(!rec.observed("Greet"));
    CHECK(rec.observed("Tick"));
}

TEST_CASE("RTH-1a: a heartbeat can leave the recent FIFO and stay fully discoverable") {
    // THE CORRECTION, IN ONE CASE. RTH-1 muted a heartbeat by making it
    // NotRetained, which also made it unfindable. The policy that replaces it says
    // only that four thousand beats are not four thousand pieces of CONTEXT.
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 8;
    policy.rules.push_back(RetentionRule{"Tick", /*last_n=*/1, /*in_recent=*/false,
                                         /*retain_payload=*/false});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema(), tick_schema()});

    for (int i = 0; i < 500; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.pump();
    for (int i = 0; i < 6; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();

    // 1. FIVE HUNDRED BEATS DID NOT FLOOD RECENT CONTEXT.
    const std::vector<HistoryRecord> ctx = rec.recent();
    CHECK(ctx.size() <= 8);
    for (const HistoryRecord& h : ctx) {
        CHECK(h.shape != "Tick");
    }
    // 2. ...AND THE BEAT IS STILL THERE TO BE FOUND.
    const Lookup last = rec.last_of("Tick");
    REQUIRE(last.horizon == Horizon::Retained);
    REQUIRE(last.record != nullptr);
    CHECK(last.record->shape == "Tick");
    CHECK(held_in(last.record->held, Held::LastCall));
    CHECK(!held_in(last.record->held, Held::Recent));
    // 3. AND THE TRAFFIC IS COUNTED, never silent.
    bool saw = false;
    for (const ShapeTally& t : rec.tallies()) {
        if (t.shape == "Tick") {
            saw = true;
            CHECK(t.observed == 500);
            CHECK(t.recorded == 500);
            CHECK(t.last_call_held == 1);
        }
    }
    CHECK(saw);
    // 4. ...and the payload rule is independent of both: metadata kept, bytes not.
    CHECK(last.record->payload == PayloadDisposition::NotRetained);
    CHECK(rec.bounds().payload_bytes < 4096);
}

TEST_CASE("RTH-1a: a rare shape stays discoverable long after it leaves recent context") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 4;
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema(), tick_schema()});

    bus.send(r.id, Message(tick(1))); // the one-off, first and never again
    bus.pump();
    const Lookup while_recent = rec.last_of("Tick");
    REQUIRE(while_recent.horizon == Horizon::Retained);
    CHECK(held_in(while_recent.record->held, Held::Recent));

    for (int i = 0; i < 200; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();

    for (const HistoryRecord& h : rec.recent()) {
        CHECK(h.shape != "Tick"); // long gone from context
    }
    const Lookup after = rec.last_of("Tick");
    REQUIRE(after.horizon == Horizon::Retained); // ...and still answerable
    CHECK(!held_in(after.record->held, Held::Recent));
    CHECK(held_in(after.record->held, Held::LastCall));
}

TEST_CASE("RTH-1a: removing a shape from the FIFO does not make it unrecordable") {
    // The stop condition, stated as an assertion rather than as prose. Two shapes,
    // one muted from context and one silenced outright, and the recorder tells
    // them apart.
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.rules.push_back(RetentionRule{"Tick", 1, false, true});   // muted
    policy.rules.push_back(RetentionRule{"Greet", 0, false, false}); // silenced
    Recorder rec(bus, policy);
    Registered r = reg(bus, {tick_schema(), greet_schema()});
    bus.send(r.id, Message(tick(1)));
    bus.send(r.id, Message(greet("hi")));
    bus.pump();

    CHECK(rec.last_of("Tick").horizon == Horizon::Retained);
    // Observed, and deliberately given no slot. NOT `Unobserved` — the recorder saw
    // it and chose, and those are different facts.
    CHECK(rec.observed("Greet"));
    CHECK(rec.last_of("Greet").horizon == Horizon::NotRecorded);
    CHECK(rec.counters().declined_by_policy == 1);
}

TEST_CASE("RTH-1a: the counters add up, and a policy note is on neither side of it") {
    // `observed == recorded + declined_by_policy + declined_internal`, checkable by
    // hand. A recorder-local note counted as `recorded` would make the history
    // appear to hold more than it was ever shown — which is the arithmetic a
    // reader uses to decide whether to trust the rest.
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.rules.push_back(RetentionRule{"Tick", 0, false, false}); // silenced
    Recorder rec(bus, policy);
    Registered store = reg(bus, {greet_schema()});
    rec.blacklist().declare_participant(store.id);
    Registered r = reg(bus, {ping_schema(), tick_schema()});

    bus.send(store.id, Message(greet("recorder machinery")));
    bus.send(r.id, Message(ping(1)));
    for (int i = 0; i < 3; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.pump();
    RecorderPolicy next = rec.policy();
    next.recent_capacity = 100;
    rec.apply_policy(next); // writes a note, and it is NOT an event

    const RecorderCounters c = rec.counters();
    CHECK(c.observed == 5);
    CHECK(c.recorded == 1);
    CHECK(c.declined_by_policy == 3);
    CHECK(c.declined_internal == 1);
    CHECK(c.observed == c.recorded + c.declined_by_policy + c.declined_internal);
    // ...and the note IS retained, so `retained()` is legitimately one more than
    // `recorded`. Two different questions, and both answers are honest.
    CHECK(rec.retained() == 2);
}

TEST_CASE("RTH-1a: the per-shape last-call depth is a number, and it is honoured") {
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 2;
    policy.rules.push_back(RetentionRule{"Tick", 3, false, true});
    Recorder rec(bus, policy);
    Registered r = reg(bus, {tick_schema()});
    for (int i = 0; i < 10; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.pump();

    const std::vector<HistoryRecord> kept = rec.last_calls_of("Tick");
    REQUIRE(kept.size() == 3); // the last three, oldest first
    CHECK(kept[0].record_seq < kept[1].record_seq);
    CHECK(kept[1].record_seq < kept[2].record_seq);
    CHECK(rec.last_of("Tick").record->record_seq == kept[2].record_seq);
    CHECK(rec.bounds().last_call_held == 3);
    CHECK(rec.bounds().recent_held == 0); // it competes for no context at all
}

TEST_CASE("RTH-1a: one fact, several windows, and the mask says which") {
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(greet("refused"))); // a refusal: structurally protected
    bus.pump();

    const std::vector<HistoryRecord> all = rec.snapshot();
    const HistoryRecord* p = only_of(all, "Greet");
    REQUIRE(p != nullptr); // ONCE, though three windows hold it — there is one of it
    CHECK(held_in(p->held, Held::Protected));
    CHECK(held_in(p->held, Held::Recent));
    CHECK(held_in(p->held, Held::LastCall));
    CHECK(describe_held(p->held) == "LastCall|Recent|Protected");
    const RecorderBounds b = rec.bounds();
    CHECK(b.retained == 1);
    CHECK(b.recent_held == 1);
    CHECK(b.protected_held == 1);
    CHECK(b.last_call_held == 1);
}

TEST_CASE("RTH-1a: protection decides what is KEPT; the shape decides what takes context") {
    // RTH-1 trap 3, restated for two windows instead of one. A muted shape's
    // REFUSALS are still kept — and a storm of them still cannot drown the context
    // a maker came for.
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 4;
    policy.rules.push_back(RetentionRule{"Greet", 0, false, false}); // silenced outright
    Recorder rec(bus, policy);
    Registered r = reg(bus, {ping_schema()});

    for (int i = 0; i < 20; ++i) {
        bus.send(r.id, Message(greet("nobody accepts this"))); // refusals of a muted shape
    }
    bus.pump();
    for (int i = 0; i < 4; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();

    // KEPT: the refusals are structural and the shape rule did not swallow them.
    CHECK(count_of(rec.snapshot(), "Greet") > 0);
    for (const HistoryRecord& h : rec.snapshot_of("Greet")) {
        CHECK(h.outcome == RecordedOutcome::Refused);
        CHECK(held_in(h.held, Held::Protected));
        CHECK(!held_in(h.held, Held::Recent)); // ...and none of them took context
    }
    // ...so recent context is the four Pings, undrowned.
    const std::vector<HistoryRecord> ctx = rec.recent();
    REQUIRE(ctx.size() == 4);
    for (const HistoryRecord& h : ctx) {
        CHECK(h.shape == "Ping");
    }
}

TEST_CASE("RTH-1a: a policy change reseats every shape already being watched") {
    // The hot path caches each shape's resolved rule on first sight, so a policy
    // change has to invalidate what it cached. This is the case that goes red if it
    // does not.
    Switchboard bus;
    Recorder rec(bus);
    Registered r = reg(bus, {tick_schema()});
    bus.send(r.id, Message(tick(1)));
    bus.pump();
    REQUIRE(held_in(rec.last_of("Tick").record->held, Held::Recent));

    RecorderPolicy next = rec.policy();
    next.rules.push_back(RetentionRule{"Tick", 2, false, true});
    rec.apply_policy(next);

    for (int i = 0; i < 5; ++i) {
        bus.send(r.id, Message(tick(i)));
    }
    bus.pump();
    CHECK(rec.last_calls_of("Tick").size() == 2);
    CHECK(!held_in(rec.last_of("Tick").record->held, Held::Recent));
}

} // TEST_SUITE("recorder")
