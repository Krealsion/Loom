// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

// Loom's participants claim (zen/host/participants.hpp): who is on the bus, the roles each holds,
// whether it is alive, its life and incarnation, and what it accepts and emits, read by a granted
// weave inside its handler. Every case reads through a real reader's delivery, as a weave would,
// and asserts the bus's own tables, never a value the test wrote: the claim is derived at each
// read (SENSE-08), so a change shows at the next read with no claim made since.

#include "switchboard_fixtures.hpp"

#include <zen/host/lifecycle_wiring.hpp>
#include <zen/host/participants.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace loom::host;
using namespace sbfx;

namespace {

struct Look {
    ZEN_SHAPE(Look, 1);
};
struct Lamp {
    std::int64_t level = 0;
    ZEN_SHAPE(Lamp, 1, ZEN_FIELD(level));
};
struct Lit {
    ZEN_SHAPE(Lit, 1);
};
struct LeaveRole {
    std::string role;
    ZEN_SHAPE(LeaveRole, 1, ZEN_FIELD(role));
};
struct ReaderState {
    std::int64_t looks = 0;
    ZEN_SHAPE(ReaderState, 1, ZEN_FIELD(looks));
};
struct LampState {
    std::int64_t level = 0;
    ZEN_SHAPE(LampState, 1, ZEN_FIELD(level));
};

/// A reader, as a weave reads: inside its handler, keeping nothing past it but what the test
/// inspects. `LeaveRole` reads, releases one of its own roles and reads again in the same handler,
/// so the second read meets the world its own act just made.
class Reader : public WeaveBase<Reader, ReaderState, Accept<Look, LeaveRole>> {
public:
    void on(const Look&, Mail& mail) {
        ++state_.looks;
        reading = mail.latest_from_role<Participants>(kParticipantsRole);
        table = read_participants(mail);
    }
    void on(const LeaveRole& m, Mail& mail) {
        before = read_participants(mail);
        released = mail.release_role(m.role) == RoleRelease::Released;
        table = read_participants(mail);
    }
    SenseReading reading{};
    std::optional<Participants> before;
    std::optional<Participants> table;
    bool released = false;
};

/// A holder mounted natively: woven, so it carries the substrate doors beside its own.
class LampWeave : public WeaveBase<LampWeave, LampState, Accept<Lamp>, Emit<Lit>> {
public:
    void on(const Lamp& m, Mail&) { state_.level = m.level; }
};

/// A weave that claims a shape of the same name as Loom's, personally and as its own role.
class Imposter : public WeaveBase<Imposter, ReaderState, Accept<Look>, Emit<>,
                                  Claims<Participants>> {
public:
    void on(const Look&, Mail& mail) {
        Participants forged;
        forged.count = 999;
        mine = mail.claim(forged);
        as_role = mail.as_role("imposter").claim(forged);
    }
    SenseClaimResult mine{};
    SenseClaimResult as_role{};
};

template <class W>
std::pair<WeaveId, W*> put(Switchboard& bus, Grant grant, std::vector<std::string> roles) {
    auto owned = std::make_unique<W>();
    W* raw = owned.get();
    const WeaveId id = bus.register_weave(std::move(owned), std::move(grant), std::move(roles));
    raw->zen_set_self(id);
    return {id, raw};
}

Grant reader_grant() {
    Grant g;
    allow_observe_participants(g);
    return g;
}

struct Rig {
    Switchboard bus;
    WeaveId loom = mount_participants(bus);
    std::pair<WeaveId, Reader*> reader = put<Reader>(bus, reader_grant(), {});

    /// The reader's reading now, taken in its own delivery.
    const Participants& look() {
        bus.send(reader.first, Message(to_value(Look{})));
        bus.drain_until_idle();
        REQUIRE(reader.second->table.has_value());
        return *reader.second->table;
    }
};

std::vector<std::string> names(const std::vector<DeclaredShape>& shapes) {
    std::vector<std::string> out;
    for (const DeclaredShape& s : shapes) {
        out.push_back(s.name + " v" + std::to_string(s.version));
    }
    return out;
}

/// The row a lookup found, required: a missing row is a failed case, never a crash.
const Participant& row_of(const Participant* row) {
    REQUIRE(row != nullptr);
    return *row;
}

bool lists(const std::vector<DeclaredShape>& shapes, const std::string& name) {
    for (const DeclaredShape& s : shapes) {
        if (s.name + " v" + std::to_string(s.version) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_SUITE("participants") {

TEST_CASE("a holder mounted natively is read as the holder it is: its id, its roles, its life and "
          "incarnation, and what it accepts and declares it emits") {
    Rig r;
    auto [lamp, raw] = put<LampWeave>(r.bus, Grant{}, {"lamp", "light"});
    (void)raw;

    const Participants& t = r.look();
    CHECK(t.whole);
    CHECK(t.count == static_cast<std::int64_t>(t.rows.size()));
    const Participant* row = t.holder_of("light");
    REQUIRE(row != nullptr);
    CHECK(row->id == static_cast<std::int64_t>(lamp.value));
    CHECK(row->roles == std::vector<std::string>{"lamp", "light"});
    CHECK(row->alive);
    CHECK(row->life == static_cast<std::int64_t>(r.bus.participant(lamp).life));
    CHECK(row->incarnation == static_cast<std::int64_t>(r.bus.participant(lamp).incarnation));
    CHECK_FALSE(row->accepts_any);
    // Its accept-set as the bus matches deliveries against, in its order, and its emit-set.
    CHECK(names(row->accepts) == names(row_of(t.find(lamp)).accepts));
    std::vector<std::string> bus_accepts;
    for (const auto& s : r.bus.accepted_schemas(lamp)) {
        bus_accepts.push_back(s->name() + " v" + std::to_string(s->version()));
    }
    CHECK(names(row->accepts) == bus_accepts);
    CHECK(lists(row->accepts, "Lamp v1"));
    CHECK(names(row->emits) == std::vector<std::string>{"Lit v1"});
    CHECK(t.holder_accepts("lamp", *schema_of<Lamp>()));
    CHECK_FALSE(t.holder_accepts("lamp", *schema_of<Lit>()));
    CHECK(t.holder_emits("light", *schema_of<Lit>()));
    CHECK_FALSE(t.holder_emits("light", *schema_of<Lamp>()));

    // The claimant is on the bus too, in Loom's role, and the reading says who claimed.
    CHECK(row_of(t.holder_of(kParticipantsRole)).id == static_cast<std::int64_t>(r.loom.value));
    CHECK(r.reader.second->reading.by.author == r.loom);
    CHECK(r.reader.second->reading.by.role == kParticipantsRole);
    CHECK(r.reader.second->reading.by.role_holder_is_current);
}

TEST_CASE("a weave holding no role has its row, by its id, with no roles") {
    Rig r;
    const Registered loose = register_probe(r.bus, {ping_schema()});

    const Participants& t = r.look();
    const Participant* row = t.find(loose.id);
    REQUIRE(row != nullptr);
    CHECK(row->roles.empty());
    CHECK(row->alive);
    // And the reader itself, which holds none either.
    REQUIRE(t.find(r.reader.first) != nullptr);
    CHECK(row_of(t.find(r.reader.first)).roles.empty());
}

TEST_CASE("a weave built by hand is read by its accept-set and emit-set, though it answers no "
          "describe door") {
    Rig r;
    auto owned = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{greet_schema(), ping_schema()});
    owned->declared_emits = {pong_schema()};
    const WeaveId hand =
        r.bus.register_weave(std::move(owned), Grant{}, std::string("hand.made"));

    const Participants& t = r.look();
    const Participant* row = t.holder_of("hand.made");
    REQUIRE(row != nullptr);
    CHECK(row->id == static_cast<std::int64_t>(hand.value));
    // Exactly what it declared, in its order: no describe door, and nothing invented.
    CHECK(names(row->accepts) == std::vector<std::string>{"Greet v1", "Ping v1"});
    CHECK(names(row->emits) == std::vector<std::string>{"Pong v1"});
    CHECK_FALSE(lists(row->accepts, std::string(kDescribeAcceptedShapeName) + " v1"));
    CHECK(t.holder_accepts("hand.made", *ping_schema()));
}

TEST_CASE("a weave accepting any registered shape says so beside its list") {
    Rig r;
    auto owned = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{});
    const WeaveId any =
        r.bus.register_weave(std::move(owned), Grant{}, AcceptMode::AnyRegistered);

    const Participants& t = r.look();
    REQUIRE(t.find(any) != nullptr);
    CHECK(row_of(t.find(any)).accepts_any);
    CHECK_FALSE(row_of(t.find(r.reader.first)).accepts_any);
}

TEST_CASE("a role moved between two live holders is read at its new holder at the next read, with "
          "no claim made since; the sealed candidate has no row until it is committed") {
    Rig r;
    const Registered coordinator = register_probe(r.bus, {ping_schema()});
    const WeaveId holder =
        r.bus.register_weave(std::make_unique<ProbeWeave>(
                                 std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
                             Grant{}, std::string("station"));
    const Registered candidate = register_probe(r.bus, {ping_schema()});
    REQUIRE(r.bus.seal_weave(candidate.id, coordinator.id));
    {
        const Participants& t = r.look();
        REQUIRE(t.holder_of("station") != nullptr);
        CHECK(t.holder_of("station")->id == static_cast<std::int64_t>(holder.value));
        CHECK(t.find(candidate.id) == nullptr); // outside the world while sealed
    }
    const std::uint64_t revision = r.reader.second->reading.by.revision;

    REQUIRE(r.bus.commit_candidate(candidate.id, holder, "station"));
    const Participants& t = r.look();
    REQUIRE(t.holder_of("station") != nullptr);
    CHECK(t.holder_of("station")->id == static_cast<std::int64_t>(candidate.id.value));
    REQUIRE(t.find(holder) != nullptr);
    CHECK(t.find(holder)->roles.empty());
    // Nothing claimed since: the same claim, read again, made from the table as it is now.
    CHECK(r.reader.second->reading.by.revision == revision);
}

TEST_CASE("a weave replaced in place is read at its new incarnation, its life standing") {
    Rig r;
    auto [lamp, raw] = put<LampWeave>(r.bus, Grant{}, {"lamp"});
    (void)raw;
    const std::int64_t before = row_of(r.look().holder_of("lamp")).incarnation;
    const std::int64_t life = row_of(r.look().holder_of("lamp")).life;

    REQUIRE(r.bus.swap_state(lamp, serialize(to_value(LampState{3}))).revived);
    const Participants& t = r.look();
    REQUIRE(t.holder_of("lamp") != nullptr);
    CHECK(t.holder_of("lamp")->id == static_cast<std::int64_t>(lamp.value));
    CHECK(t.holder_of("lamp")->incarnation == before + 1);
    CHECK(t.holder_of("lamp")->life == life);
}

TEST_CASE("a participant that died is read not alive, and one that ended has no row and holds "
          "no role") {
    Rig r;
    auto [lamp, raw] = put<LampWeave>(r.bus, Grant{}, {"lamp"});
    (void)raw;
    REQUIRE(row_of(r.look().holder_of("lamp")).alive);

    r.bus.kill(lamp);
    REQUIRE(r.look().find(lamp) != nullptr);
    CHECK_FALSE(row_of(r.look().find(lamp)).alive);

    const std::int64_t count = r.look().count;
    REQUIRE(r.bus.unregister_weave(lamp) != nullptr);
    const Participants& t = r.look();
    CHECK(t.find(lamp) == nullptr);
    CHECK(t.holder_of("lamp") == nullptr);
    CHECK(t.count == count - 1);
}

TEST_CASE("a read inside the handler that released a role meets the table that release left, "
          "where the read before it in that handler met the role held") {
    Rig r;
    // A reader in a role of its own, which it releases and reads in one delivery.
    auto [reader, raw] = put<Reader>(r.bus, reader_grant(), {"desk"});
    r.bus.send(reader, Message(to_value(LeaveRole{"desk"})));
    r.bus.drain_until_idle();
    REQUIRE(raw->released);
    REQUIRE(raw->before.has_value());
    REQUIRE(raw->before->holder_of("desk") != nullptr);
    CHECK(raw->before->holder_of("desk")->id == static_cast<std::int64_t>(reader.value));
    REQUIRE(raw->table.has_value());
    CHECK(raw->table->holder_of("desk") == nullptr);
    REQUIRE(raw->table->find(reader) != nullptr);
    CHECK(raw->table->find(reader)->roles.empty());
}

TEST_CASE("a reader not granted the claim is refused and learns nothing, a rule for another shape "
          "included") {
    Rig r;
    auto [bare, bare_raw] = put<Reader>(r.bus, Grant{}, {});
    auto [other, other_raw] = put<Reader>(r.bus, Grant{}.allow_observe("Lamp", 1), {});
    r.bus.send(bare, Message(to_value(Look{})));
    r.bus.send(other, Message(to_value(Look{})));
    r.bus.drain_until_idle();
    for (Reader* refused : {bare_raw, other_raw}) {
        CHECK(refused->reading.refusal == SenseRefusal::NotAuthorized);
        CHECK_FALSE(refused->reading.value.has_value());
        CHECK_FALSE(refused->table.has_value());
    }
    // The granted reader, beside them, reads it.
    CHECK(r.look().count >= 4);
}

TEST_CASE("another weave's claim of a same-named shape is never read as Loom's, and the role "
          "cannot be taken while Loom's claimant holds it") {
    Rig r;
    auto [imp, imposter] = put<Imposter>(r.bus, Grant{}, {"imposter"});
    r.bus.send(imp, Message(to_value(Look{})));
    r.bus.drain_until_idle();
    REQUIRE(imposter->mine.accepted);
    REQUIRE(imposter->as_role.accepted);

    const Participants& t = r.look();
    CHECK(t.count != 999);
    CHECK(t.find(imp) != nullptr);
    CHECK(r.reader.second->reading.by.author == r.loom);

    // A second holder of Loom's role is refused at its registration, as a load naming it is.
    CHECK_THROWS_AS(put<Imposter>(r.bus, Grant{}, {kParticipantsRole}), std::invalid_argument);
    CHECK(row_of(r.look().holder_of(kParticipantsRole)).id ==
          static_cast<std::int64_t>(r.loom.value));

    // With no claimant mounted there is no Loom's claim, whatever another weave claims.
    Switchboard bare;
    auto [reader, raw] = put<Reader>(bare, reader_grant(), {});
    auto [imp2, imposter2] = put<Imposter>(bare, Grant{}, {"imposter"});
    bare.send(imp2, Message(to_value(Look{})));
    bare.send(reader, Message(to_value(Look{})));
    bare.drain_until_idle();
    REQUIRE(imposter2->as_role.accepted);
    CHECK(raw->reading.refusal == SenseRefusal::NoClaim);
    CHECK_FALSE(raw->table.has_value());
}

TEST_CASE("a registration naming Loom's participants role is refused on a bus with no claimant, "
          "binding none of its set, and the claimant's own mount still binds it") {
    Switchboard bus;
    const std::size_t before = bus.list_weaves().size();
    // Nobody holds the role here, so only the role being Loom's own can refuse it: a refusal for
    // a held role would pass this case without the reservation.
    REQUIRE_FALSE(bus.role_holder(kParticipantsRole).valid());
    CHECK_THROWS_AS(put<Imposter>(bus, Grant{}, {kParticipantsRole}), std::invalid_argument);
    CHECK_THROWS_AS(put<Imposter>(bus, Grant{}, {"imposter", kParticipantsRole}),
                    std::invalid_argument);
    CHECK_FALSE(bus.role_holder(kParticipantsRole).valid());
    CHECK_FALSE(bus.role_holder("imposter").valid());
    CHECK(bus.list_weaves().size() == before);

    const WeaveId loom = mount_participants(bus);
    CHECK(bus.role_holder(kParticipantsRole) == loom);
    CHECK(bus.roles_of(loom) == std::vector<std::string>{kParticipantsRole});
    auto [reader, raw] = put<Reader>(bus, reader_grant(), {});
    bus.send(reader, Message(to_value(Look{})));
    bus.drain_until_idle();
    REQUIRE(raw->table.has_value());
    CHECK(raw->reading.by.author == loom);

    // The claimant ended, the role is free and still refused to anyone else; a new mount binds it.
    REQUIRE(bus.unregister_weave(loom));
    CHECK_THROWS_AS(put<Imposter>(bus, Grant{}, {kParticipantsRole}), std::invalid_argument);
    CHECK_FALSE(bus.role_holder(kParticipantsRole).valid());
    const WeaveId again = mount_participants(bus);
    CHECK(bus.role_holder(kParticipantsRole) == again);
}

TEST_CASE("Loom's participants role is never moved off its claimant: a commit, an admission and a "
          "prepared replacement naming it are refused, and the claimant still holds it") {
    Rig r;
    const Registered coordinator = register_probe(r.bus, {ping_schema()});
    // The candidate accepts the activation and holds no role, so every other precondition of a
    // move holds: what refuses it is the role.
    const Registered candidate = register_probe(r.bus, {schema_of<Activated>(), ping_schema()});
    REQUIRE(r.bus.seal_weave(candidate.id, coordinator.id));
    const std::string role(kParticipantsRole);

    CHECK_FALSE(r.bus.commit_candidate(candidate.id, r.loom, role));
    CHECK(r.bus.role_holder(kParticipantsRole) == r.loom);

    const AdmitResult admitted =
        r.bus.admit_candidate(candidate.id, r.loom, role, host_lifecycle_authority(r.bus),
                              Message(to_value(Activated{1})), 1);
    CHECK_FALSE(admitted.scheduled);
    CHECK(admitted.why == AdmitRefusal::RoleNotHeld);
    r.bus.drain_until_idle();
    CHECK(r.bus.role_holder(kParticipantsRole) == r.loom);

    const TxnResult begun = r.bus.begin_prepared_replacement(coordinator.id, coordinator.id,
                                                             r.loom, candidate.id, role, 4);
    CHECK_FALSE(begun.ok);
    CHECK(begun.why == TxnReason::PreconditionFailed);
    CHECK(r.bus.role_holder(kParticipantsRole) == r.loom);
    CHECK(row_of(r.look().holder_of(kParticipantsRole)).id ==
          static_cast<std::int64_t>(r.loom.value));
}

TEST_CASE("a table past what one decode admits is refused whole, its count kept, and is whole "
          "again once it fits") {
    Rig r;
    // A hundred shapes each of three hundred weaves accepts: past kMaxDecodedCells as rows.
    std::vector<std::shared_ptr<const Schema>> many;
    for (int i = 0; i < 100; ++i) {
        many.push_back(SchemaBuilder("Bulk" + std::to_string(i), 1).field("n", Kind::Int).build());
    }
    std::vector<WeaveId> crowd;
    for (int i = 0; i < 300; ++i) {
        crowd.push_back(register_probe(r.bus, many).id);
    }
    r.bus.send(r.reader.first, Message(to_value(Look{})));
    r.bus.drain_until_idle();
    const SenseReading& over = r.reader.second->reading;
    REQUIRE(over);
    const Participants refused = from_value<Participants>(*over.value);
    CHECK_FALSE(refused.whole);
    CHECK(refused.rows.empty());
    CHECK(refused.count == 302);
    CHECK(decoded_cells(*over.value) <= kMaxDecodedCells);
    CHECK_FALSE(r.reader.second->table.has_value());

    for (std::size_t i = 0; i < 290; ++i) {
        REQUIRE(r.bus.unregister_weave(crowd[i]) != nullptr);
    }
    const Participants& t = r.look();
    CHECK(t.whole);
    CHECK(t.count == 12);
    CHECK(t.rows.size() == 12);
}

} // TEST_SUITE
