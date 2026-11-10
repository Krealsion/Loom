// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// Deliberate role authorship: role-authored provenance.
// MSG-07, MSG-04; docs/laws/messaging-laws.md
// A weave may deliberately author one statement as a role it holds; Loom verifies the membership
// at authorship and carries the role fact as immutable delivery provenance, and merely holding
// the role attaches nothing. Both halves bear load: AUTHORIZATION (the author held R) and INTENT
// (this statement was spoken as R). Every attack is made by an ordinary weave with an ordinary
// grant for the shape it abuses: "the honest API cannot express it" is not "Loom refuses it".

#include "doctest.h"
#include "switchboard_fixtures.hpp"

#include <zen/host/lifecycle_wiring.hpp> // the harness IS a host; a weave is not
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using sbfx::greet;
using sbfx::greet_schema;
using sbfx::ping;
using sbfx::ping_schema;
using sbfx::ProbeWeave;
using sbfx::register_probe;
using sbfx::Registered;

namespace {

// ---- local fixture vocabulary ----------------------------------------------

/// What a recipient can actually know about one delivery, copied out durably:
/// the stamped sender, and the two role facts a handler reads back.
struct Heard {
    std::uint64_t sender = 0;
    std::string schema;
    std::string authored_role; ///< empty = personal speech
    bool from_matchmaker = false;
    bool from_empty_probe = false; ///< authored_from_role("") — must never be true
    bool answers_ask = false;
    bool lifecycle = false;
    std::int64_t attested_sequence = 0;
};

/// A listener that records every delivery with its role facts.
struct Listener {
    std::vector<Heard> heard;

    static Registered mount(Switchboard& bus, std::vector<std::shared_ptr<const Schema>> accept,
                            std::shared_ptr<Listener> log) {
        Registered r = register_probe(bus, std::move(accept));
        r.weave->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
            Heard h;
            h.sender = in.sender.value;
            h.schema = in.payload.schema().name();
            h.authored_role = std::string(in.provenance.authored_role());
            h.from_matchmaker = in.provenance.authored_from_role("matchmaker");
            h.from_empty_probe = in.provenance.authored_from_role("");
            h.answers_ask = in.provenance.answers_ask();
            h.lifecycle = in.provenance.lifecycle_activation();
            h.attested_sequence = in.provenance.attested_sequence();
            log->heard.push_back(std::move(h));
        };
        return r;
    }
};

/// Register a probe bound to a role (register_probe itself binds none).
Registered register_role_probe(Switchboard& bus, const char* role,
                               std::vector<std::shared_ptr<const Schema>> accept,
                               Grant grant = Grant{}.allow_any()) {
    auto owned = std::make_unique<ProbeWeave>(std::move(accept));
    ProbeWeave* raw = owned.get();
    WeaveId id = bus.register_weave(std::move(owned), std::move(grant), std::string(role));
    return {id, raw};
}

/// The tap: every refusal reason and every stamped authorship fact, durably.
struct Tap {
    struct Row {
        EventKind kind{};
        std::uint64_t sender = 0;
        std::string schema;
        RefusalReason reason{};
        std::string authored_role;
    };
    std::vector<Row> rows;

    ObserverId attach(Switchboard& bus) {
        return bus.add_observer([this](const BusEvent& ev) {
            rows.push_back(Row{ev.kind, ev.sender.value, ev.schema_name, ev.refusal.reason,
                               ev.authored_role});
        });
    }

    std::size_t count(RefusalReason reason) const {
        std::size_t n = 0;
        for (const Row& r : rows) {
            if (r.kind == EventKind::Refused && r.reason == reason) {
                ++n;
            }
        }
        return n;
    }
};

Value activation_of(std::int64_t sequence) { return to_value(loom::Activated{sequence}); }

/// Register a probe bound to a set of roles, accepting Ping and Greet and declaring Tick as a
/// claim, so it can be reached, author and claim as each role it holds.
Registered register_set_probe(Switchboard& bus, std::vector<std::string> roles) {
    auto owned = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{ping_schema(), greet_schema()});
    owned->declared_claims = {sbfx::tick_schema()};
    ProbeWeave* raw = owned.get();
    WeaveId id = bus.register_weave(std::move(owned), Grant{}.allow_any(), std::move(roles));
    return {id, raw};
}

std::int64_t claimed_n(const SenseReading& r) {
    return r.value ? r.value->get("n")->as_int() : -1;
}

using Roles = std::vector<std::string>;

} // namespace

TEST_SUITE("role_authorship") {

// ---------------------------------------------------------------------------
// The central hostile case: same WeaveId, same role, same payload shape, same
// destination — different authored standing.
// ---------------------------------------------------------------------------

TEST_CASE("the same holder speaks personally and as the role, and the two arrive distinguishable") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Registered holder = register_role_probe(bus, "matchmaker", {ping_schema()});

    // From inside its own handler — the WeaveBus, the only door a weave has.
    holder.weave->on_handle = [player](const Message&, Bus& b, ProbeWeave&) {
        (void)b.send(player.id, Message(greet("personal")));
        const Ticket role = b.role_send("matchmaker", player.id, Message(greet("role")));
        CHECK(role.valid()); // authored: verified and queued
    };
    bus.send(holder.id, Message(ping(1)));
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 2);
    const Heard& personal = log->heard[0];
    const Heard& role = log->heard[1];
    // Same sender, same shape — the stamp alone cannot tell these apart.
    CHECK(personal.sender == holder.id.value);
    CHECK(role.sender == holder.id.value);
    CHECK(personal.schema == role.schema);
    // The role fact is the ONLY difference, and it is the one that matters.
    CHECK_FALSE(personal.from_matchmaker);
    CHECK(personal.authored_role.empty());
    CHECK(role.from_matchmaker);
    CHECK(role.authored_role == "matchmaker");
    // "No role" can never satisfy a membership question, in either direction.
    CHECK_FALSE(personal.from_empty_probe);
    CHECK_FALSE(role.from_empty_probe);
}

TEST_CASE("wrong role and no role are refused at authorship — visibly, and nothing is queued "
          "or downgraded") {
    Switchboard bus;
    Tap tap;
    tap.attach(bus);
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Registered holder_a = register_role_probe(bus, "roleA", {ping_schema()});
    Registered nobody = register_probe(bus, {ping_schema()});

    holder_a.weave->on_handle = [player](const Message&, Bus& b, ProbeWeave&) {
        // Holder of A asks to speak as B: holding ONE role is not holding another.
        const Ticket t = b.role_send("roleB", player.id, Message(greet("as B")));
        CHECK_FALSE(t.valid());
        const RolePublication p = b.role_publish("roleB", Message(greet("as B")));
        CHECK_FALSE(p.authored);
        CHECK(p.recipients == 0);
    };
    nobody.weave->on_handle = [player](const Message&, Bus& b, ProbeWeave&) {
        // No role at all.
        const Ticket t = b.role_send("roleA", player.id, Message(greet("as A")));
        CHECK_FALSE(t.valid());
        const Ticket via_role = b.role_send_to_role("roleA", "roleA", Message(greet("as A")));
        CHECK_FALSE(via_role.valid());
    };
    bus.send(holder_a.id, Message(ping(1)));
    bus.send(nobody.id, Message(ping(2)));
    bus.drain_until_idle();

    // Nothing arrived — a refused authorship is not a personal send in disguise.
    CHECK(log->heard.empty());
    CHECK(bus.pending() == 0);
    // Every attempt is on the tap with the PRECISE reason: not a grant problem,
    // not a forged capability — the sender does not hold that role.
    CHECK(tap.count(RefusalReason::RoleAuthorshipDenied) == 4);
}

TEST_CASE("a payload role field buys zero provenance") {
    // The forgeable representation, used honestly by its owner: a schema whose
    // payload SAYS role=matchmaker. The delivery fact stays personal, which is
    // the entire difference between data and provenance.
    Switchboard bus;
    auto claim_schema = SchemaBuilder("RoleClaim", 1).field("role", Kind::Text).build();
    Value claim(claim_schema);
    claim.set("role", Cell::text("matchmaker"));

    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {claim_schema}, log);
    Registered forger = register_probe(bus, {ping_schema()});
    forger.weave->on_handle = [player, &claim](const Message&, Bus& b, ProbeWeave&) {
        (void)b.send(player.id, Message(claim));
    };
    bus.send(forger.id, Message(ping(1)));
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 1);
    CHECK_FALSE(log->heard[0].from_matchmaker);
    CHECK(log->heard[0].authored_role.empty());
}

// ---------------------------------------------------------------------------
// Destination is orthogonal to authorship: the four combinations, plus both
// publications.
// ---------------------------------------------------------------------------

TEST_CASE("authorship and destination are orthogonal — all four combinations, and the two roles "
          "never conflate") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    // The dispatcher holds a role AND records — a role-holding listener.
    auto dispatcher_probe = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{greet_schema()});
    ProbeWeave* dispatcher_raw = dispatcher_probe.get();
    WeaveId dispatcher =
        bus.register_weave(std::move(dispatcher_probe), Grant{}.allow_any(), "dispatcher");
    dispatcher_raw->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        Heard h;
        h.sender = in.sender.value;
        h.authored_role = std::string(in.provenance.authored_role());
        log->heard.push_back(std::move(h));
    };

    Registered worker = register_role_probe(bus, "worker.a", {ping_schema()});
    worker.weave->on_handle = [dispatcher](const Message&, Bus& b, ProbeWeave&) {
        // personal -> direct
        (void)b.send(dispatcher, Message(greet("p-direct")));
        // personal -> role destination (the sender HOLDS a role; addressing a
        // role still attaches nothing — MSG-04 is not authorship)
        (void)b.send_to_role("dispatcher", Message(greet("p-role")));
        // as a role -> direct
        CHECK(b.role_send("worker.a", dispatcher, Message(greet("r-direct"))).valid());
        // as a role -> role destination: authored as worker.a, delivered to the
        // dispatcher role. BOTH facts, never conflated.
        CHECK(b.role_send_to_role("worker.a", "dispatcher", Message(greet("r-role"))).valid());
    };
    bus.send(worker.id, Message(ping(1)));
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 4);
    CHECK(log->heard[0].authored_role.empty());           // personal -> direct
    CHECK(log->heard[1].authored_role.empty());           // personal -> role
    CHECK(log->heard[2].authored_role == "worker.a");     // as a role -> direct
    CHECK(log->heard[3].authored_role == "worker.a");     // as a role -> role: the
    // authored role is worker.a; the DESTINATION was the dispatcher role —
    // never "authored as dispatcher".
    for (const Heard& h : log->heard) {
        CHECK(h.sender == worker.id.value);
        CHECK(h.authored_role != "dispatcher");
    }
}

TEST_CASE("publication carries the role fact to every listener; a rogue's same-shaped "
          "publication carries nothing") {
    Switchboard bus;
    auto log_a = std::make_shared<Listener>();
    auto log_b = std::make_shared<Listener>();
    Listener::mount(bus, {greet_schema()}, log_a);
    Listener::mount(bus, {greet_schema()}, log_b);
    Registered worker = register_role_probe(bus, "worker.a", {ping_schema()});
    Registered rogue = register_probe(bus, {ping_schema()});

    worker.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        const RolePublication p = b.role_publish("worker.a", Message(greet("open")));
        CHECK(p.authored);
        CHECK(p.recipients == 2);
        // ...and personal publication remains personal, from the same holder.
        (void)b.publish(Message(greet("chatter")));
    };
    rogue.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        (void)b.publish(Message(greet("open"))); // same shape, no role
    };
    bus.send(worker.id, Message(ping(1)));
    bus.send(rogue.id, Message(ping(2)));
    bus.drain_until_idle();

    for (const auto& log : {log_a, log_b}) {
        REQUIRE(log->heard.size() == 3);
        // The role publication: verifiable by EVERY recipient.
        CHECK(log->heard[0].authored_role == "worker.a");
        // The same holder's personal publication: not role speech.
        CHECK(log->heard[1].authored_role.empty());
        // The rogue's same-shaped publication: not verifiable as the role.
        CHECK(log->heard[2].authored_role.empty());
    }
}

TEST_CASE("an authorized publication with zero recipients is not a refusal, and a refusal is not a "
          "zero-recipient publication") {
    Switchboard bus;
    Tap tap;
    tap.attach(bus);
    // Nobody accepts Greet at all.
    Registered worker = register_role_probe(bus, "worker.a", {ping_schema()});
    Registered rogue = register_probe(bus, {ping_schema()});
    worker.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        const RolePublication p = b.role_publish("worker.a", Message(greet("open")));
        CHECK(p.authored);          // the role spoke...
        CHECK(p.recipients == 0);   // ...to an empty room. Both facts true.
    };
    rogue.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        const RolePublication p = b.role_publish("worker.a", Message(greet("open")));
        CHECK_FALSE(p.authored);    // no role spoke...
        CHECK(p.recipients == 0);   // ...and the 0 means NOTHING fanned out.
    };
    bus.send(worker.id, Message(ping(1)));
    bus.send(rogue.id, Message(ping(2)));
    bus.drain_until_idle();
    // Exactly one of the two attempts was an authorship refusal.
    CHECK(tap.count(RefusalReason::RoleAuthorshipDenied) == 1);
}

// ---------------------------------------------------------------------------
// Role authorship does not widen grants, and composes with every existing
// delivery law rather than bypassing any.
// ---------------------------------------------------------------------------

TEST_CASE("a valid role author with an insufficient ordinary grant is still denied delivery — "
          "the role is not a super-grant") {
    Switchboard bus;
    Tap tap;
    tap.attach(bus);
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    // Holds the role; may emit only Ping. The role send of Greet is
    // legitimately AUTHORED and then refused by the ordinary grant at delivery.
    Registered holder = register_role_probe(bus, "matchmaker", {ping_schema()},
                                            Grant{}.allow_to_any("Ping", 1));

    const Ticket t =
        bus.role_send_as(holder.id, "matchmaker", player.id, Message(greet("match")));
    CHECK(t.valid()); // authorship verified; the delivery was queued...
    bus.drain_until_idle();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::CapabilityDenied); // ...and denied.
    CHECK(log->heard.empty());
    // The tap shows the refused delivery still carried its authored role —
    // the stamp is not erased by an independent refusal, and the reason names
    // the grant, not the authorship.
    REQUIRE(tap.count(RefusalReason::CapabilityDenied) == 1);
    for (const Tap::Row& r : tap.rows) {
        if (r.kind == EventKind::Refused && r.reason == RefusalReason::CapabilityDenied) {
            CHECK(r.authored_role == "matchmaker");
        }
    }
}

TEST_CASE("role speech from a life that ended obeys SenderLifeEnded — role provenance cannot "
          "resurrect dead speech") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Registered holder = register_role_probe(bus, "matchmaker", {ping_schema()});

    const Ticket t =
        bus.role_send_as(holder.id, "matchmaker", player.id, Message(greet("match")));
    REQUIRE(t.valid());
    bus.kill(holder.id); // the author's life ends while the speech is queued
    bus.drain_until_idle();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::SenderLifeEnded);
    CHECK(log->heard.empty());
}

TEST_CASE("a root cannot author role speech — the refusal is visible, not a base-class silence") {
    Switchboard bus;
    Tap tap;
    tap.attach(bus);
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    (void)register_role_probe(bus, "matchmaker", {ping_schema()});

    Bus& as_bus = bus; // the root surface, through the Bus contract it inherits
    CHECK_FALSE(as_bus.role_send("matchmaker", player.id, Message(greet("root"))).valid());
    CHECK_FALSE(as_bus.role_send_to_role("matchmaker", "matchmaker", Message(greet("root")))
                    .valid());
    CHECK_FALSE(as_bus.role_publish("matchmaker", Message(greet("root"))).authored);
    bus.drain_until_idle();
    CHECK(log->heard.empty());
    CHECK(tap.count(RefusalReason::RoleAuthorshipDenied) == 3);
}

// ---------------------------------------------------------------------------
// Copy/replay: observed role provenance cannot be laundered.
// ---------------------------------------------------------------------------

TEST_CASE("re-sending a received role-authored Message — provenance field and all — arrives "
          "as ordinary personal speech") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered second = Listener::mount(bus, {greet_schema()}, log);

    // The launderer receives a legitimate role statement and re-sends the
    // whole public Message representation it observed — the exact struct, the
    // provenance member included.
    Registered launderer = register_probe(bus, {greet_schema()});
    launderer.weave->on_handle = [second](const Message& in, Bus& b, ProbeWeave&) {
        Message copied(in.payload, in.sender, in.reply_to, in.correlation);
        copied.provenance = in.provenance; // keep everything it can see
        (void)b.send(second.id, std::move(copied));
    };

    Registered holder = register_role_probe(bus, "matchmaker", {ping_schema()});
    REQUIRE(bus.role_send_as(holder.id, "matchmaker", launderer.id, Message(greet("real")))
                .valid());
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 1);
    // The re-send is the LAUNDERER'S personal speech: its own stamp, no role.
    CHECK(log->heard[0].sender == launderer.id.value);
    CHECK_FALSE(log->heard[0].from_matchmaker);
    CHECK(log->heard[0].authored_role.empty());
}

// ---------------------------------------------------------------------------
// The authorship moment, and the immutability of the authored fact.
// ---------------------------------------------------------------------------

TEST_CASE("a legitimately authored fact survives a later role move — history is not recomputed "
          "from current topology") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Registered holder = register_role_probe(bus, "matchmaker", {ping_schema()});
    // A successor waits sealed; commit_candidate moves the role without killing
    // or sealing the predecessor — the author stays alive and ordinary.
    Registered coordinator = register_probe(bus, {ping_schema()});
    Registered successor = register_probe(bus, {greet_schema()});
    REQUIRE(bus.seal_weave(successor.id, coordinator.id));

    const Ticket t =
        bus.role_send_as(holder.id, "matchmaker", player.id, Message(greet("match")));
    REQUIRE(t.valid()); // authored while holder held the role
    REQUIRE(bus.commit_candidate(successor.id, holder.id, "matchmaker"));
    REQUIRE(bus.role_holder("matchmaker") == successor.id); // the role has moved
    bus.drain_until_idle();

    // Delivered — and the historical fact is intact: the author DID deliberately
    // speak as matchmaker while it held matchmaker. Current membership is a
    // different question with a different answer.
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);
    REQUIRE(log->heard.size() == 1);
    CHECK(log->heard[0].from_matchmaker);
    CHECK(log->heard[0].sender == holder.id.value);
}

TEST_CASE("a message queued personally does not become role-authored because its sender acquires "
          "the role before delivery") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered coordinator = Listener::mount(bus, {greet_schema(), ping_schema()}, log);
    Registered incumbent = register_role_probe(bus, "service", {ping_schema()});
    // The candidate accepts its activation, and speaks to its coordinator.
    Registered candidate = register_probe(bus, {schema_of<loom::Activated>()});
    REQUIRE(bus.seal_weave(candidate.id, coordinator.id));

    // Schedule the admission FIRST, so it dispatches ahead of the send below.
    const AdmitResult admitted =
        bus.admit_candidate(candidate.id, incumbent.id, "service",
                            host_lifecycle_authority(bus), Message(activation_of(1)), 1);
    REQUIRE(admitted.scheduled);
    // Queued while the candidate holds NO role (it is a sealed candidate);
    // delivered after the admission has made it the service.
    const Ticket personal = bus.send_as(candidate.id, coordinator.id, Message(greet("hello")));
    bus.drain_until_idle();

    REQUIRE(bus.role_holder("service") == candidate.id); // it holds the role now...
    CHECK(bus.outcome(personal).disposition == Disposition::Delivered);
    REQUIRE(!log->heard.empty());
    const Heard& h = log->heard.back();
    CHECK(h.sender == candidate.id.value);
    CHECK(h.authored_role.empty()); // ...and the earlier statement stays personal.
}

// ---------------------------------------------------------------------------
// The replacement matrix: a REAL prepared replacement, end to end.
// ---------------------------------------------------------------------------

TEST_CASE("the role survives its role holder — prepared replacement moves authorship without "
          "conflating identities") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Tap tap;
    tap.attach(bus);

    // The cast: op/coordinator, v1 holding the role, the candidate sealed.
    Registered op = register_probe(bus, {ping_schema()});
    auto ready_schema = SchemaBuilder("Ready", 1).field("txn", Kind::Int).build();
    Registered coordinator = register_probe(bus, {ready_schema});
    Registered v1 = register_role_probe(bus, "service", {ping_schema()});
    Registered cand = register_probe(bus, {schema_of<loom::Activated>(), ping_schema()});
    REQUIRE(bus.seal_weave(cand.id, coordinator.id));

    // v1 role speech accepted.
    REQUIRE(bus.role_send_as(v1.id, "service", player.id, Message(greet("from v1"))).valid());
    bus.drain_until_idle();
    REQUIRE(log->heard.size() == 1);
    CHECK(log->heard[0].sender == v1.id.value);
    CHECK(log->heard[0].authored_role == "service");

    // v2, sealed, cannot author as the production role — refused with the
    // precise reason, before its seal is even consulted.
    CHECK_FALSE(
        bus.role_send_as(cand.id, "service", player.id, Message(greet("from v2"))).valid());
    CHECK(tap.count(RefusalReason::RoleAuthorshipDenied) == 1);

    // The real transaction: begin, converse, become Ready.
    const TxnResult begun = bus.begin_prepared_replacement(op.id, coordinator.id, v1.id, cand.id,
                                                           "service", 8);
    REQUIRE(begun.ok);
    // The candidate answers its preparation ask authentically; the credulous
    // coordinator offers whatever it hears to the bus.
    cand.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Ping") {
            Value ready(SchemaBuilder("Ready", 1).field("txn", Kind::Int).build());
            ready.set("txn", Cell::integer(0));
            (void)b.answer(Message(std::move(ready)));
        }
    };
    const TxnId txn = begun.id;
    coordinator.weave->on_handle = [&bus, txn](const Message&, Bus&, ProbeWeave&) {
        (void)bus.accept_preparation_answer(txn, PreparationAnswer::Ready);
    };
    REQUIRE(bus.ask_candidate_to_prepare(txn, Message(ping(7))).ok);
    bus.drain_until_idle();
    REQUIRE(bus.transaction_state(txn) == TxnState::Ready);

    // v1 remains the role throughout preparation.
    REQUIRE(bus.role_send_as(v1.id, "service", player.id, Message(greet("still v1"))).valid());
    bus.drain_until_idle();
    CHECK(log->heard.back().sender == v1.id.value);
    CHECK(log->heard.back().authored_role == "service");

    // Commit: scheduled, AdmissionPending — nothing moved yet.
    REQUIRE(bus.commit_prepared_replacement(txn, host_lifecycle_authority(bus),
                                            Message(activation_of(41)), 41)
                .ok);
    CHECK(bus.transaction_state(txn) == TxnState::AdmissionPending);
    CHECK(bus.role_holder("service") == v1.id);

    // Dispatch the admission: the role moves, and the candidate's first breath
    // is a lifecycle attestation — not role speech, not an answer.
    bus.drain_until_idle();
    REQUIRE(bus.role_holder("service") == cand.id);
    CHECK(bus.transaction_state(txn) == TxnState::Committed);

    // The successor's role speech is accepted; the predecessor's NEW attempt
    // is refused. Identity changed; the role did not.
    REQUIRE(
        bus.role_send_as(cand.id, "service", player.id, Message(greet("from v2"))).valid());
    CHECK_FALSE(
        bus.role_send_as(v1.id, "service", player.id, Message(greet("v1 again"))).valid());
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 3);
    // A strict receiver trusting the ROLE accepts v1's pre-move speech and
    // v2's post-move speech — different senders, the same authored role.
    CHECK(log->heard[0].sender == v1.id.value);
    CHECK(log->heard[2].sender == cand.id.value);
    CHECK(log->heard[0].sender != log->heard[2].sender);
    CHECK(log->heard[0].authored_role == log->heard[2].authored_role);
    CHECK(log->heard[2].authored_role == "service");
}

// ---------------------------------------------------------------------------
// Answer/activation provenance: undamaged, and representable beside the role.
// ---------------------------------------------------------------------------

TEST_CASE("the representation lets conversation provenance and the authored role coexist — "
          "neither fact erases the other") {
    // The type-level pin: Kind and authored_role are separate axes. No public
    // V1 door produces the combination; the representation must still hold it,
    // so a future answer_as_role needs no redesign.
    const Provenance answer_and_role =
        Provenance::attested(Provenance::Kind::Answer, 0).with_authored_role("worker.a");
    CHECK(answer_and_role.answers_ask());
    CHECK(answer_and_role.authored_from_role("worker.a"));
    CHECK_FALSE(answer_and_role.lifecycle_activation());

    const Provenance activation_and_role =
        Provenance::attested(Provenance::Kind::Activation, 7).with_authored_role("worker.a");
    CHECK(activation_and_role.lifecycle_activation());
    CHECK(activation_and_role.attested_sequence() == 7);
    CHECK(activation_and_role.authored_from_role("worker.a"));
    CHECK_FALSE(activation_and_role.answers_ask());

    // And the empty role matches nothing, on every kind.
    CHECK_FALSE(answer_and_role.authored_from_role(""));
    CHECK_FALSE(Provenance{}.authored_from_role(""));
}

TEST_CASE("a role-authored ask earns an ordinary authenticated answer — the answer is an "
          "answer, not role speech") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    // The asker holds a role and asks AS it; it records what comes back.
    auto asker_probe = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{greet_schema(), ping_schema()});
    ProbeWeave* asker_raw = asker_probe.get();
    WeaveId asker = bus.register_weave(std::move(asker_probe), Grant{}.allow_any(), "client");
    asker_raw->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        Heard h;
        h.sender = in.sender.value;
        h.authored_role = std::string(in.provenance.authored_role());
        h.answers_ask = in.provenance.answers_ask();
        log->heard.push_back(std::move(h));
    };
    Registered service = register_role_probe(bus, "service", {greet_schema()});
    service.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        // The service sees the role-authored ask, and answers it.
        CHECK(in.provenance.authored_from_role("client"));
        (void)b.answer(Message(greet("answered")));
    };

    REQUIRE(bus.role_send_to_role_as(asker, "client", "service", Message(greet("ask")))
                .valid());
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 1);
    CHECK(log->heard[0].answers_ask);              // Loom's word: THE answer
    CHECK(log->heard[0].authored_role.empty());    // the ask's role did not leak
    CHECK(log->heard[0].sender == service.id.value);
}

TEST_CASE("a committed activation remains lifecycle provenance — never role speech, even "
          "though the admitted candidate now holds the role") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered coordinator = register_probe(bus, {ping_schema()});
    Registered incumbent = register_role_probe(bus, "service", {ping_schema()});
    auto cand_probe = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>()});
    ProbeWeave* cand_raw = cand_probe.get();
    WeaveId cand = bus.register_weave(std::move(cand_probe), Grant{}.allow_any());
    cand_raw->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        Heard h;
        h.lifecycle = in.provenance.lifecycle_activation();
        h.attested_sequence = in.provenance.attested_sequence();
        h.authored_role = std::string(in.provenance.authored_role());
        h.answers_ask = in.provenance.answers_ask();
        log->heard.push_back(std::move(h));
    };
    REQUIRE(bus.seal_weave(cand, coordinator.id));
    REQUIRE(bus.admit_candidate(cand, incumbent.id, "service", host_lifecycle_authority(bus),
                                Message(activation_of(9)), 9)
                .scheduled);
    bus.drain_until_idle();

    REQUIRE(log->heard.size() == 1);
    CHECK(log->heard[0].lifecycle);
    CHECK(log->heard[0].attested_sequence == 9);
    CHECK(log->heard[0].authored_role.empty()); // Loom's act, nobody's role
    CHECK_FALSE(log->heard[0].answers_ask);
}

// ---------------------------------------------------------------------------
// The weaver tier: the definition-of-done program, verbatim shape.
// ---------------------------------------------------------------------------

struct MatchCreated {
    std::string server;
    ZEN_SHAPE(MatchCreated, 1, ZEN_FIELD(server));
};
struct MakeMatch {
    ZEN_SHAPE(MakeMatch, 1);
};
struct MatchmakerState {
    std::int64_t n = 0;
    ZEN_SHAPE(MatchmakerState, 1, ZEN_FIELD(n));
};

class Matchmaker : public WeaveBase<Matchmaker, MatchmakerState, Accept<MakeMatch>,
                                    Emit<MatchCreated>> {
public:
    Matchmaker(WeaveId player, std::string server)
        : player_(player), server_(std::move(server)) {}
    void on(const MakeMatch&, Mail& mail) {
        // Speaking personally...
        mail.send(player_, MatchCreated{server_});
        // ...and deliberately speaking AS the role, for this one statement.
        mail.as_role("matchmaker").send(player_, MatchCreated{server_});
    }

private:
    WeaveId player_;
    std::string server_;
};

struct PlayerState {
    std::int64_t joined = 0;
    std::int64_t rejected = 0;
    ZEN_SHAPE(PlayerState, 1, ZEN_FIELD(joined), ZEN_FIELD(rejected));
};

class StrictPlayer : public WeaveBase<StrictPlayer, PlayerState, Accept<MatchCreated>, Emit<>> {
public:
    void on(const MatchCreated&, Mail& mail) {
        if (!mail.authored_from_role("matchmaker")) {
            ++state_.rejected;
            return;
        }
        ++state_.joined;
    }
    std::int64_t joined() const { return state_.joined; }
    std::int64_t rejected() const { return state_.rejected; }
};

TEST_CASE("the whole program — a strict player joins only role-authored matches, through Mail "
          "alone") {
    Switchboard bus;
    auto player = std::make_unique<StrictPlayer>();
    StrictPlayer* player_raw = player.get();
    Grant player_grant = emit_default_grant(*player_raw);
    allow_poke_answers(player_grant);
    const WeaveId player_id = bus.register_weave(std::move(player), std::move(player_grant));
    player_raw->zen_set_self(player_id);

    auto maker = std::make_unique<Matchmaker>(player_id, "srv-1");
    Matchmaker* maker_raw = maker.get();
    Grant maker_grant = emit_default_grant(*maker_raw);
    allow_poke_answers(maker_grant);
    const WeaveId maker_id =
        bus.register_weave(std::move(maker), std::move(maker_grant), "matchmaker");
    maker_raw->zen_set_self(maker_id);

    bus.send(maker_id, Message(to_value(MakeMatch{})));
    bus.drain_until_idle();

    // Two identical statements arrived from one weave; the player joined on the
    // role-authored one and rejected the personal one. No Switchboard access,
    // no role lookup, no payload field — the delivery already carried the fact.
    CHECK(player_raw->joined() == 1);
    CHECK(player_raw->rejected() == 1);
}

// ---------------------------------------------------------------------------
// A weave answering to a set of roles (MSG-13): bound at registration, all or none, each role
// still with one holder, and released one at a time by its holder.
// ---------------------------------------------------------------------------

TEST_CASE("a weave holding two roles is reached, authors and claims as each, and is refused as "
          "neither once it holds neither") {
    Switchboard bus;
    auto log = std::make_shared<Listener>();
    Registered player = Listener::mount(bus, {greet_schema()}, log);
    Registered holder = register_set_probe(bus, {"svc.one", "svc.two"});
    CHECK(bus.roles_of(holder.id) == Roles{"svc.one", "svc.two"}); // in binding order
    CHECK(bus.role_holder("svc.one") == holder.id);
    CHECK(bus.role_holder("svc.two") == holder.id);

    // Reached by each name: one weave, two addresses.
    bus.send_to_role("svc.one", Message(ping(1)));
    bus.send_to_role("svc.two", Message(ping(2)));
    bus.drain_until_idle();
    CHECK(holder.weave->handled_values == std::vector<std::int64_t>{1, 2});

    // Authors as each, verified per statement, and the listener can tell them apart.
    REQUIRE(bus.role_send_as(holder.id, "svc.one", player.id, Message(greet("as one"))).valid());
    REQUIRE(bus.role_send_as(holder.id, "svc.two", player.id, Message(greet("as two"))).valid());
    bus.drain_until_idle();
    REQUIRE(log->heard.size() == 2);
    CHECK(log->heard[0].authored_role == "svc.one");
    CHECK(log->heard[1].authored_role == "svc.two");

    // Claims as each, under each role's own key.
    REQUIRE(bus.role_claim_as(holder.id, "svc.one", sbfx::tick(1)));
    REQUIRE(bus.role_claim_as(holder.id, "svc.two", sbfx::tick(2)));
    CHECK(claimed_n(bus.observe_role("svc.one", "Tick", 1)) == 1);
    CHECK(claimed_n(bus.observe_role("svc.two", "Tick", 1)) == 2);
    CHECK(bus.observe_role("svc.two", "Tick", 1).by.role_holder_is_current);

    // Released one at a time: it leaves `svc.one` and keeps `svc.two`.
    CHECK(bus.release_role(holder.id, "svc.one") == RoleRelease::Released);
    CHECK(bus.roles_of(holder.id) == Roles{"svc.two"});
    CHECK_FALSE(bus.role_holder("svc.one").valid());
    CHECK(bus.release_role(holder.id, "svc.one") == RoleRelease::NotHeld); // nothing twice
    CHECK_FALSE(bus.role_send_as(holder.id, "svc.one", player.id, Message(greet("x"))).valid());
    CHECK(bus.role_claim_as(holder.id, "svc.one", sbfx::tick(3)).why == SenseRefusal::RoleNotHeld);
    CHECK(bus.observe_role("svc.one", "Tick", 1).refusal == SenseRefusal::NoClaim); // ended
    CHECK(claimed_n(bus.observe_role("svc.two", "Tick", 1)) == 2); // the other role's stays
    CHECK(bus.observe_role("svc.two", "Tick", 1).by.role_holder_is_current);

    // ...and then the last: it is a weave holding no role, refused as either.
    CHECK(bus.release_role(holder.id, "svc.two") == RoleRelease::Released);
    CHECK(bus.roles_of(holder.id).empty());
    CHECK_FALSE(bus.role_send_as(holder.id, "svc.two", player.id, Message(greet("y"))).valid());
    const Ticket late = bus.send_to_role("svc.two", Message(ping(9)));
    bus.drain_until_idle();
    CHECK(bus.outcome(late).refusal.reason == RefusalReason::NoSuchTarget);
    CHECK(holder.weave->handled_values == std::vector<std::int64_t>{1, 2}); // nothing more
    CHECK(bus.alive(holder.id)); // a release ends a role, never the weave
}

TEST_CASE("a set overlapping a held role is refused whole, and binds none of it") {
    Switchboard bus;
    Registered incumbent = register_role_probe(bus, "shared", {ping_schema()});
    const auto refused = [&bus](Roles roles) {
        auto owned = std::make_unique<ProbeWeave>(
            std::vector<std::shared_ptr<const Schema>>{ping_schema()});
        CHECK_THROWS_AS(bus.register_weave(std::move(owned), Grant{}, std::move(roles)),
                        std::invalid_argument);
    };
    refused({"fresh", "shared"}); // the held name comes second: the first must not be bound
    CHECK_FALSE(bus.role_holder("fresh").valid());
    CHECK(bus.role_holder("shared") == incumbent.id);
    CHECK(bus.roles_of(incumbent.id) == Roles{"shared"});

    refused({"fresh", ""});      // an empty name
    refused({"fresh", "fresh"}); // a name twice
    Roles nine;
    for (int i = 0; i < 9; ++i) {
        nine.push_back("r" + std::to_string(i));
    }
    refused(nine); // past the bound
    CHECK_FALSE(bus.role_holder("fresh").valid());
    CHECK_FALSE(bus.role_holder("r0").valid());

    // The bound itself is a set it may hold.
    nine.pop_back();
    REQUIRE(nine.size() == Switchboard::kMaxRolesPerWeave);
    Registered eight = register_set_probe(bus, nine);
    CHECK(bus.roles_of(eight.id) == nine);
}

TEST_CASE("removal and release each let go of exactly their own roles and those roles' claims") {
    Switchboard bus;
    Registered a = register_set_probe(bus, {"a.one", "a.two"});
    Registered b = register_set_probe(bus, {"b.one"});
    REQUIRE(bus.role_claim_as(a.id, "a.one", sbfx::tick(11)));
    REQUIRE(bus.role_claim_as(a.id, "a.two", sbfx::tick(12)));
    REQUIRE(bus.role_claim_as(b.id, "b.one", sbfx::tick(21)));
    CHECK(bus.retained_claim_count() == 3);

    // A release: one role and its claims, nothing of the holder's other role or of b.
    REQUIRE(bus.release_role(a.id, "a.one") == RoleRelease::Released);
    CHECK(bus.retained_claim_count() == 2);
    CHECK(bus.observe_role("a.one", "Tick", 1).refusal == SenseRefusal::NoClaim);
    CHECK(claimed_n(bus.observe_role("a.two", "Tick", 1)) == 12);
    CHECK(claimed_n(bus.observe_role("b.one", "Tick", 1)) == 21);
    CHECK(bus.release_role(b.id, "a.two") == RoleRelease::NotHeld); // not b's to release
    CHECK(bus.role_holder("a.two") == a.id);

    // A removal: every role it still holds, and their claims; b keeps its own.
    Registered a2 = register_set_probe(bus, {"c.one", "c.two"});
    REQUIRE(bus.role_claim_as(a2.id, "c.one", sbfx::tick(31)));
    REQUIRE(bus.role_claim_as(a2.id, "c.two", sbfx::tick(32)));
    REQUIRE(bus.unregister_weave(a2.id) != nullptr);
    CHECK_FALSE(bus.role_holder("c.one").valid());
    CHECK_FALSE(bus.role_holder("c.two").valid()); // the second goes with the first
    CHECK(bus.observe_role("c.one", "Tick", 1).refusal == SenseRefusal::NoClaim);
    CHECK(bus.observe_role("c.two", "Tick", 1).refusal == SenseRefusal::NoClaim);
    REQUIRE(bus.unregister_weave(a.id) != nullptr);
    CHECK_FALSE(bus.role_holder("a.two").valid());
    CHECK(bus.retained_claim_count() == 1);
    CHECK(claimed_n(bus.observe_role("b.one", "Tick", 1)) == 21);
    CHECK(bus.role_holder("b.one") == b.id);

    // A role let go of is a name another weave may be loaded into.
    Registered next = register_set_probe(bus, {"a.one", "c.two"});
    CHECK(bus.roles_of(next.id) == Roles{"a.one", "c.two"});
}

TEST_CASE("a holder releases its own role from its own delivery, and a host's bus holds none to "
          "release") {
    Switchboard bus;
    Registered holder = register_set_probe(bus, {"keep", "leave"});
    RoleRelease said = RoleRelease::NotOffered;
    holder.weave->on_handle = [&said](const Message&, Bus& b, ProbeWeave&) {
        said = b.release_role("leave");
    };
    bus.send(holder.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(said == RoleRelease::Released);
    CHECK(bus.roles_of(holder.id) == Roles{"keep"});
    CHECK(std::string(name_of(said)) == "Released");

    // The root surface is a host: it holds no role, so it has none to leave, and says so.
    Bus& root = bus;
    CHECK(root.release_role("keep") == RoleRelease::NotOffered);
    CHECK(bus.role_holder("keep") == holder.id);
}

TEST_CASE("a release during a prepared replacement over that weave aborts it, RoleChanged") {
    Switchboard bus;
    Registered op = register_probe(bus, {ping_schema()});
    Registered coordinator = register_probe(bus, {ping_schema()});
    Registered incumbent = register_set_probe(bus, {"svc"});
    Registered candidate = register_probe(bus, {ping_schema()});
    REQUIRE(bus.seal_weave(candidate.id, coordinator.id));
    const TxnResult begun = bus.begin_prepared_replacement(op.id, coordinator.id, incumbent.id,
                                                           candidate.id, "svc", 4);
    REQUIRE(begun);
    REQUIRE(bus.transaction_active(begun.id));

    REQUIRE(bus.release_role(incumbent.id, "svc") == RoleRelease::Released);
    CHECK_FALSE(bus.transaction_active(begun.id));
    TxnOutcome out;
    REQUIRE(bus.take_outcome(op.id, begun.id, out));
    CHECK(out.state == TxnState::Aborted);
    CHECK(out.reason == TxnReason::RoleChanged);
}

TEST_CASE("a replacement of an incumbent holding two roles is refused in words, and nothing "
          "moves") {
    Switchboard bus;
    Registered op = register_probe(bus, {ping_schema()});
    Registered coordinator = register_probe(bus, {ping_schema()});
    Registered incumbent = register_set_probe(bus, {"svc", "aux"});
    Registered candidate = register_probe(bus, {ping_schema(), sbfx::counter_schema()});
    REQUIRE(bus.seal_weave(candidate.id, coordinator.id));

    // The prepared replacement: refused before a slot is taken.
    const TxnResult begun = bus.begin_prepared_replacement(op.id, coordinator.id, incumbent.id,
                                                           candidate.id, "svc", 4);
    CHECK_FALSE(begun);
    CHECK(begun.why == TxnReason::SeveralRoles);
    CHECK(std::string(name_of(begun.why)) == "SeveralRoles");
    CHECK(bus.active_transactions() == 0);

    // The direct commit, and a direct admission: refused, nothing moved.
    CHECK_FALSE(bus.commit_candidate(candidate.id, incumbent.id, "svc"));
    const AdmitResult admitted =
        bus.admit_candidate(candidate.id, incumbent.id, "svc", host_lifecycle_authority(bus),
                            Message(activation_of(1)), 1);
    CHECK_FALSE(admitted);
    CHECK(admitted.why == AdmitRefusal::SeveralRoles);
    CHECK(std::string(name_of(admitted.why)) == "SeveralRoles");
    bus.drain_until_idle();
    CHECK(bus.roles_of(incumbent.id) == Roles{"svc", "aux"});
    CHECK(bus.roles_of(candidate.id).empty());
    CHECK(bus.sealed(candidate.id));
    CHECK_FALSE(bus.sealed(incumbent.id));
}

} // TEST_SUITE
