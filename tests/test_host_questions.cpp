// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

// The facts a host's weave needs about other participants, each asked by message and answered as
// a fact: a role's holder whoever it is (QueryRole version 2), one participant's life and
// incarnation (QueryParticipant), and what an actor may do now (the authority reader). None of
// the answers hands its asker anything it could spend.

#include "switchboard_fixtures.hpp"

#include <zen/host/authority_reader.hpp>
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

struct Ping {
    ZEN_SHAPE(Ping, 1);
};
struct Pong {
    ZEN_SHAPE(Pong, 1);
};
struct Tally {
    std::int64_t n = 0;
    ZEN_SHAPE(Tally, 1, ZEN_FIELD(n));
};
struct NativeState {
    std::int64_t n = 0;
    ZEN_SHAPE(NativeState, 1, ZEN_FIELD(n));
};

/// A holder mounted natively, in a role: it declares it emits Pong and Tally, in that order.
class Native : public WeaveBase<Native, NativeState, Accept<Ping>, Emit<Pong, Tally>> {
public:
    void on(const Ping&, Mail& mail) { mail.send(mail.sender(), Pong{}); }
};

struct Rig {
    Switchboard bus;
    Kernel kernel{bus, fixture_admission()};
    WeaveId control = mount_control(kernel, bus);
};

/// A participant that asks under the grant it is given and keeps every answer it hears.
struct Asker {
    Registered probe;
    std::vector<Value> got;

    Asker(Switchboard& bus, Grant grant)
        : probe(register_probe(bus,
                               {schema_of<RoleInfo>(), schema_of<RoleInfoV3>(),
                                schema_of<ParticipantInfo>(), schema_of<host::AuthorityAnswered>(),
                                schema_of<Result>(), schema_of<Refused>()},
                               2, true, std::move(grant))) {
        probe.weave->on_handle = [this](const Message& in, Bus&, ProbeWeave&) {
            got.push_back(in.payload);
        };
    }

    template <class T>
    Ticket ask(Switchboard& bus, WeaveId door, const T& shape) {
        const Ticket t = bus.send_as(probe.id, door, Message(to_value(shape), WeaveId{}, probe.id, 1));
        bus.drain_until_idle();
        return t;
    }
};

WeaveId mount_native(Switchboard& bus, const std::string& role) {
    auto w = std::make_unique<Native>();
    Native* raw = w.get();
    const WeaveId id = bus.register_weave(std::move(w), emit_default_grant(*raw), role);
    raw->zen_set_self(id);
    return id;
}

std::vector<std::string> names_of(const RoleInfoV3& info) {
    std::vector<std::string> out;
    for (const DeclaredShape& s : info.emitted) {
        out.push_back(s.name + " v" + std::to_string(s.version));
    }
    return out;
}

} // namespace

TEST_SUITE("host_questions") {

TEST_CASE("QueryRole version 2 answers a natively mounted holder as the holder it is, where "
          "version 1 answers none") {
    Rig r;
    const WeaveId native = mount_native(r.bus, "native.role");
    Asker asker(r.bus, query_capability(r.control));

    asker.ask(r.bus, r.control, QueryRole{"native.role"});
    asker.ask(r.bus, r.control, QueryRoleV2{"native.role"});
    REQUIRE(asker.got.size() == 2);
    // Version 1 is a steward's: no weave this Kernel loaded holds the role.
    CHECK(from_value<RoleInfo>(asker.got[0]).holder == 0);

    REQUIRE(asker.got[1].schema().version() == 3);
    const RoleInfoV3 info = from_value<RoleInfoV3>(asker.got[1]);
    CHECK(info.holder == static_cast<std::int64_t>(native.value));
    CHECK_FALSE(info.loaded);
    CHECK_FALSE(info.converses);
    CHECK(info.roles == std::vector<std::string>{"native.role"});
    CHECK(info.incarnation == static_cast<std::int64_t>(r.bus.participant(native).incarnation));
    // What it declared it emits, in its declared order: the bus's own record of the declaration.
    CHECK(names_of(info) == std::vector<std::string>{"Pong v1", "Tally v1"});
}

TEST_CASE("QueryRole version 2 answers a holder this Kernel loaded, with what its image "
          "declares it emits, and an unheld role as no holder") {
    Rig r;
    // The image that declares it emits Greet v1, loaded into two roles.
    REQUIRE(r.kernel.load("pair", ZEN_SO_EMITS, std::vector<std::string>{"pair.one", "pair.two"})
                .ok);
    const WeaveId pair = r.kernel.weave_id("pair");
    Asker asker(r.bus, query_capability(r.control));

    asker.ask(r.bus, r.control, QueryRoleV2{"pair.two"});
    asker.ask(r.bus, r.control, QueryRoleV2{"nobody.holds.this"});
    REQUIRE(asker.got.size() == 2);
    const RoleInfoV3 info = from_value<RoleInfoV3>(asker.got[0]);
    CHECK(info.holder == static_cast<std::int64_t>(pair.value));
    CHECK(info.loaded);
    CHECK(info.roles == std::vector<std::string>{"pair.one", "pair.two"});
    CHECK(info.incarnation == static_cast<std::int64_t>(r.bus.participant(pair).incarnation));
    std::vector<std::string> declared;
    for (const auto& s : r.bus.emitted_schemas(pair)) {
        declared.push_back(s->name() + " v" + std::to_string(s->version()));
    }
    CHECK(declared == std::vector<std::string>{"Greet v1"});
    CHECK(names_of(info) == declared);

    const RoleInfoV3 none = from_value<RoleInfoV3>(asker.got[1]);
    CHECK(none.holder == 0);
    CHECK_FALSE(none.loaded);
    CHECK(none.roles.empty());
    CHECK(none.incarnation == 0);
    CHECK(none.emitted.empty());
}

TEST_CASE("QueryParticipant answers a participant's life and incarnation now: a revived one "
          "at its new life, an ended one as not present, an unknown id as nothing") {
    Rig r;
    const WeaveId native = mount_native(r.bus, "native.role");
    Asker asker(r.bus, query_capability(r.control));
    const auto id = static_cast<std::int64_t>(native.value);

    asker.ask(r.bus, r.control, QueryParticipant{id});
    REQUIRE(asker.got.size() == 1);
    const ParticipantInfo before = from_value<ParticipantInfo>(asker.got[0]);
    CHECK(before.participant == id);
    CHECK(before.present);
    CHECK_FALSE(before.loaded);
    CHECK(before.life == static_cast<std::int64_t>(r.bus.participant(native).life));
    CHECK(before.incarnation == static_cast<std::int64_t>(r.bus.participant(native).incarnation));
    CHECK(before.roles == std::vector<std::string>{"native.role"});

    // Killed and revived: the same id, a new life, which the answer says.
    const std::string bytes = r.bus.snapshot_bytes(native);
    r.bus.kill(native);
    asker.ask(r.bus, r.control, QueryParticipant{id});
    REQUIRE(asker.got.size() == 2);
    CHECK_FALSE(from_value<ParticipantInfo>(asker.got[1]).present);
    CHECK(from_value<ParticipantInfo>(asker.got[1]).roles.empty());
    REQUIRE(r.bus.reload(native, bytes).revived);
    asker.ask(r.bus, r.control, QueryParticipant{id});
    REQUIRE(asker.got.size() == 3);
    const ParticipantInfo after = from_value<ParticipantInfo>(asker.got[2]);
    CHECK(after.present);
    CHECK(after.life == static_cast<std::int64_t>(r.bus.participant(native).life));
    CHECK(after.life != before.life);

    // An id the bus never registered, and one that is no id at all.
    asker.ask(r.bus, r.control, QueryParticipant{987654});
    asker.ask(r.bus, r.control, QueryParticipant{-3});
    REQUIRE(asker.got.size() == 5);
    for (std::size_t i = 3; i < 5; ++i) {
        const ParticipantInfo none = from_value<ParticipantInfo>(asker.got[i]);
        CHECK_FALSE(none.present);
        CHECK(none.life == 0);
        CHECK(none.incarnation == 0);
    }
}

TEST_CASE("the questions are their own grant: a weave granted them drives no load, and a weave "
          "not granted them is refused at its send") {
    Rig r;
    (void)mount_native(r.bus, "native.role");
    std::vector<TapRecord> tap;
    r.bus.add_observer([&](const BusEvent& e) { tap.push_back(to_record(e)); });

    Asker questions(r.bus, query_capability(r.control));
    questions.ask(r.bus, r.control, LoadLibrary{"x", ZEN_SO_WEAVE, {}, {}});
    CHECK(questions.got.empty());
    CHECK_FALSE(r.kernel.is_loaded("x"));

    Asker nothing(r.bus, Grant::nothing());
    nothing.ask(r.bus, r.control, QueryRoleV2{"native.role"});
    nothing.ask(r.bus, r.control, QueryParticipant{1});
    CHECK(nothing.got.empty());
    int denied = 0;
    for (const TapRecord& t : tap) {
        if (t.kind == EventKind::Refused && t.reason == RefusalReason::CapabilityDenied &&
            (t.schema == "LoadLibrary" || t.schema == "QueryRole" ||
             t.schema == "QueryParticipant")) {
            ++denied;
        }
    }
    CHECK(denied == 3);
}

// ---- the authority reader ---------------------------------------------------

/// A subject whose baseline lets it send Ping to role `pane.role` at the door `pane.role/notes`.
WeaveId mount_subject(Switchboard& bus, const Door& door) {
    Grant g;
    g.allow_to_role("Ping", 1, "pane.role", door);
    return register_probe(bus, {schema_of<Ping>()}, 2, true, std::move(g)).id;
}

TEST_CASE("the authority reader tells a granted asker what a participant may do now, as a "
          "fact: permitted at its door, not elsewhere, and not once it has left") {
    Switchboard bus;
    const auto door = Door::parse("pane.role/notes");
    REQUIRE(door);
    const WeaveId subject = mount_subject(bus, *door);
    const WeaveId reader = mount_granted<host::AuthorityReader>(bus, host::authority_reader_grant(),
                                                                bus);
    Grant may_ask;
    may_ask.allow(host::AuthorityAsked::zen_name, host::AuthorityAsked::zen_version, reader);
    Asker asker(bus, may_ask);
    const auto s = static_cast<std::int64_t>(subject.value);

    asker.ask(bus, reader, host::AuthorityAsked{s, "Ping", 1, "pane.role", "pane.role/notes"});
    asker.ask(bus, reader, host::AuthorityAsked{s, "Ping", 1, "pane.role", "pane.role/other"});
    asker.ask(bus, reader, host::AuthorityAsked{s, "Pong", 1, "pane.role", "pane.role/notes"});
    asker.ask(bus, reader, host::AuthorityAsked{s, "", 0, "", ""});
    REQUIRE(asker.got.size() == 4);
    const auto answer = [&](std::size_t i) {
        REQUIRE(asker.got[i].schema().name() == "zen.AuthorityAnswered");
        return from_value<host::AuthorityAnswered>(asker.got[i]);
    };
    CHECK(answer(0).subject == s);
    CHECK(answer(0).present);
    CHECK(answer(0).permits);
    CHECK_FALSE(answer(1).permits);
    CHECK_FALSE(answer(2).permits);
    // Presence alone.
    CHECK(answer(3).present);
    CHECK_FALSE(answer(3).permits);

    // Gone, it is not present and permits nothing; an id never registered, the same.
    bus.kill(subject);
    asker.ask(bus, reader, host::AuthorityAsked{s, "Ping", 1, "pane.role", "pane.role/notes"});
    asker.ask(bus, reader, host::AuthorityAsked{424242, "Ping", 1, "pane.role", ""});
    REQUIRE(asker.got.size() == 6);
    for (std::size_t i = 4; i < 6; ++i) {
        CHECK_FALSE(answer(i).present);
        CHECK_FALSE(answer(i).permits);
    }
}

TEST_CASE("the authority reader refuses what names no door or no version, and a weave not "
          "granted the ask learns nothing") {
    Switchboard bus;
    const auto door = Door::parse("pane.role/notes");
    REQUIRE(door);
    const WeaveId subject = mount_subject(bus, *door);
    const WeaveId reader = mount_granted<host::AuthorityReader>(bus, host::authority_reader_grant(),
                                                                bus);
    Grant may_ask;
    may_ask.allow(host::AuthorityAsked::zen_name, host::AuthorityAsked::zen_version, reader);
    Asker asker(bus, may_ask);
    const auto s = static_cast<std::int64_t>(subject.value);

    asker.ask(bus, reader, host::AuthorityAsked{s, "Ping", 1, "pane.role", "not a door"});
    asker.ask(bus, reader, host::AuthorityAsked{s, "Ping", -1, "pane.role", ""});
    REQUIRE(asker.got.size() == 2);
    CHECK(asker.got[0].get("reason")->as_text() == "'not a door' is not a door");
    CHECK(asker.got[1].get("reason")->as_text() == "version -1 is no shape's version");

    std::vector<TapRecord> tap;
    bus.add_observer([&](const BusEvent& e) { tap.push_back(to_record(e)); });
    Asker stranger(bus, Grant::nothing());
    stranger.ask(bus, reader, host::AuthorityAsked{s, "Ping", 1, "pane.role", "pane.role/notes"});
    CHECK(stranger.got.empty());
    bool denied = false;
    for (const TapRecord& t : tap) {
        denied = denied || (t.schema == "zen.AuthorityAsked" && t.kind == EventKind::Refused &&
                            t.reason == RefusalReason::CapabilityDenied);
    }
    CHECK(denied);
}

TEST_CASE("the authority reader reads authority delegated since admission, and its answer "
          "changes nothing the subject may do") {
    Switchboard bus;
    const WeaveId subject = register_probe(bus, {schema_of<Ping>()}, 2, true, Grant::nothing()).id;
    const WeaveId reader = mount_granted<host::AuthorityReader>(bus, host::authority_reader_grant(),
                                                                bus);
    Grant may_ask;
    may_ask.allow(host::AuthorityAsked::zen_name, host::AuthorityAsked::zen_version, reader);
    Asker asker(bus, may_ask);
    const auto s = static_cast<std::int64_t>(subject.value);
    const host::AuthorityAsked ping{s, "Ping", 1, "pane.role", ""};

    asker.ask(bus, reader, ping);
    REQUIRE(asker.got.size() == 1);
    CHECK_FALSE(from_value<host::AuthorityAnswered>(asker.got[0]).permits);

    // An administrator delegates the role send live; the next ask reads it.
    const GrantAuthority cap =
        host_grant_authority(bus, subject, LiveAuthority{}.allow_to_role("Ping", 1, "pane.role"));
    Registered admin = register_probe(bus, {schema_of<Ping>()}, 2, true, Grant::nothing());
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        (void)b.delegate_authority(cap, LiveAuthority{}.allow_to_role("Ping", 1, "pane.role"));
    };
    bus.send(admin.id, Message(to_value(Ping{})));
    bus.drain_until_idle();
    asker.ask(bus, reader, ping);
    REQUIRE(asker.got.size() == 2);
    CHECK(from_value<host::AuthorityAnswered>(asker.got[1]).permits);

    // Reading installed nothing: the subject's authority is what the administrator delegated,
    // and the reader's own counters are all it keeps.
    CHECK(bus.weave(reader)->snapshot().get("asked")->as_int() == 2);
}

} // TEST_SUITE
