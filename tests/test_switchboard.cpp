// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/gate.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace loom;
using namespace loom;
using namespace sbfx;

namespace {
// Shorthand; the real helper lives in the fixtures.
Registered reg(Switchboard& bus, std::vector<std::shared_ptr<const Schema>> accept,
               std::int64_t max_reloads = 2, bool revive_from_last_good = true) {
    return register_probe(bus, std::move(accept), max_reloads, revive_from_last_good);
}
} // namespace

TEST_SUITE("switchboard") {

TEST_CASE("registration records the accept-set and is queryable") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema(), greet_schema()});
    REQUIRE(bus.list_weaves().size() == 1);
    CHECK(bus.list_weaves()[0] == r.id);
    CHECK(bus.accepted_schemas(r.id).size() == 2);
    CHECK(bus.weave(r.id) != nullptr);
    CHECK(bus.alive(r.id));
}

TEST_CASE("a directed send to an accepting target is delivered and gated") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    Ticket t = bus.send(r.id, Message(ping(7)));
    CHECK(bus.outcome(t).disposition == Disposition::Pending); // deferred until pump
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);
    REQUIRE(r.weave->handled_values.size() == 1);
    CHECK(r.weave->handled_values[0] == 7);
}

TEST_CASE("a send whose shape the target does not accept is refused, handler untouched") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    Ticket t = bus.send(r.id, Message(greet("hi")));
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NotAccepted);
    CHECK(r.weave->handled_names.empty());
}

TEST_CASE("a directed send to an unknown target is refused") {
    Switchboard bus;
    Ticket t = bus.send(WeaveId{9999}, Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

TEST_CASE("the delivery journal is a bounded ring: recent outcomes survive, ancient ones are evicted") {
    // Audit F-6: journal_ used to retain one outcome per message EVER enqueued — a
    // linear leak in lifetime throughput, and a bus is precisely the component that
    // runs for weeks. It is now a fixed-capacity ring of kJournalCapacity slots.
    // Flood far past the bound and show the journal keeps a recent window intact (no
    // live correlation lost) while the footprint stays bounded (an out-of-window ticket
    // has been evicted — the journal did NOT keep one slot per message sent).
    Switchboard bus;
    constexpr std::uint64_t cap = Switchboard::kJournalCapacity;
    const std::uint64_t flood = cap * 3; // three windows' worth of traffic

    Ticket first{}, recent{}, edge_in{}, edge_out{};
    for (std::uint64_t i = 0; i < flood; ++i) {
        Ticket t = bus.send(WeaveId{9999}, Message(ping(static_cast<std::int64_t>(i)))); // no such target
        const std::uint64_t seq = i + 1; // seqs start at 1
        if (seq == 1) first = t;                     // ancient: 3x the window old by the end
        if (seq == flood - cap) edge_out = t;        // one slot past the retained window
        if (seq == flood - cap + 1) edge_in = t;     // the oldest slot still inside the window
        recent = t;
        bus.pump(); // drain fully each time — the leak was in the journal, not the queue
    }

    // In-window tickets still report their true fate — correlation is preserved.
    CHECK(bus.outcome(recent).disposition == Disposition::Refused);
    CHECK(bus.outcome(recent).refusal.reason == RefusalReason::NoSuchTarget);
    CHECK(bus.outcome(edge_in).disposition == Disposition::Refused); // oldest retained: kept

    // Out-of-window tickets have been evicted — the observable proof the ring is
    // bounded (it did not retain all `flood` outcomes). An evicted seq reads Pending,
    // exactly as an unknown seq does.
    CHECK(bus.outcome(edge_out).disposition == Disposition::Pending); // one past the window
    CHECK(bus.outcome(first).disposition == Disposition::Pending);    // ancient
}

TEST_CASE("publish reaches every accepter in registration order; non-accepters get nothing") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {ping_schema()});
    Registered c = reg(bus, {greet_schema()});

    std::size_t recipients = bus.publish(Message(ping(5)));
    CHECK(recipients == 2);
    bus.pump();
    CHECK(a.weave->handled_values.size() == 1);
    CHECK(b.weave->handled_values.size() == 1);
    CHECK(c.weave->handled_values.empty());
}

TEST_CASE("publish with zero accepters is legal: recipient count 0, no delivery") {
    Switchboard bus;
    reg(bus, {ping_schema()});
    std::size_t recipients = bus.publish(Message(tick(1)));
    CHECK(recipients == 0);
    bus.pump();
    CHECK(bus.pending() == 0);
}

TEST_CASE("a fixed sequence of sends is delivered FIFO, reproducibly") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    for (std::int64_t i = 1; i <= 5; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();
    CHECK(r.weave->handled_values == std::vector<std::int64_t>{1, 2, 3, 4, 5});
}

TEST_CASE("a handler that sends during handling causes a later delivery, never a nested one") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {pong_schema()});

    struct Reentry {
        int depth = 0;
        int max = 0;
    } re;
    auto track = [&re] {
        ++re.depth;
        re.max = std::max(re.max, re.depth);
        --re.depth;
    };

    a.weave->on_handle = [&re, bid = b.id](const Message&, Bus& bus_, ProbeWeave&) {
        ++re.depth;
        re.max = std::max(re.max, re.depth);
        bus_.send(bid, Message(pong(99))); // enqueues; must not deliver now
        --re.depth;
    };
    b.weave->on_handle = [&track](const Message&, Bus&, ProbeWeave&) { track(); };

    bus.send(a.id, Message(ping(1)));
    bus.pump();

    CHECK(re.max == 1); // delivery never nested
    REQUIRE(b.weave->handled_values.size() == 1);
    CHECK(b.weave->handled_values[0] == 99); // the during-handle send was delivered, later
}

TEST_CASE("the live delivery path funnels through the same gate as persistence") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()}); // pure recorder: handler enqueues nothing

    const auto g0 = gate_invocations();
    Ticket t = bus.send(r.id, Message(ping(1)));
    bus.pump();
    const auto g1 = gate_invocations();
    CHECK(g1 == g0 + 1); // exactly one validator call for one delivery
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);

    // The persistence (bytes) path advances the very same global counter.
    std::string bytes = bus.snapshot_bytes(r.id);
    const auto g2 = gate_invocations();
    ReviveOutcome ro = bus.reload(r.id, bytes);
    const auto g3 = gate_invocations();
    CHECK(ro.revived);
    CHECK(g3 > g2);
}

TEST_CASE("an observer taps deliveries and refusals without being a recipient") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    bus.send(r.id, Message(ping(1)));          // delivered
    bus.send(r.id, Message(greet("x")));       // refused: NotAccepted
    bus.send(r.id, Message(malformed_ping())); // refused: GateRefused (MissingField)
    bus.pump();

    REQUIRE(tap.size() == 3);
    CHECK(tap[0].kind == EventKind::Delivered);
    CHECK(tap[1].kind == EventKind::Refused);
    CHECK(tap[1].reason == RefusalReason::NotAccepted);
    CHECK(tap[2].kind == EventKind::Refused);
    CHECK(tap[2].reason == RefusalReason::GateRefused);
    CHECK(tap[2].error_kind == ErrorKind::MissingField);
}

TEST_CASE("two weaves declaring the same (name,version) with different shapes conflict") {
    Switchboard bus;
    reg(bus, {ping_schema()});
    auto impostor = SchemaBuilder("Ping", 1).field("seq", Kind::Text).build(); // different shape
    auto bad = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{impostor});
    CHECK_THROWS_AS(bus.register_weave(std::move(bad)), loom::SchemaConflict);
}

// ---- R2E-0: event-loop composition ----------------------------------------
//
// The Rule Garden's sharpest seam: a perpetual service (Zengine's Timer paces
// itself inside Drive and enqueues its next Drive before returning) means the
// queue never becomes empty, so a drain-to-empty pump never returns to the
// outer network loop. Both components work as designed; their liveness
// assumptions do not compose. Reproduced here with the same SHAPE as a Timer —
// a weave whose handler re-enqueues its own next turn — without needing Zengine.

// A weave that re-arms itself on every delivery, exactly as a repeating Timer
// does. `stop_after` bounds the reproduction so the test terminates; the seam
// is what happens when nothing bounds it.
struct Perpetual : ProbeWeave {
    explicit Perpetual(std::int64_t stop_after)
        : ProbeWeave({ping_schema()}), stop_after_(stop_after) {}

    void handle(const Message& in, Bus& bus) override {
        ProbeWeave::handle(in, bus);
        if (count < stop_after_) {
            bus.send(self, Message(ping(count))); // the next Drive, enqueued before returning
        }
    }
    WeaveId self{};

private:
    std::int64_t stop_after_;
};

TEST_CASE("R2E-0: a perpetual service starves the outer loop — pump() returns only when the "
          "PRODUCER stops, not when the host wants control back") {
    Switchboard bus;
    auto owned = std::make_unique<Perpetual>(500);
    Perpetual* raw = owned.get();
    const WeaveId id = bus.register_weave(std::move(owned), Grant{}.allow_any());
    raw->self = id;

    bus.send(id, Message(ping(0)));
    bus.pump(); // ONE call

    // 500 deliveries in a single pump: the queue emptied only because the weave
    // chose to stop re-arming. A real Timer never does. This is the starvation.
    CHECK(raw->count == 500);
}

// The six cases that pinned `pump_bounded(n)` were REMOVED WITH THE SURFACE
// (R2E-0a), not because they failed but because the thing they proved correct
// turned out to be the wrong question to ask a host. The number they exercised
// so carefully is the number the Rule Garden could not pick: 64 made its live
// round-trip 17x slower, and a budget large enough not to throttle was
// drain-to-empty again. Every property they pinned that still has a public
// surface — FIFO exactness across a turn boundary, honouring `stop()`, the
// empty-queue no-op, non-reentrancy — is pinned below against `pump_pending()`,
// which is the only bounded turn Loom now offers. The experiment itself is kept
// as history in `docs/decisions/`, not as API nobody wanted.

TEST_CASE("R2E-0: pump() itself is unchanged — still drain-to-empty for every existing caller") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    for (std::int64_t i = 1; i <= 5; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    CHECK(bus.pending() == 5);

    bus.pump(); // the pre-R2E-0 contract, untouched by either bounded turn
    CHECK(r.weave->count == 5);
    CHECK(bus.pending() == 0);
}

TEST_CASE("R2E-0: pump_pending dispatches exactly the backlog present at entry — a self-re-arming "
          "producer cannot extend the turn, and a busy bus is not throttled") {
    Switchboard bus;
    auto owned = std::make_unique<Perpetual>(1'000'000); // never stops
    Perpetual* raw = owned.get();
    const WeaveId id = bus.register_weave(std::move(owned), Grant{}.allow_any());
    raw->self = id;

    // A REAL BACKLOG plus a perpetual producer: 20 waiting, and the first one
    // handled re-arms the producer forever.
    for (int i = 0; i < 20; ++i) {
        bus.send(id, Message(ping(i)));
    }
    CHECK(bus.pending() == 20);

    // Exactly the 20 that were waiting. The 20 continuations they enqueued are
    // NOT part of this turn — that is what bounds it.
    CHECK(bus.pump_pending() == 20);
    CHECK(raw->count == 20);
    CHECK(bus.pending() == 20); // the next turn's backlog, already waiting

    // ...and the next turn takes exactly that, forever, without ever being asked
    // for a number. This is what a caller-supplied budget cannot do without the
    // host knowing the producer's rate: too small throttles, too large starves.
    CHECK(bus.pump_pending() == 20);
    CHECK(raw->count == 40);
}

TEST_CASE("R2E-0: pump_pending on a quiet bus is a no-op, and drains a finite backlog whole") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});

    CHECK(bus.pump_pending() == 0); // nothing waiting; no spin, no block

    for (std::int64_t i = 1; i <= 5; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    // A finite backlog with no re-arming producer clears in ONE turn — the
    // throughput a fixed budget would have capped.
    CHECK(bus.pump_pending() == 5);
    CHECK(bus.pending() == 0);
    const std::vector<std::int64_t> expected{1, 2, 3, 4, 5};
    CHECK(r.weave->handled_values == expected);
}

TEST_CASE("R2E-0: pump_pending keeps FIFO exact and honours stop()") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    r.weave->on_handle = [&bus](const Message& in, Bus&, ProbeWeave&) {
        if (in.payload.get("seq")->as_int() == 3) {
            bus.stop();
        }
    };
    for (std::int64_t i = 1; i <= 6; ++i) {
        bus.send(r.id, Message(ping(i)));
    }

    CHECK(bus.pump_pending() == 3); // stopped at the third, and says so
    CHECK(bus.pending() == 3);
    CHECK(bus.pump_pending() == 3);
    const std::vector<std::int64_t> expected{1, 2, 3, 4, 5, 6};
    CHECK(r.weave->handled_values == expected);
}

TEST_CASE("R2E-0: the bounded turn is non-reentrant, exactly as pump() is") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    std::size_t nested = 1; // a sentinel the handler must overwrite
    r.weave->on_handle = [&bus, &nested](const Message&, Bus&, ProbeWeave&) {
        nested = bus.pump_pending(); // must dispatch nothing from inside a delivery
    };
    bus.send(r.id, Message(ping(1)));
    bus.send(r.id, Message(ping(2)));

    // Both were waiting at entry, so both belong to this turn — and the nested
    // call inside the first handler dispatched nothing rather than re-entering.
    CHECK(bus.pump_pending() == 2);
    CHECK(nested == 0);
    CHECK(r.weave->count == 2);
}

// ---------------------------------------------------------------------------
// STF-1 — the native callback boundary (MSG-10).
//
// Loom calls two kinds of code it did not write: a native `Weave::handle`, and a
// host observer. Both may throw. These cases pin what Loom owes the host when
// one does — the exception itself is the host's business, the bookkeeping is
// Loom's.
// ---------------------------------------------------------------------------

TEST_CASE("STF-1: a native handler that throws through pump() leaves no poisoned dispatch") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {pong_schema()});
    a.weave->on_handle = [bid = b.id](const Message&, Bus& bus_, ProbeWeave&) {
        bus_.send(bid, Message(pong(7))); // enqueued BEFORE the failure
        throw std::runtime_error("native handler failure");
    };

    const Ticket doomed = bus.send(a.id, Message(ping(1)));
    bus.send(b.id, Message(pong(2)));
    REQUIRE(bus.pending() == 2);

    // 1. THE EXCEPTION REACHES THE HOST. Not swallowed, not translated into a
    //    refusal, not turned into a process abort.
    CHECK_THROWS_AS(bus.pump(), std::runtime_error);
    CHECK(a.weave->count == 1); // the handler did run

    // 2. The failed message was consumed — it is not silently retried — and what
    //    was queued behind it is untouched. The handler's own send, made before
    //    it threw, is queued too: there is no rollback, and none is claimed.
    CHECK(bus.pending() == 2);

    // 3. THE FAILED DELIVERY GETS NO OUTCOME. Its ticket reads Pending — not
    //    Delivered, not Refused — because STF-1 restores state rather than
    //    inventing a disposition for a delivery that neither finished nor was
    //    refused. The QUEUE is what says the message was consumed.
    CHECK(bus.outcome(doomed).disposition == Disposition::Pending);

    // 4. DISPATCH IS NOT POISONED. Without the restore this pump would return
    //    immediately, believing itself reentrant, and the backlog would never
    //    move again.
    bus.pump();
    CHECK(bus.pending() == 0);
    CHECK(b.weave->handled_values == std::vector<std::int64_t>{2, 7});
    CHECK(bus.outcome(doomed).disposition == Disposition::Pending); // still, and forever

    // 5. ...and the bus is an ordinary working bus afterwards.
    const Ticket later = bus.send(b.id, Message(pong(3)));
    bus.pump();
    CHECK(bus.outcome(later).disposition == Disposition::Delivered);
    CHECK(b.weave->handled_values.size() == 3);
}

TEST_CASE("STF-1: a native handler that throws through pump_pending() leaves no poisoned dispatch") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {pong_schema()});
    a.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
        throw std::runtime_error("native handler failure");
    };

    bus.send(a.id, Message(ping(1)));
    bus.send(b.id, Message(pong(2)));

    CHECK_THROWS_AS(bus.pump_pending(), std::runtime_error);
    CHECK(bus.pending() == 1);

    // The backlog-at-entry turn is serviceable again, and still bounded by the
    // queue as it stands NOW — one envelope, dispatched.
    CHECK(bus.pump_pending() == 1);
    REQUIRE(b.weave->handled_values.size() == 1);
    CHECK(b.weave->handled_values[0] == 2);

    // And pump() is equally unpoisoned by a pump_pending() that threw.
    bus.send(b.id, Message(pong(4)));
    bus.pump();
    CHECK(b.weave->handled_values.size() == 2);
}

namespace {
/// The whole authenticated-readiness staging, native and .so-free: an operator,
/// a coordinator, an incumbent holding the production role, and a candidate
/// sealed to the coordinator. Used to witness DELIVERY-SCOPED authority, which
/// is otherwise reachable only from inside a handler.
struct ReadinessStage {
    Switchboard bus;
    Registered op = register_probe(bus, {ping_schema()});
    Registered coordinator = register_probe(bus, {pong_schema()});
    WeaveId incumbent{};
    Registered cand = register_probe(bus, {ping_schema()});
    TxnId txn{};

    ReadinessStage() {
        auto held = std::make_unique<ProbeWeave>(
            std::vector<std::shared_ptr<const Schema>>{ping_schema()});
        incumbent = bus.register_weave(std::move(held), Grant{}.allow_any(), std::string("service"));
        REQUIRE(bus.seal_weave(cand.id, coordinator.id));
        const TxnResult begun =
            bus.begin_prepared_replacement(op.id, coordinator.id, incumbent, cand.id, "service", 8);
        REQUIRE(begun.ok);
        txn = begun.id;
        // The candidate answers its one preparation ask authentically.
        cand.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
            (void)b.answer(Message(pong(1)));
        };
    }

    void ask() { REQUIRE(bus.ask_candidate_to_prepare(txn, Message(ping(7))).ok); }
};
} // namespace

TEST_CASE("STF-1: a callback that throws leaves no delivery-scoped authority behind") {
    ReadinessStage s;

    // The readiness gate reads the delivery Loom is dispatching RIGHT NOW. Its
    // documented contract is that offering from outside a delivery refuses. An
    // escaped exception must not turn the coordinator's finished delivery into a
    // standing right to declare its candidate ready.
    SUBCASE("the handler itself throws") {
        s.coordinator.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
            throw std::runtime_error("coordinator failure mid-readiness");
        };
        s.ask();
        CHECK_THROWS_AS(s.bus.pump(), std::runtime_error);
    }

    SUBCASE("an observer throws while the coordinator's delivery is live") {
        // A second answer has no authority left, so it refuses SYNCHRONOUSLY and
        // taps the refusal — which is how an observer gets to run while the
        // delivery's ambient authority is still set.
        s.coordinator.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
            (void)b.answer(Message(pong(2))); // spends this delivery's one answer
            (void)b.answer(Message(pong(3))); // refused now, on the spot
        };
        s.bus.add_observer([](const BusEvent& e) {
            if (e.kind == EventKind::Refused) {
                throw std::runtime_error("observer failure");
            }
        });
        s.ask();
        CHECK_THROWS_AS(s.bus.pump(), std::runtime_error);
    }

    const TxnResult late = s.bus.accept_preparation_answer(s.txn, PreparationAnswer::Ready);
    CHECK_FALSE(late.ok);
    CHECK(late.why == TxnReason::InvalidReadiness);
    CHECK(s.bus.transaction_state(s.txn) == TxnState::Preparing);
    CHECK(s.bus.role_holder("service") == s.incumbent); // and nothing moved
}

TEST_CASE("STF-1: an already-minted deferred answer survives a handler that later throws") {
    Switchboard bus;
    Registered asker = reg(bus, {pong_schema()});
    Registered responder = reg(bus, {ping_schema()});

    responder.weave->on_handle = [](const Message&, Bus& b, ProbeWeave& self) {
        self.pending = b.make_deferred_answer(); // a DURABLE capability, deliberately taken
        throw std::runtime_error("failed after deferring");
    };

    bus.send_as(asker.id, responder.id, Message(ping(1), asker.id, asker.id, 0));
    CHECK_THROWS_AS(bus.pump(), std::runtime_error);

    // AMBIENT authority is delivery-scoped and gone. A capability the handler
    // deliberately minted is neither, and STF-1 does not revoke it: the answer is
    // still spendable from a later delivery, exactly as R2B-2 defines it.
    REQUIRE(responder.weave->pending.valid());
    responder.weave->on_handle = [](const Message&, Bus& b, ProbeWeave& self) {
        (void)b.spend_deferred(self.pending, Message(pong(9)));
    };
    bus.send(responder.id, Message(ping(2)));
    bus.pump();
    REQUIRE(asker.weave->handled_values.size() == 1);
    CHECK(asker.weave->handled_values[0] == 9);
}

TEST_CASE("STF-1: an observer that throws propagates, and later observation still happens") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {pong_schema()});
    int seen = 0;
    int behind = 0;
    bool armed = true;
    bus.add_observer([&seen, &armed](const BusEvent&) {
        ++seen;
        if (armed) {
            armed = false;
            throw std::runtime_error("observer failure");
        }
    });
    bus.add_observer([&behind](const BusEvent&) { ++behind; });

    bus.send(a.id, Message(ping(1)));
    bus.send(b.id, Message(pong(2)));
    CHECK_THROWS_AS(bus.pump(), std::runtime_error);
    CHECK(seen == 1);
    CHECK(a.weave->count == 1); // the delivery itself completed
    CHECK(bus.pending() == 1);  // the message behind it is still queued

    // ORDINARY C++ PROPAGATION, PRESERVED: notification stops at the throwing
    // observer. Continuing past it would swallow the exception for the observers
    // behind it, and Loom has no error channel to report that on.
    CHECK(behind == 0);

    bus.pump();
    CHECK(b.weave->handled_values.size() == 1); // later dispatch succeeds
    CHECK(seen == 2);                           // ...and so does later observation
    CHECK(behind == 1);
}

// ---------------------------------------------------------------------------
// Observer notification under subscription mutation.
// ---------------------------------------------------------------------------

TEST_CASE("STF-1: an observer added during notification joins at the NEXT event") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    int early = 0;
    int late = 0;
    bool added = false;
    bus.add_observer([&](const BusEvent&) {
        ++early;
        if (!added) {
            added = true;
            bus.add_observer([&late](const BusEvent&) { ++late; });
        }
    });

    bus.send(r.id, Message(ping(1)));
    bus.pump();
    CHECK(early == 1);
    CHECK(late == 0); // the newcomer did not join the event it was added during

    bus.send(r.id, Message(ping(2)));
    bus.pump();
    CHECK(early == 2);
    CHECK(late == 1); // ...and does receive the next one
}

TEST_CASE("STF-1: an observer may remove itself while being notified") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    ObserverId self{};
    int calls = 0;
    int after = 0;
    self = bus.add_observer([&](const BusEvent&) {
        ++calls;
        bus.remove_observer(self);
    });
    bus.add_observer([&after](const BusEvent&) { ++after; });

    bus.send(r.id, Message(ping(1)));
    bus.pump();
    CHECK(calls == 1);
    CHECK(after == 1); // the observer behind the self-removing one still ran

    bus.send(r.id, Message(ping(2)));
    bus.pump();
    CHECK(calls == 1); // never again
    CHECK(after == 2);
}

TEST_CASE("STF-1: removing another observer during notification takes effect at once") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    int victim_calls = 0;
    int survivor_calls = 0;
    ObserverId victim{};
    bool cut = false;

    bus.add_observer([&](const BusEvent&) {
        if (!cut) {
            cut = true;
            bus.remove_observer(victim); // registered AFTER us: not yet notified
        }
    });
    victim = bus.add_observer([&victim_calls](const BusEvent&) { ++victim_calls; });
    bus.add_observer([&survivor_calls](const BusEvent&) { ++survivor_calls; });

    bus.send(r.id, Message(ping(1)));
    bus.pump();

    // REMOVAL IS IMMEDIATE, and that is the point: `remove_observer` is how the
    // console and the bridge stop a callback before the object it captures dies.
    // A notification that ran the removed callback anyway would be a use-after-
    // free, so removal wins over the snapshot.
    CHECK(victim_calls == 0);
    CHECK(survivor_calls == 1); // and the traversal was not derailed

    bus.send(r.id, Message(ping(2)));
    bus.pump();
    CHECK(victim_calls == 0);
    CHECK(survivor_calls == 2);
}

TEST_CASE("STF-1: observer notification survives reallocation of the observer list") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    int first = 0;
    int second = 0;
    int newcomers = 0;
    bool grown = false;
    bus.add_observer([&](const BusEvent&) {
        ++first;
        if (grown) {
            return;
        }
        grown = true;
        // Far past any small-vector capacity: the live container is guaranteed
        // to reallocate while an iteration is walking it.
        for (int i = 0; i < 64; ++i) {
            bus.add_observer([&newcomers](const BusEvent&) { ++newcomers; });
        }
    });
    // REGISTERED SECOND, so the event's view still owes it a notification after
    // the reallocation. Reaching it through the container the first observer just
    // grew is precisely the dangling read; reaching it through the view is not.
    bus.add_observer([&second](const BusEvent&) { ++second; });

    bus.send(r.id, Message(ping(1)));
    bus.pump();
    CHECK(first == 1);
    CHECK(second == 1);
    CHECK(newcomers == 0); // this event's view was fixed before they existed

    bus.send(r.id, Message(ping(2)));
    bus.pump();
    CHECK(first == 2);
    CHECK(second == 2);
    CHECK(newcomers == 64); // ...and all of them are in the next one
}

TEST_CASE("STF-1: an observer may mutate the observer list and then throw") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    ObserverId victim{};
    int victim_calls = 0;
    int newcomer_calls = 0;
    bool armed = true;

    bus.add_observer([&](const BusEvent&) {
        if (!armed) {
            return;
        }
        armed = false;
        bus.remove_observer(victim);
        bus.add_observer([&newcomer_calls](const BusEvent&) { ++newcomer_calls; });
        throw std::runtime_error("observer failure after mutating");
    });
    victim = bus.add_observer([&victim_calls](const BusEvent&) { ++victim_calls; });

    bus.send(r.id, Message(ping(1)));
    CHECK_THROWS_AS(bus.pump(), std::runtime_error);
    CHECK(victim_calls == 0);
    CHECK(newcomer_calls == 0);

    // The mutations stand — they were not a transaction — and the NEXT event uses
    // the updated set.
    bus.send(r.id, Message(ping(2)));
    bus.pump();
    CHECK(victim_calls == 0);
    CHECK(newcomer_calls == 1);
}

TEST_CASE("STF-1: a nested event takes its own observer view") {
    Switchboard bus;
    Registered doomed = reg(bus, {ping_schema()});
    Registered other = reg(bus, {pong_schema()});
    std::vector<EventKind> outer;
    int nested_only = 0;
    bool nested = false;

    bus.add_observer([&](const BusEvent& e) {
        outer.push_back(e.kind);
        if (nested || e.kind != EventKind::Died) {
            return;
        }
        nested = true;
        // An observer registered DURING the outer event is absent from it and
        // present in the event this callback itself causes.
        bus.add_observer([&nested_only](const BusEvent&) { ++nested_only; });
        bus.kill(other.id); // a second, nested emission
    });

    bus.kill(doomed.id);
    REQUIRE(outer.size() == 2); // the outer Died, then the nested one
    CHECK(outer[0] == EventKind::Died);
    CHECK(outer[1] == EventKind::Died);
    CHECK(nested_only == 1); // the newcomer saw the nested event, not the outer one
}

} // TEST_SUITE
