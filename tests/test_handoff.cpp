// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE HANDOFF GARDEN (R2E-0) — how one incarnation deliberately becomes another.
//
// THE CLAIM UNDER TEST, and it is a claim about what Loom does NOT need:
//
//     Authored Handoff requires no new Loom primitive. Prepared replacement
//     already verifies the successor; FIFO already provides an exact boundary;
//     NotAccepted already provides a visible refusal for incompatible traffic;
//     and an ordinary weave already provides an inspectable, testable,
//     versioned, refusable, attributable migrator.
//
// Every case below is written to break that claim if it is false. Nothing in
// this file calls an API that R2E-0 added for continuity, because R2E-0 added
// none: the substrate calls are all PR-era, and what is new is the PATTERN.
//
// The standing law is unchanged and is re-proven here rather than assumed:
// prepared replacement preserves NOTHING (PR-09). The migration is what carries
// meaning across, and it is authored.

#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "weavelib/handoff_protocol.hpp"

#include <zen/host/lifecycle_wiring.hpp>
#include <zen/host/prepared_replacement.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace sbfx;
using namespace hg;

namespace {

/// A minimal persistable state for the native participants below — they are
/// bystanders to the ceremony, not part of what crosses it.
struct Bookkeeping {
    std::int64_t seen = 0;
    ZEN_SHAPE(Bookkeeping, 1, ZEN_FIELD(seen));
};

/// THE COORDINATOR, and the whole host/coordinator bridge in one place.
///
/// It is an ordinary weave that happens to hold a reference to a host-owned
/// PreparedReplacement handle, because `accept_preparation_answer` must run
/// inside the coordinator's own delivery of the candidate's answer. That is the
/// known authoring friction (known-seams § PreparedReplacement
/// host/coordinator), and this phase records another sighting of it rather than
/// pre-emptively fixing it.
///
/// It drives the whole sequence: ask the incumbent to describe itself, hand that
/// to the migrator, hand the migrator's answer to the candidate, and map the
/// candidate's authenticated answer to Ready or Refused.
class Coordinator : public WeaveBase<Coordinator, Bookkeeping,
                                     Accept<LedgerV1Report, MigrationResult, Adopted>,
                                     Emit<MigrateV1ToV2, AdoptMigrated>> {
public:
    void on(const LedgerV1Report& r, Mail& mail) {
        // THE HONEST LABEL travels with the value. A report taken before the
        // boundary is a snapshot of a live service; one taken after it is that
        // service's final authored value. This coordinator records which it got
        // and never conflates them.
        last_report = r;
        got_report = true;
        if (!forward_to_migrator) {
            return;
        }
        mail.send(migrator, MigrateV1ToV2{r.state, carry_namespace});
    }

    void on(const MigrationResult& m, Mail&) {
        migration = m;
        got_migration = true;
        // A REFUSED MIGRATION NEVER REACHES THE CANDIDATE. There is nothing valid
        // to offer it, and manufacturing something would be exactly the invisible
        // coercion this design refuses.
        if (!m.ok || txn == nullptr) {
            return;
        }
        (void)txn->ask(AdoptMigrated{m.to});
    }

    void on(const Adopted& a, Mail& mail) {
        (void)mail;
        got_adopted = true;
        adopted = a;
        if (txn == nullptr) {
            return;
        }
        // THE TRUST BOUNDARY, exactly as PR-04 states it: the candidate supplies
        // the DOMAIN answer; this coordinator maps it to the transaction's
        // verdict; the Switchboard authenticates only the conversation.
        offered = txn->offer_current_answer(a.ready ? PreparationAnswer::Ready
                                              : PreparationAnswer::Refused)
                      .ok;
    }

    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, false}; }

    PreparedReplacement* txn = nullptr;
    WeaveId migrator{};
    bool forward_to_migrator = true;
    bool carry_namespace = true;

    LedgerV1Report last_report{};
    MigrationResult migration{};
    Adopted adopted{};
    bool got_report = false;
    bool got_migration = false;
    bool got_adopted = false;
    bool offered = false;
};



/// A plain client that records the ids it was issued, so identity collisions are
/// observed rather than asserted.
class Client : public WeaveBase<Client, Bookkeeping, Accept<Issued>, Emit<Issue, AddV1, AddV2>> {
public:
    void on(const Issued& i, Mail&) { ids.push_back(i.id); }
    std::vector<std::int64_t> ids;
    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, false}; }
};

/// The garden: a bus, a kernel, the three artifacts, a coordinator and a client.
struct Garden {
    Switchboard bus;
    Kernel kernel{bus};
    Coordinator* coordinator = nullptr;
    Client* client = nullptr;
    WeaveId coordinator_id{};
    WeaveId client_id{};
    WeaveId incumbent{};
    WeaveId migrator{};

    Garden() {
        auto c = std::make_unique<Coordinator>();
        coordinator = c.get();
        coordinator_id = bus.register_weave(std::move(c), Grant{}.allow_any());
        coordinator->zen_set_self(coordinator_id);

        auto cl = std::make_unique<Client>();
        client = cl.get();
        client_id = bus.register_weave(std::move(cl), Grant{}.allow_any());
        client->zen_set_self(client_id);
    }

    /// Load v1 into the production role and run some real business through it.
    void start_v1(std::int64_t issue_count, std::int64_t add_total) {
        LoadResult v1 = kernel.load("ledger.v1", ZEN_SO_HANDOFF_V1, kLedgerRole,
                                    Grant{}.allow_any().allow_observe_any());
        REQUIRE_MESSAGE(v1.ok, v1.error);
        incumbent = v1.id;
        for (std::int64_t i = 0; i < issue_count; ++i) {
            bus.send_as(client_id, incumbent,
                        Message(to_value(Issue{i}), client_id, client_id, 0));
        }
        if (add_total != 0) {
            bus.send(incumbent, Message(to_value(AddV1{add_total})));
        }
        bus.pump();
    }

    void load_migrator() {
        LoadResult m = kernel.load("migrator", ZEN_SO_HANDOFF_MIGRATOR, "", Grant{}.allow_any());
        REQUIRE_MESSAGE(m.ok, m.error);
        migrator = m.id;
        coordinator->migrator = migrator;
    }

    /// Ask the incumbent to describe itself, through the ordinary answer rail.
    void ask_describe() {
        bus.send_as(coordinator_id, incumbent,
                    Message(to_value(Describe{1}), coordinator_id, coordinator_id, 77));
        bus.pump();
    }

    SenseReading ledger_status() {
        return bus.observe_office(kLedgerRole, "LedgerStatus", 1);
    }
    LedgerStatus status() { return from_value<LedgerStatus>(*ledger_status().value); }
};

} // namespace

TEST_SUITE("handoff") {

// ---- law 1: different schemas remain different ------------------------------

TEST_CASE("R2E-0/H: a v1 value does not pass v2's gate — no automatic transcode, however "
          "related the versions look") {
    // The premise the whole part rests on. If this ever passes, the migration is
    // decoration.
    const Value v1 = to_value(LedgerV1{48, 500, "plain"});
    Admission as_v2 = admit(Value(v1), *schema_of<LedgerV2>());
    CHECK_FALSE(as_v2.ok());

    // ...and the reverse, so nobody can claim the gate is merely lenient one way.
    LedgerV2 rich;
    rich.ids.high_water = 47;
    rich.totals = Metrics{1, 500};
    rich.modes.push_back(ModeFlag{"plain", true});
    Admission as_v1 = admit(to_value(rich), *schema_of<LedgerV1>());
    CHECK_FALSE(as_v1.ok());
}

// ---- H1: the snapshot-tolerant migration, labelled honestly ------------------

TEST_CASE("R2E-0/H1: a snapshot taken while the incumbent is LIVE is a snapshot — it can go "
          "stale before it is used, and the witness says so rather than calling it exact") {
    Garden g;
    g.start_v1(/*issue_count=*/3, /*add_total=*/100);
    g.load_migrator();
    g.coordinator->forward_to_migrator = false; // just capture, for now

    g.ask_describe();
    REQUIRE(g.coordinator->got_report);
    const LedgerV1 snapshot = g.coordinator->last_report.state;
    CHECK(snapshot.next_id == 4);
    // THE LABEL. This is the honest half: the incumbent had NOT quiesced, so this
    // is a description of a moving thing.
    CHECK_FALSE(g.coordinator->last_report.quiesced);

    // ...and the incumbent proves the point by moving.
    g.bus.send_as(g.client_id, g.incumbent,
                  Message(to_value(Issue{9}), g.client_id, g.client_id, 0));
    g.bus.pump();

    // The captured snapshot is now WRONG about the world, and nothing in Loom
    // stopped that. PR-09 is unchanged: replacement creates no atomic handoff.
    CHECK(snapshot.next_id == 4);
    CHECK(g.status().issued_high_water == 4); // the world moved on
}

// ---- H2: the exact authored boundary ----------------------------------------

TEST_CASE("R2E-0/H2: the FIFO boundary makes the incumbent's final value EXACT — A/B/C are "
          "handled ordinarily, the boundary lands at its exact position, and D/E meet the "
          "domain's declared post-boundary policy") {
    Garden g;
    g.start_v1(/*issue_count=*/2, /*add_total=*/0);
    g.coordinator->forward_to_migrator = false;

    // A, B, C — ordinary production. Then the BOUNDARY. Then D, E.
    // Everything is queued in one go, so the boundary's position is decided by
    // FIFO and nothing else.
    g.bus.send(g.incumbent, Message(to_value(AddV1{10})));                     // A
    g.bus.send(g.incumbent, Message(to_value(AddV1{20})));                     // B
    g.bus.send_as(g.client_id, g.incumbent,
                  Message(to_value(Issue{3}), g.client_id, g.client_id, 0));   // C
    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));                    // BOUNDARY
    g.bus.send(g.incumbent, Message(to_value(AddV1{999})));                    // D
    g.bus.send_as(g.client_id, g.incumbent,
                  Message(to_value(Issue{4}), g.client_id, g.client_id, 0));   // E
    g.bus.pump();

    const LedgerStatus st = g.status();
    // A/B/C landed under ordinary policy: three ids issued in total (2 + C).
    CHECK(st.issued_high_water == 3);
    // The boundary was reached, and the ledger quiesced there.
    CHECK(st.quiesced);
    // D and E met the DECLARED policy — this domain refuses — and the refusal is
    // observable, counted, and attributable to a policy the domain chose.
    CHECK(st.refused_after_boundary == 2);
    // E was refused, so the client was never issued a fourth id.
    CHECK(g.client->ids.size() == 3);

    // NOW the description is exact: nothing further can change it.
    g.ask_describe();
    REQUIRE(g.coordinator->got_report);
    CHECK(g.coordinator->last_report.quiesced); // the label that makes it exact
    CHECK(g.coordinator->last_report.state.next_id == 4);
}

// ---- the full authored handoff ----------------------------------------------

TEST_CASE("R2E-0/H: the whole authored handoff — quiesce, author the final value, migrate it "
          "through a temporary weave, prepare, commit, and carry the identity namespace") {
    Garden g;
    g.start_v1(/*issue_count=*/47, /*add_total=*/500);
    g.load_migrator();
    CHECK(g.status().issued_high_water == 47);

    // 1. THE BOUNDARY. The incumbent quiesces at an exact FIFO position.
    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));
    g.bus.pump();
    CHECK(g.status().quiesced);

    // 2. THE FINAL AUTHORED VALUE, taken after the boundary — exact, not a
    //    snapshot of a moving thing, and labelled as such.
    g.ask_describe();
    REQUIRE(g.coordinator->got_report);
    REQUIRE(g.coordinator->last_report.quiesced);
    CHECK(g.coordinator->last_report.state.next_id == 48); // 47 issued, 48 is next

    // 3. THE CANDIDATE, loaded sealed by the prepared-replacement handle. The
    //    ceremony is entirely PR-era; nothing new was added for continuity.
    PreparedReplacement txn(g.bus, g.kernel);
    g.coordinator->txn = &txn;
    const auto started = txn.start(PreparedReplacement::Start{
        g.coordinator_id, g.coordinator_id, kLedgerRole, "ledger.v2", ZEN_SO_HANDOFF_V2, 16});
    REQUIRE_MESSAGE(started.ok, started.error);
    const WeaveId candidate = txn.candidate();

    // The candidate is sealed: it has claimed nothing as the office, and cannot.
    SenseReading before = g.ledger_status();
    REQUIRE(before);
    CHECK(before.by.author == g.incumbent);
    CHECK(before.by.office_holder_is_current);
    CHECK_FALSE(g.bus.observe(candidate, "LedgerStatus", 1));

    // 4. THE MIGRATION, authored by a temporary weave with a bus identity.
    g.coordinator->forward_to_migrator = true;
    g.ask_describe(); // this report is forwarded to the migrator
    g.bus.pump();
    REQUIRE(g.coordinator->got_migration);
    CHECK(g.coordinator->migration.ok);
    // The transformation really transformed: a "next" became a "highest issued".
    CHECK(g.coordinator->migration.to.ids.high_water == 47);
    CHECK(g.coordinator->migration.to.totals.sum == 500);
    CHECK(g.coordinator->migration.to.totals.count == 1);
    REQUIRE(g.coordinator->migration.to.modes.size() == 1);
    CHECK(g.coordinator->migration.to.modes[0].name == "plain");

    // 5. The candidate adopted it and answered for itself; the coordinator
    //    mapped that authenticated answer to Ready.
    g.bus.pump();
    REQUIRE(g.coordinator->got_adopted);
    CHECK(g.coordinator->adopted.ready);
    CHECK(g.coordinator->offered);
    CHECK(txn.state() == TxnState::Ready);

    // 6. COMMIT. The role moves in place, and the candidate is told it is alive.
    REQUIRE(txn.commit(1).ok);
    g.bus.pump();
    const auto outcome = txn.take_outcome();
    REQUIRE(outcome.has_value());
    CHECK(outcome->state == TxnState::Committed);
    CHECK(g.bus.role_holder(kLedgerRole) == candidate);

    // 7. THE NAMESPACE WITNESS. The next identity is 48, not 1: the high-water
    //    mark crossed an incompatible state schema because a migration carried
    //    it, and for no other reason.
    g.bus.send_as(g.client_id, candidate,
                  Message(to_value(Issue{99}), g.client_id, g.client_id, 0));
    g.bus.pump();
    REQUIRE(g.client->ids.size() == 48);
    CHECK(g.client->ids.back() == 48);
    // No id was ever issued twice.
    std::vector<std::int64_t> sorted = g.client->ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // 8. BOTH TEMPORARY ARTIFACTS UNLOAD. The migrator was never required after
    //    the handoff, and the retired incumbent is genuinely retired.
    CHECK(g.kernel.unload("migrator"));
    CHECK(g.kernel.unload("ledger.v1"));
    // ...and they are GONE, not merely reported gone. A "temporary" artifact
    // that is still loaded is still a dependency, whatever the return value said.
    CHECK_FALSE(g.kernel.is_loaded("migrator"));
    CHECK_FALSE(g.kernel.is_loaded("ledger.v1"));
    CHECK_FALSE(g.bus.observe(g.incumbent, "LedgerStatus", 1)); // its keys went too
    // ...and the successor keeps serving with both of them gone, which is what
    // "temporary" has to mean to be worth the word.
    g.bus.send_as(g.client_id, candidate,
                  Message(to_value(Issue{100}), g.client_id, g.client_id, 0));
    g.bus.pump();
    CHECK(g.client->ids.back() == 49);
}

// ---- the namespace control case ---------------------------------------------

TEST_CASE("R2E-0/H: the control case — a domain that deliberately does NOT carry the namespace "
          "gets repeated identities, and the difference is the migration's, not Loom's") {
    Garden g;
    g.start_v1(/*issue_count=*/47, /*add_total=*/500);
    g.load_migrator();
    g.coordinator->carry_namespace = false; // THE ONLY DIFFERENCE

    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));
    g.bus.pump();

    PreparedReplacement txn(g.bus, g.kernel);
    g.coordinator->txn = &txn;
    const auto started = txn.start(PreparedReplacement::Start{
        g.coordinator_id, g.coordinator_id, kLedgerRole, "ledger.v2", ZEN_SO_HANDOFF_V2, 16});
    REQUIRE_MESSAGE(started.ok, started.error);

    g.ask_describe();
    g.bus.pump();
    g.bus.pump();
    REQUIRE(g.coordinator->migration.ok);
    CHECK(g.coordinator->migration.to.ids.high_water == 0); // deliberately dropped

    REQUIRE(txn.commit(1).ok);
    g.bus.pump();
    CHECK(g.bus.role_holder(kLedgerRole) == txn.candidate());

    // THE DEFECT, made visible: the successor mints 1 again — an id the
    // predecessor already handed out. Loom has no allocator and no opinion; the
    // namespace obligation was the domain's, and this domain declined it.
    g.bus.send_as(g.client_id, txn.candidate(),
                  Message(to_value(Issue{99}), g.client_id, g.client_id, 0));
    g.bus.pump();
    CHECK(g.client->ids.back() == 1);
    std::vector<std::int64_t> sorted = g.client->ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()); // a repeat exists
}

// ---- a refused migration -----------------------------------------------------

TEST_CASE("R2E-0/H: a migrator that does not understand its input REFUSES, nothing reaches the "
          "candidate, and the incumbent is still the service") {
    Garden g;
    // An empty mode is meaningless in v2's vocabulary; the migrator says so.
    g.start_v1(/*issue_count=*/1, /*add_total=*/0);
    g.load_migrator();
    // Force the coordinator to hand the migrator something it will refuse.
    g.coordinator->forward_to_migrator = false;
    g.bus.send_as(g.coordinator_id, g.migrator,
                  Message(to_value(MigrateV1ToV2{LedgerV1{5, 0, ""}, true}), g.coordinator_id,
                          g.coordinator_id, 0));
    g.bus.pump();

    REQUIRE(g.coordinator->got_migration);
    CHECK_FALSE(g.coordinator->migration.ok);
    CHECK(g.coordinator->migration.reason.find("mode") != std::string::npos);
    // A refusal is a value, not an exception and not a silence — and the world
    // is unchanged: the incumbent still holds the role.
    CHECK(g.bus.role_holder(kLedgerRole) == g.incumbent);
}

// ---- queued old-protocol traffic --------------------------------------------

TEST_CASE("R2E-0/H: queued OLD-PROTOCOL traffic is never magically migrated — four positions "
          "around the replacement, four honest outcomes") {
    Garden g;
    g.start_v1(/*issue_count=*/2, /*add_total=*/0);
    g.load_migrator();

    std::vector<RefusalReason> refusals;
    g.bus.add_observer([&refusals](const BusEvent& ev) {
        if (ev.kind == EventKind::Refused) {
            refusals.push_back(ev.refusal.reason);
        }
    });

    // POSITION 1 — before the boundary, incumbent owns the office. Ordinary
    // handling: the old command means what it always meant.
    g.bus.send(g.incumbent, Message(to_value(AddV1{10})));
    g.bus.pump();
    CHECK(refusals.empty());

    // POSITION 2 — after the boundary, incumbent STILL owns the office. The
    // message is delivered (the shape is accepted, the holder is unchanged) and
    // the DOMAIN declines it. This is a domain policy, not a Loom refusal, and
    // the two are deliberately different things.
    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));
    g.bus.send(g.incumbent, Message(to_value(AddV1{20})));
    g.bus.pump();
    CHECK(g.status().refused_after_boundary == 1);
    CHECK(refusals.empty()); // nothing was refused BY LOOM

    // Prepare and commit the replacement.
    PreparedReplacement txn(g.bus, g.kernel);
    g.coordinator->txn = &txn;
    const auto started = txn.start(PreparedReplacement::Start{
        g.coordinator_id, g.coordinator_id, kLedgerRole, "ledger.v2", ZEN_SO_HANDOFF_V2, 16});
    REQUIRE_MESSAGE(started.ok, started.error);
    g.ask_describe();
    g.bus.pump();
    g.bus.pump();
    REQUIRE(txn.state() == TxnState::Ready);

    // POSITION 3 — queued AROUND the commit, addressed to the ROLE. It is
    // resolved at delivery, so it reaches whoever holds the office THEN: the
    // successor, which does not accept the old shape.
    REQUIRE(txn.commit(1).ok);
    g.bus.send_to_role(kLedgerRole, Message(to_value(AddV1{30})));
    g.bus.pump();
    CHECK(g.bus.role_holder(kLedgerRole) == txn.candidate());

    // POSITION 4 — after the role moved, to a successor that no longer accepts
    // the old shape at all.
    g.bus.send_to_role(kLedgerRole, Message(to_value(AddV1{40})));
    g.bus.pump();

    // THE LESSON, in the refusal list. Loom gave the developer a boundary and a
    // refusal; it did not pretend to know whether AddV1 still meant anything.
    // Both post-admission old-protocol messages refuse VISIBLY and by name.
    REQUIRE(refusals.size() == 2);
    CHECK(refusals[0] == RefusalReason::NotAccepted);
    CHECK(refusals[1] == RefusalReason::NotAccepted);
    // ...and nothing was transformed: the successor's totals never saw 30 or 40.
    CHECK(g.bus.observe_office(kLedgerRole, "LedgerStatus", 1));
}

// ---- Senses meet Handoff -----------------------------------------------------

TEST_CASE("R2E-0/H+S: across the handoff the office's claim is never relabelled — the "
          "incumbent's final claim stays the incumbent's, the successor claims nothing until it "
          "does, and then the role-bound view follows it") {
    Garden g;
    g.start_v1(/*issue_count=*/5, /*add_total=*/50);
    g.load_migrator();

    // The incumbent's latest claim, as the office, while it is still the office.
    SenseReading live = g.ledger_status();
    REQUIRE(live);
    CHECK(live.by.author == g.incumbent);
    CHECK(live.by.office_holder_is_current);
    CHECK(from_value<LedgerStatus>(*live.value).version == "v1");

    // The boundary: the incumbent makes its FINAL claim as the office.
    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));
    g.bus.pump();
    SenseReading final_claim = g.ledger_status();
    REQUIRE(final_claim);
    CHECK(from_value<LedgerStatus>(*final_claim.value).quiesced);
    const std::uint64_t final_revision = final_claim.by.revision;

    PreparedReplacement txn(g.bus, g.kernel);
    g.coordinator->txn = &txn;
    const auto started = txn.start(PreparedReplacement::Start{
        g.coordinator_id, g.coordinator_id, kLedgerRole, "ledger.v2", ZEN_SO_HANDOFF_V2, 16});
    REQUIRE_MESSAGE(started.ok, started.error);

    // A SEALED CANDIDATE HAS NO OFFICE CLAIM, and cannot make one.
    CHECK_FALSE(g.bus.observe(txn.candidate(), "LedgerStatus", 1));

    g.ask_describe();
    g.bus.pump();
    g.bus.pump();
    REQUIRE(txn.commit(1).ok);
    g.bus.pump();
    REQUIRE(g.bus.role_holder(kLedgerRole) == txn.candidate());

    // The successor's zen.Activated handler claimed as the office — legally,
    // after activation. Before that handler ran, the office's claim was still the
    // predecessor's; the two facts are never merged.
    SenseReading now = g.ledger_status();
    REQUIRE(now);
    CHECK(now.by.author == txn.candidate());
    CHECK(now.by.office_holder_is_current);
    CHECK(from_value<LedgerStatus>(*now.value).version == "v2");
    // A replacement of the SAME office key: the revision advanced rather than
    // resetting, because the role moved in place and the key never died.
    CHECK(now.by.revision == final_revision + 1);

    // ...and the predecessor's own PERSONAL view is untouched by any of it: it
    // never made a personal claim, and nobody invented one for it.
    CHECK_FALSE(g.bus.observe(g.incumbent, "LedgerStatus", 1));
}

TEST_CASE("R2E-0/H+S: an ordinary Sense is NOT an exact handoff snapshot — a domain using one "
          "as migration input inherits its staleness") {
    Garden g;
    g.start_v1(/*issue_count=*/3, /*add_total=*/0);

    // Read the Sense while the incumbent is live and NOT quiesced.
    const LedgerStatus mid_flight = g.status();
    CHECK(mid_flight.issued_high_water == 3);
    CHECK_FALSE(mid_flight.quiesced); // the Sense says so itself

    // The world moves; the value read a moment ago is now stale, exactly as any
    // pre-boundary snapshot would be.
    g.bus.send_as(g.client_id, g.incumbent,
                  Message(to_value(Issue{4}), g.client_id, g.client_id, 0));
    g.bus.pump();
    CHECK(mid_flight.issued_high_water == 3); // what the reader holds
    CHECK(g.status().issued_high_water == 4); // what is now claimed

    // AFTER the boundary the same Sense IS exact, and says which it is — the
    // `quiesced` flag is the domain's own honest label, not a Loom guarantee.
    g.bus.send(g.incumbent, Message(to_value(Quiesce{1})));
    g.bus.pump();
    const LedgerStatus settled = g.status();
    CHECK(settled.quiesced);
    g.bus.send_as(g.client_id, g.incumbent,
                  Message(to_value(Issue{5}), g.client_id, g.client_id, 0));
    g.bus.pump();
    CHECK(g.status().issued_high_water == settled.issued_high_water); // nothing moved
}

// ---- what replacement still does NOT do -------------------------------------

TEST_CASE("R2E-0/H: PR-09 is unchanged — a replacement with NO authored migration carries "
          "nothing, and the successor starts empty") {
    Garden g;
    g.start_v1(/*issue_count=*/47, /*add_total=*/500);

    PreparedReplacement txn(g.bus, g.kernel);
    g.coordinator->txn = &txn;
    const auto started = txn.start(PreparedReplacement::Start{
        g.coordinator_id, g.coordinator_id, kLedgerRole, "ledger.v2", ZEN_SO_HANDOFF_V2, 16});
    REQUIRE_MESSAGE(started.ok, started.error);

    // No migration, no adoption ask — the candidate is simply told to be ready by
    // a coordinator that asked it nothing. (The domain's choice; PR authenticates
    // the conversation, not its wisdom.)
    g.bus.send_as(g.coordinator_id, txn.candidate(),
                  Message(to_value(AdoptMigrated{LedgerV2{}}), g.coordinator_id,
                          g.coordinator_id, 0));
    g.bus.pump();

    // The transaction is not Ready: that AdoptMigrated was an ordinary send, not
    // the preparation ask, so its answer authenticated nothing.
    CHECK(txn.state() == TxnState::Preparing);
    // And the incumbent is still the service, still holding everything.
    CHECK(g.bus.role_holder(kLedgerRole) == g.incumbent);
    CHECK(g.status().issued_high_water == 47);
}

} // TEST_SUITE
