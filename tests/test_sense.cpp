// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// SENSES — the S1..S6 witnesses.
// SENSE-01..05; docs/laws/sense-laws.md
//
// The category under test:
//
//   MESSAGES   what happened / what I want done      causal, FIFO, queued
//   SENSES     what I currently claim is so          acausal, latest-only, pulled
//
// What each case pins, in the phase's own words:
//
//   S1  ordered observation      readers see claims in FIFO order, no ask/answer
//   S2  no future knowledge      a reader queued AHEAD of a change sees the old claim
//   S3  office across replacement a predecessor's claim is never relabelled
//   S4  personal vs office       the same holder's two claims stay distinguishable
//   S5  lifetime cleanup         the repository is bounded by current keys
//   S6  authorization            unauthorized read and forged office claim refuse loudly
//
// Plus the structural ones: a reader cannot mutate a producer through a reading,
// and an older revision never wins over a newer one.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/host/lifecycle_wiring.hpp>
#include <zen/serialize.hpp> // serialize() — the swap_state snapshot in S3b
#include <zen/weave.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

// ---- the domain -------------------------------------------------------------

struct Health {
    std::int64_t hp;
    ZEN_SHAPE(Health, 1, ZEN_FIELD(hp));
};
struct Damage {
    std::int64_t amount;
    ZEN_SHAPE(Damage, 1, ZEN_FIELD(amount));
};
struct ReaderTick {
    std::int64_t n;
    ZEN_SHAPE(ReaderTick, 1, ZEN_FIELD(n));
};
struct Status {
    std::string text;
    ZEN_SHAPE(Status, 1, ZEN_FIELD(text));
};
struct ProducerState {
    std::int64_t hp = 100; // a full-health producer, so the arithmetic below reads plainly
    ZEN_SHAPE(ProducerState, 1, ZEN_FIELD(hp));
};
struct WatcherState {
    std::int64_t seen;
    ZEN_SHAPE(WatcherState, 1, ZEN_FIELD(seen));
};

/// THE PRODUCER. Handles ordinary FIFO messages, and after each one claims what
/// it now says is so. Nothing about the claim is special: it is one line at the
/// end of an ordinary handler.
class Producer : public WeaveBase<Producer, ProducerState, Accept<Damage>, Emit<>,
                                  Claims<Health, Status>> {
public:
    void on(const Damage& d, Mail& mail) {
        state_.hp -= d.amount;
        last_claim = mail.claim(Health{state_.hp});
    }
    SenseClaimResult last_claim{};
};

/// THE READER. Observes synchronously from inside an ordinary delivery — no ask,
/// no answer, no traffic of its own.
class Reader : public WeaveBase<Reader, WatcherState, Accept<ReaderTick>> {
public:
    void on(const ReaderTick&, Mail& mail) {
        SenseReading r = mail.latest<Health>(watch);
        seen.push_back(r);
        ++state_.seen;
    }
    WeaveId watch{};
    std::vector<SenseReading> seen;
};

/// A weave that holds an office and can be told to claim personally or as the
/// office, so the two can be compared with everything else held equal.
struct ClaimPersonally {
    std::string text;
    ZEN_SHAPE(ClaimPersonally, 1, ZEN_FIELD(text));
};
struct ClaimAsOffice {
    std::string text;
    ZEN_SHAPE(ClaimAsOffice, 1, ZEN_FIELD(text));
};

class Officer : public WeaveBase<Officer, WatcherState, Accept<ClaimPersonally, ClaimAsOffice>,
                                 Emit<>, Claims<Status>> {
public:
    void on(const ClaimPersonally& c, Mail& mail) { personal = mail.claim(Status{c.text}); }
    void on(const ClaimAsOffice& c, Mail& mail) {
        office = mail.as_role(role).claim(Status{c.text});
    }
    std::string role = "station";
    SenseClaimResult personal{};
    SenseClaimResult office{};
};

/// Register a weave and hand back its raw pointer, the way these tests want it.
template <class W, class... Args>
std::pair<WeaveId, W*> put(Switchboard& bus, Grant grant, std::string role, Args&&... args) {
    auto owned = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = owned.get();
    const WeaveId id = bus.register_weave(std::move(owned), std::move(grant), std::move(role));
    raw->zen_set_self(id);
    return {id, raw};
}

/// The reader grant these tests use: permission to observe the two shapes, and
/// nothing else. Note it grants no SEND authority at all — observing is its own
/// authority, and that is the point.
Grant observer_of_health() {
    return Grant{}.allow_observe("Health", 1).allow_observe("Status", 1);
}

std::int64_t hp_of(const SenseReading& r) { return from_value<Health>(*r.value).hp; }
std::string text_of(const SenseReading& r) { return from_value<Status>(*r.value).text; }

} // namespace

TEST_SUITE("sense") {

// ---- S1: ordered observation -----------------------------------------------

TEST_CASE("S1: many readers observe a producer's latest claim synchronously, with no "
          "request/answer traffic, and in exact FIFO order") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    auto [r1, reader1] = put<Reader>(bus, observer_of_health(), "");
    auto [r2, reader2] = put<Reader>(bus, observer_of_health(), "");
    reader1->watch = pid;
    reader2->watch = pid;

    // The producer's opening claim, made by the host on its behalf is NOT how
    // this works — a claim is the producer's own act. So: damage it once.
    bus.send(pid, Message(to_value(Damage{18})));
    bus.pump();
    CHECK(producer->last_claim.accepted);
    CHECK(producer->last_claim.revision == 1);

    // TWO readers, ONE claim, ZERO messages between them and the producer.
    bus.send(r1, Message(to_value(ReaderTick{1})));
    bus.send(r2, Message(to_value(ReaderTick{1})));
    bus.pump();

    REQUIRE(reader1->seen.size() == 1);
    REQUIRE(reader2->seen.size() == 1);
    CHECK(hp_of(reader1->seen[0]) == 82);
    CHECK(hp_of(reader2->seen[0]) == 82);

    // THE ORDERING WITNESS, exactly as the phase states it. Queue:
    //     Damage{18}  ReaderTick  Damage{18}  ReaderTick
    // Each ReaderTick must see the claim as of the Damage BEFORE it.
    bus.send(pid, Message(to_value(Damage{12})));
    bus.send(r1, Message(to_value(ReaderTick{2})));
    bus.send(pid, Message(to_value(Damage{30})));
    bus.send(r1, Message(to_value(ReaderTick{3})));
    bus.pump();

    REQUIRE(reader1->seen.size() == 3);
    CHECK(hp_of(reader1->seen[1]) == 70); // after Damage{12}, before Damage{30}
    CHECK(hp_of(reader1->seen[2]) == 40); // after Damage{30}
    // Revisions order the replacement of this one claim, and only that.
    CHECK(reader1->seen[0].by.revision == 1);
    CHECK(reader1->seen[1].by.revision == 2);
    CHECK(reader1->seen[2].by.revision == 3);
}

TEST_CASE("S1: a reading carries truthful authorship — who claimed it, under which life and "
          "incarnation, at which revision, and personally rather than as an office") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    auto [rid, reader] = put<Reader>(bus, observer_of_health(), "");
    reader->watch = pid;

    bus.send(pid, Message(to_value(Damage{1})));
    bus.send(rid, Message(to_value(ReaderTick{1})));
    bus.pump();

    REQUIRE(reader->seen.size() == 1);
    const SenseAuthorship& by = reader->seen[0].by;
    CHECK(by.author == pid);
    CHECK(by.author_life == 1);
    CHECK(by.author_incarnation == 1);
    CHECK(by.author_life_is_current);
    CHECK(by.office.empty()); // a personal claim, and it says so
    CHECK_FALSE(by.office_holder_is_current);
    CHECK(by.revision == 1);
    CHECK(by.schema_name == "Health");
    CHECK(by.schema_version == 1);
}

// ---- S2: no future knowledge ------------------------------------------------

TEST_CASE("S2: a reader delivered BEFORE a state-changing message observes the OLD claim — "
          "queued work is never applied speculatively to make a claim look current") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    auto [rid, reader] = put<Reader>(bus, observer_of_health(), "");
    reader->watch = pid;

    bus.send(pid, Message(to_value(Damage{0}))); // establish Health{100}
    bus.pump();
    REQUIRE(hp_of(bus.observe(pid, "Health", 1)) == 100);

    // THE READER IS QUEUED FIRST, the change SECOND. Both are already in the
    // queue when the pump starts, so the change is a known future — and the
    // reader must not see it.
    bus.send(rid, Message(to_value(ReaderTick{1})));
    bus.send(pid, Message(to_value(Damage{25})));
    bus.pump();

    REQUIRE(reader->seen.size() == 1);
    CHECK(hp_of(reader->seen[0]) == 100); // the claim as it stood, not as it will be
    // ...and afterwards the claim really did change, so the reader saw a real
    // past rather than a repository that simply never updates.
    CHECK(hp_of(bus.observe(pid, "Health", 1)) == 75);
    CHECK(producer->last_claim.revision == 2);
}

// ---- S3: office claim across replacement ------------------------------------

TEST_CASE("S3: role movement never relabels a predecessor's office claim, and the successor is "
          "not considered to have claimed anything until it deliberately does") {
    Switchboard bus;
    auto [aid, incumbent] = put<Officer>(bus, Grant{}, "station");
    incumbent->role = "station";

    bus.send(aid, Message(to_value(ClaimAsOffice{"A is on duty"})));
    bus.pump();
    REQUIRE(incumbent->office.accepted);

    // The office claim reads as A's, and A currently holds the office.
    SenseReading before = bus.observe_office("station", "Status", 1);
    REQUIRE(before);
    CHECK(text_of(before) == "A is on duty");
    CHECK(before.by.author == aid);
    CHECK(before.by.office == "station");
    CHECK(before.by.office_holder_is_current);
    CHECK_FALSE(before.by.office_claim_is_stale());

    // A LEAVES. The role becomes unheld, which is the ONLY way an office claim
    // is dropped — an office with no officeholder has no current claimant, and
    // keeping the claim would be a claim by nobody.
    bus.unregister_weave(aid);

    SenseReading after = bus.observe_office("station", "Status", 1);
    CHECK_FALSE(after);                            // gone with the office
    CHECK(after.refusal == SenseRefusal::NoClaim); // and says exactly that

    // The successor takes the office and claims for itself.
    auto [bid, successor] = put<Officer>(bus, Grant{}, "station");
    successor->role = "station";
    // Loom invents nothing for it: before it claims, the office has no claim.
    CHECK_FALSE(bus.observe_office("station", "Status", 1));
    CHECK_FALSE(bus.observe(bid, "Status", 1));

    bus.send(bid, Message(to_value(ClaimAsOffice{"B is on duty"})));
    bus.pump();
    SenseReading now = bus.observe_office("station", "Status", 1);
    REQUIRE(now);
    CHECK(text_of(now) == "B is on duty");
    CHECK(now.by.author == bid);
    CHECK(now.by.office_holder_is_current);
    // A fresh claim under a key that had been cleaned up — not a continuation of
    // A's, and its revision says so.
    CHECK(now.by.revision == 1);
}

// The other half of S3 — the role MOVING between two live holders, where the
// predecessor's claim must survive stamped stale rather than being deleted or
// relabelled — needs the real prepared-replacement ceremony (the only thing that
// moves a role holder in place). It lives in the kernel suite beside that
// ceremony: `test_kernel.cpp`, the Senses-across-replacement section.

// ---- S3b: the two generation facts are independent --------------------------
//
// A LIVE code swap moves the INCARNATION and leaves the LIFE alone (`swap_state`
// bumps the counter; `begin_new_life` advances only from `!alive`). So across a
// same-life replacement a materialized claim is in a state no single "is the
// author still current?" flag can describe:
//
//     life 7 / incarnation 3   claims X
//     ... live replacement ...
//     life 7 / incarnation 4   is now the code at that address
//
// The claim stays historically truthful — it IS incarnation 3's, and Loom never
// rewrites it — but a reader needs to know the code behind it has moved on.
// These cases pin that the two facts are asked separately, and that neither is
// derived from the other.

TEST_CASE("S3b: a same-life code replacement leaves the LIFE current and the INCARNATION "
          "stale — a predecessor's claim is distinguishable from the current incarnation's") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");

    bus.send(pid, Message(to_value(Damage{10})));
    bus.pump();
    REQUIRE(producer->last_claim.accepted);

    // BEFORE: the claim is the current code's, on the current life. Both true.
    SenseReading fresh = bus.observe(pid, "Health", 1);
    REQUIRE(fresh);
    CHECK(hp_of(fresh) == 90);
    const std::uint64_t life_at_claim = fresh.by.author_life;
    const std::uint64_t inc_at_claim = fresh.by.author_incarnation;
    CHECK(fresh.by.author_life_is_current);
    CHECK(fresh.by.author_incarnation_is_current);

    // NEW CODE BEHIND THE SAME LIVE ID. Not a death: the weave never stopped
    // being alive, so the life stands and only the incarnation advances.
    // (`revived` reports that the swap TOOK, not that a life restarted — the
    // life question is asked of the reading below, which is where it belongs.)
    const std::string snapshot = serialize(to_value(ProducerState{90}));
    REQUIRE(bus.swap_state(pid, snapshot).revived);

    // AFTER: the predecessor's claim is still there, still says who made it, and
    // now says the code behind that author has been replaced. This is the whole
    // correction — a single life-currentness flag reports `true` here and hides
    // the replacement entirely.
    SenseReading stale = bus.observe(pid, "Health", 1);
    REQUIRE(stale);
    CHECK(hp_of(stale) == 90);
    CHECK(stale.by.author == pid);
    CHECK(stale.by.author_life == life_at_claim);           // history is not rewritten
    CHECK(stale.by.author_incarnation == inc_at_claim);     // ...in either field
    CHECK(stale.by.author_life_is_current);                 // the life stands
    CHECK_FALSE(stale.by.author_incarnation_is_current);    // the code does not

    // The successor claims for itself, and reports both current — so the flag
    // tracks the topology rather than latching once a swap has ever happened.
    bus.send(pid, Message(to_value(Damage{5})));
    bus.pump();
    SenseReading current = bus.observe(pid, "Health", 1);
    REQUIRE(current);
    CHECK(hp_of(current) == 85);
    CHECK(current.by.author_incarnation == inc_at_claim + 1);
    CHECK(current.by.author_life_is_current);
    CHECK(current.by.author_incarnation_is_current);
}

TEST_CASE("S3b: a DEATH-AND-REVIVAL moves both generations — incarnation-currentness is not "
          "merely a slower copy of life-currentness") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");

    bus.send(pid, Message(to_value(Damage{10})));
    bus.pump();
    REQUIRE(producer->last_claim.accepted);
    REQUIRE(bus.observe(pid, "Health", 1).by.author_life_is_current);

    // A real death, then a revival: the life advances too, so BOTH facts go
    // false. The pair of cases together is what proves the two are independent —
    // one where they disagree, one where they agree.
    bus.kill(pid);
    const std::string snapshot = serialize(to_value(ProducerState{90}));
    REQUIRE(bus.swap_state(pid, snapshot).revived); // and from DEAD it is a new life too

    SenseReading after = bus.observe(pid, "Health", 1);
    REQUIRE(after);
    CHECK_FALSE(after.by.author_life_is_current);
    CHECK_FALSE(after.by.author_incarnation_is_current);
}

// ---- S4: personal vs office -------------------------------------------------

TEST_CASE("S4: the SAME holder's personal claim and office claim stay distinguishable — merely "
          "holding the office attaches nothing") {
    Switchboard bus;
    auto [oid, officer] = put<Officer>(bus, Grant{}, "station");
    officer->role = "station";

    bus.send(oid, Message(to_value(ClaimPersonally{"personally speaking"})));
    bus.send(oid, Message(to_value(ClaimAsOffice{"officially speaking"})));
    bus.pump();
    REQUIRE(officer->personal.accepted);
    REQUIRE(officer->office.accepted);

    // Two claims, same weave, same shape, same instant — two different keys.
    SenseReading personal = bus.observe(oid, "Status", 1);
    SenseReading office = bus.observe_office("station", "Status", 1);
    REQUIRE(personal);
    REQUIRE(office);
    CHECK(text_of(personal) == "personally speaking");
    CHECK(text_of(office) == "officially speaking");
    // The personal one carries NO office. That is the whole law.
    CHECK(personal.by.office.empty());
    CHECK(office.by.office == "station");
    CHECK(personal.by.author == office.by.author); // same weave, still distinguishable
}

TEST_CASE("S4: a role holder's personal claim does NOT become the office's claim") {
    Switchboard bus;
    auto [oid, officer] = put<Officer>(bus, Grant{}, "station");
    officer->role = "station";

    bus.send(oid, Message(to_value(ClaimPersonally{"just me"})));
    bus.pump();

    CHECK(bus.observe(oid, "Status", 1));               // the personal claim exists
    CHECK_FALSE(bus.observe_office("station", "Status", 1)); // the office has claimed nothing
    CHECK(bus.observe_office("station", "Status", 1).refusal == SenseRefusal::NoClaim);
}

// ---- S5: lifetime cleanup ---------------------------------------------------

TEST_CASE("S5: the repository is bounded by CURRENT KEYS — a thousand claims, repeated "
          "load/claim/unload, and reloads add no entries") {
    Switchboard bus;

    // A thousand claims under one key is one entry.
    {
        auto [pid, producer] = put<Producer>(bus, Grant{}, "");
        (void)producer;
        for (int i = 0; i < 1000; ++i) {
            bus.send(pid, Message(to_value(Damage{1})));
        }
        bus.pump();
        CHECK(bus.retained_claim_count() == 1);
        CHECK(bus.observe(pid, "Health", 1).by.revision == 1000);
        bus.unregister_weave(pid);
        CHECK(bus.retained_claim_count() == 0); // removal takes its keys with it
    }

    // Repeated load / claim / unload does not accumulate one entry per
    // incarnation — the thing the phase names explicitly.
    for (int round = 0; round < 25; ++round) {
        auto [pid, producer] = put<Producer>(bus, Grant{}, "");
        (void)producer;
        bus.send(pid, Message(to_value(Damage{1})));
        bus.pump();
        CHECK(bus.retained_claim_count() == 1);
        bus.unregister_weave(pid);
        CHECK(bus.retained_claim_count() == 0);
    }

    // An office key is bounded the same way, and released when the office is.
    {
        auto [oid, officer] = put<Officer>(bus, Grant{}, "station");
        officer->role = "station";
        for (int i = 0; i < 50; ++i) {
            bus.send(oid, Message(to_value(ClaimAsOffice{"on duty"})));
        }
        bus.pump();
        CHECK(bus.retained_claim_count() == 1);
        bus.unregister_weave(oid); // the role becomes unheld
        CHECK(bus.retained_claim_count() == 0);
    }
}

TEST_CASE("S5: a revival replaces the value under one key rather than adding a key, and the "
          "reading says the claiming life has ended") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;

    bus.send(pid, Message(to_value(Damage{5})));
    bus.pump();
    CHECK(bus.retained_claim_count() == 1);
    CHECK(bus.observe(pid, "Health", 1).by.author_life_is_current);

    // Kill and revive: a new life behind the same id.
    bus.kill(pid);
    ProducerState fresh{95};
    REQUIRE(bus.reload(pid, serialize(to_value(fresh))).revived);

    // ONE key still, and the claim now honestly says its life has ended.
    CHECK(bus.retained_claim_count() == 1);
    SenseReading r = bus.observe(pid, "Health", 1);
    REQUIRE(r);
    CHECK(r.by.author_life == 1);
    CHECK_FALSE(r.by.author_life_is_current); // the claiming life is gone
}

// ---- S6: authorization ------------------------------------------------------

TEST_CASE("S6: an unauthorized read refuses EXPLICITLY, and is not confusable with 'nothing has "
          "been claimed'") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    // A reader with NO observe rule — the default-empty grant every weave gets.
    auto [rid, reader] = put<Reader>(bus, Grant{}.allow_any(), "");
    reader->watch = pid;

    bus.send(pid, Message(to_value(Damage{4})));
    bus.send(rid, Message(to_value(ReaderTick{1})));
    bus.pump();

    REQUIRE(reader->seen.size() == 1);
    CHECK_FALSE(reader->seen[0]);
    // NOT NoClaim: a claim exists and the reader simply may not read it. The two
    // send an operator to opposite places, so they are different answers.
    CHECK(reader->seen[0].refusal == SenseRefusal::NotAuthorized);
    CHECK_FALSE(reader->seen[0].value.has_value());
    CHECK(bus.observe(pid, "Health", 1)); // ...and the claim really is there

    // `allow_any()` — full SEND authority — grants no read. A send rule answers a
    // different question, and reusing it here would be exactly the misleading
    // convenience the design refused.
    CHECK(bus.observe_as(rid, pid, "Health", 1).refusal == SenseRefusal::NotAuthorized);
}

TEST_CASE("S6: an authorized reader is authorized per SHAPE — the rule it holds, not the one it "
          "does not") {
    Switchboard bus;
    auto [oid, officer] = put<Officer>(bus, Grant{}, "station");
    officer->role = "station";
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    const WeaveId reader =
        bus.register_weave(std::make_unique<ProbeWeave>(
                               std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
                           Grant{}.allow_observe("Status", 1));

    bus.send(oid, Message(to_value(ClaimPersonally{"visible"})));
    bus.send(pid, Message(to_value(Damage{1})));
    bus.pump();

    CHECK(bus.observe_as(reader, oid, "Status", 1));  // the granted shape
    CHECK(bus.observe_as(reader, pid, "Health", 1).refusal == SenseRefusal::NotAuthorized);
    CHECK(bus.observe_office_as(reader, "station", "Status", 1).refusal == SenseRefusal::NoClaim);
}

TEST_CASE("S6: claiming as an office you do not hold is refused, and NOTHING is stored — never "
          "downgraded to a personal claim") {
    Switchboard bus;
    auto [aid, holder] = put<Officer>(bus, Grant{}, "station");
    auto [bid, pretender] = put<Officer>(bus, Grant{}, "");
    holder->role = "station";
    pretender->role = "station"; // it will ASK to claim as an office it does not hold

    bus.send(bid, Message(to_value(ClaimAsOffice{"I am the station"})));
    bus.pump();

    CHECK_FALSE(pretender->office.accepted);
    CHECK(pretender->office.why == SenseRefusal::OfficeNotHeld);
    // Nothing was stored anywhere: not under the office key...
    CHECK_FALSE(bus.observe_office("station", "Status", 1));
    // ...and not, quietly, under the pretender's own personal key.
    CHECK_FALSE(bus.observe(bid, "Status", 1));
    CHECK(bus.retained_claim_count() == 0);
}

TEST_CASE("S6: claiming a shape the weave never declared is refused — the claim-set is a "
          "contract, not documentation") {
    Switchboard bus;
    // Reader declares Claims<> (nothing).
    auto [rid, reader] = put<Reader>(bus, observer_of_health(), "");
    (void)reader;
    const SenseClaimResult r = bus.claim_as(rid, to_value(Health{1}));
    CHECK_FALSE(r.accepted);
    CHECK(r.why == SenseRefusal::Undeclared);
    CHECK(bus.retained_claim_count() == 0);
}

// ---- discovery --------------------------------------------------------------

TEST_CASE("discovery: what a participant can claim is answerable BEFORE it has claimed "
          "anything") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;

    const auto declared = bus.claimed_schemas(pid);
    REQUIRE(declared.size() == 2);
    CHECK(declared[0]->name() == "Health");
    CHECK(declared[1]->name() == "Status");
    // Registered at mount, so the shape resolves without waiting for a claim.
    CHECK(bus.resolve_schema("Health", 1) != nullptr);
    // ...and nothing has been claimed yet.
    CHECK(bus.retained_claim_count() == 0);
    CHECK_FALSE(bus.observe(pid, "Health", 1));

    // A weave that declares no Senses claims none, and says so.
    auto [rid, reader] = put<Reader>(bus, Grant{}, "");
    (void)reader;
    CHECK(bus.claimed_schemas(rid).empty());
}

// ---- structural: no shared mutable memory -----------------------------------

TEST_CASE("a reading owns its value — mutating it cannot reach the claimant, and the next "
          "observation is unaffected") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    bus.send(pid, Message(to_value(Damage{10})));
    bus.pump();

    SenseReading mine = bus.observe(pid, "Health", 1);
    REQUIRE(mine);
    // The reader owns a copy. There is no lvalue path from here into the
    // producer's state at all — `other.sense.hp = 9000` has no spelling — and
    // scribbling on the copy proves the copy is a copy.
    mine.value->set("hp", Cell::integer(9000));
    CHECK(hp_of(mine) == 9000);

    // The repository, and therefore every other reader, is untouched.
    CHECK(hp_of(bus.observe(pid, "Health", 1)) == 90);
    // ...and so is the producer.
    Unverified u = parse(bus.snapshot_bytes(pid));
    Admission a = admit(u, schema_of<ProducerState>());
    REQUIRE(a.ok());
    CHECK(a.value().get("hp")->as_int() == 90);
}

TEST_CASE("a newer claim always wins: revisions advance and an older value never reappears") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;

    std::int64_t last = 0;
    for (int i = 1; i <= 20; ++i) {
        bus.send(pid, Message(to_value(Damage{1})));
        bus.pump();
        SenseReading r = bus.observe(pid, "Health", 1);
        REQUIRE(r);
        CHECK(r.by.revision == static_cast<std::uint64_t>(i));
        if (i > 1) {
            CHECK(hp_of(r) < last); // strictly newer, never a resurrected older value
        }
        last = hp_of(r);
    }
}

TEST_CASE("a malformed claim is refused by the same one gate, and does not replace a good one") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    bus.send(pid, Message(to_value(Damage{1})));
    bus.pump();
    REQUIRE(bus.observe(pid, "Health", 1).by.revision == 1);

    // 'hp' deliberately absent — the value claims Health v1 and does not conform.
    const SenseClaimResult bad = bus.claim_as(pid, Value(schema_of<Health>()));
    CHECK_FALSE(bad.accepted);
    CHECK(bad.why == SenseRefusal::GateRefused);
    // The good claim is still there, at its own revision.
    SenseReading r = bus.observe(pid, "Health", 1);
    REQUIRE(r);
    CHECK(r.by.revision == 1);
    CHECK(hp_of(r) == 99);
}

TEST_CASE("Senses generate no bus traffic: claiming and observing enqueue nothing") {
    Switchboard bus;
    auto [pid, producer] = put<Producer>(bus, Grant{}, "");
    (void)producer;
    auto [rid, reader] = put<Reader>(bus, observer_of_health(), "");
    reader->watch = pid;

    std::size_t events = 0;
    bus.add_observer([&events](const BusEvent&) { ++events; });

    bus.send(pid, Message(to_value(Damage{1})));
    bus.send(rid, Message(to_value(ReaderTick{1})));
    bus.pump();

    // Exactly two deliveries happened — the two ordinary messages. The claim and
    // the observation added no envelope, no seq, and no event. That is the
    // difference between a Sense and an ask/answer round trip.
    CHECK(events == 2);
    CHECK(bus.pending() == 0);
    CHECK(reader->seen.size() == 1);
}

} // TEST_SUITE
