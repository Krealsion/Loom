// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#include "switchboard_fixtures.hpp"

#include <zen/host/grant_wiring.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>

#include <doctest.h>
#include <limits>
#include <stdexcept>

namespace loom {
struct DispatchRefusalProbe {
    static void exhaust_after_next(Switchboard& bus) {
        bus.next_seq_ = std::numeric_limits<std::uint64_t>::max();
    }
    static std::size_t envelope_bytes() { return sizeof(Switchboard::Envelope); }
};
} // namespace loom
using namespace loom;
using namespace sbfx;
namespace {
struct Heard {
    DispatchRefused notice;
    std::uint64_t correlation;
    bool trusted;
};
Registered reg_role(Switchboard& sb, std::vector<std::shared_ptr<const Schema>> doors,
                    std::string role, Grant grant = Grant{}.allow_any()) {
    auto ptr = std::make_unique<ProbeWeave>(std::move(doors), 4, true);
    auto raw = ptr.get();
    auto id = sb.register_weave(std::move(ptr), std::move(grant), std::move(role));
    return {id, raw};
}
Registered author(Switchboard& sb, std::vector<Heard>& heard, Grant grant = Grant{}.allow_any(),
                  std::string role = {}) {
    auto a = reg_role(sb, {schema_of<DispatchRefused>(), pong_schema()}, std::move(role),
                      std::move(grant));
    a.weave->on_handle = [&heard](const Message& in, Bus&, ProbeWeave&) {
        if (in.payload.schema().name() == DispatchRefused::zen_name) {
            heard.push_back({from_value<DispatchRefused>(in.payload), in.correlation,
                             in.provenance.dispatch_refused()});
            CHECK_FALSE(in.provenance.answers_ask());
        }
    };
    return a;
}
Ticket send_mode(Switchboard& sb, int mode, WeaveId from, WeaveId to, Value payload,
                 std::uint64_t correlation = 0) {
    Message msg(std::move(payload), WeaveId{991}, WeaveId{992}, correlation);
    switch (mode) {
    case 0:
        return sb.send_as(from, to, std::move(msg));
    case 1:
        return sb.send_as_to_role(from, "service.dispatch", std::move(msg));
    case 2:
        return sb.office_send_as(from, "author.office", to, std::move(msg));
    default:
        return sb.office_send_to_role_as(from, "author.office", "service.dispatch", std::move(msg));
    }
}
} // namespace

TEST_SUITE("dispatch_refusal") {
    TEST_CASE("each pre-handler refusal returns later for all four authored-address combinations") {
        for (int mode = 0; mode < 4; ++mode) {
            for (int reason = 0; reason < 5; ++reason) {
                CAPTURE(mode);
                CAPTURE(reason);
                Switchboard sb;
                std::vector<Heard> heard;
                auto target = reg_role(
                    sb, reason == 3 ? std::vector{pong_schema()} : std::vector{ping_schema()},
                    reason == 1 ? "" : "service.dispatch");
                Grant grant;
                if (reason != 0) {
                    grant.allow("Ping", 1, target.id).allow_to_role("Ping", 1, "service.dispatch");
                }
                auto a = author(sb, heard, std::move(grant), "author.office");
                if (reason == 2) {
                    sb.kill(target.id);
                }
                // The direct missing target must still be permitted, so its absence
                // cannot accidentally turn this case into the denial case.
                if (reason == 1 && mode % 2 == 0) {
                    REQUIRE(sb.unregister_weave(target.id));
                }
                const Ticket t = send_mode(sb, mode, a.id, target.id,
                                           reason == 4 ? malformed_ping() : ping(7), 77);
                REQUIRE(t.valid());
                REQUIRE(sb.pending() == 1);
                CHECK(heard.empty());
                CHECK(sb.pump_pending() == 1);
                static const RefusalReason expected[] = {
                    RefusalReason::CapabilityDenied, RefusalReason::NoSuchTarget,
                    RefusalReason::TargetUnavailable, RefusalReason::NotAccepted,
                    RefusalReason::GateRefused};
                REQUIRE(sb.outcome(t).refusal.reason == expected[reason]);
                if (!(reason == 1 && mode % 2 == 0)) {
                    CHECK(target.weave->handled_names.empty());
                }
                REQUIRE(heard.empty());
                REQUIRE(sb.pending() == 1);
                CHECK(sb.pump_pending() == 1);
                REQUIRE(heard.size() == 1);
                CHECK(heard[0].trusted);
                CHECK(heard[0].notice.refused_attempt().seq == t.seq);
                CHECK(heard[0].notice.reason == name_of(expected[reason]));
                CHECK(heard[0].notice.shape == "Ping");
                CHECK(heard[0].notice.version == 1);
                CHECK(heard[0].correlation == 77);
                CHECK(heard[0].notice.role == (mode % 2 ? "service.dispatch" : ""));
                CHECK(heard[0].notice.addressed_weave() == (mode % 2 ? WeaveId{} : target.id));
                CHECK(sb.pending() == 0);
            }
        }
    }

    TEST_CASE("delivered controls preserve personal and office authorship without a refusal") {
        for (int mode = 0; mode < 4; ++mode) {
            Switchboard sb;
            std::vector<Heard> heard;
            auto a = author(sb, heard, Grant{}.allow_any(), "author.office");
            auto target = reg_role(sb, {ping_schema()}, "service.dispatch");
            target.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
                CHECK(in.sender == a.id);
                CHECK(in.provenance.authored_from_role("author.office") == (mode >= 2));
            };
            Ticket t = send_mode(sb, mode, a.id, target.id, ping(3));
            sb.pump_pending();
            CHECK(sb.outcome(t).disposition == Disposition::Delivered);
            CHECK(target.weave->handled_names.size() == 1);
            CHECK(heard.empty());
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("acceptance opts in at authorship and publication does not opt in by accident") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = register_probe(sb, {pong_schema()}, 2, true, Grant{});
        auto target = register_probe(sb, {ping_schema()});
        sb.send_as(a.id, target.id, Message(ping(1)));
        sb.pump_pending();
        CHECK(sb.pending() == 0);
        CHECK(a.weave->handled_names.empty());
        auto opted = author(sb, heard, Grant{});
        REQUIRE(sb.publish_as(opted.id, Message(ping(2))) == 1);
        sb.pump_pending();
        CHECK(sb.pending() == 0);
        CHECK(heard.empty());
        // Root injection is not a weave's ordinary authorship, even with its id supplied.
        sb.send(WeaveId{919}, Message(ping(1), opted.id));
        sb.pump_pending();
        CHECK(sb.pending() == 0);
        CHECK(heard.empty());
    }

    TEST_CASE(
        "forged provenance is stripped and a real refusal creates no answer right or redirection") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard);
        auto redirect = register_probe(sb, {schema_of<DispatchRefused>()});
        auto forger = register_probe(sb, {pong_schema()});
        DispatchRefused fake;
        fake.attempt = "42";
        fake.target = "800";
        fake.shape = "Ping";
        fake.version = 1;
        fake.reason = "CapabilityDenied";
        forger.weave->on_handle = [&](const Message&, Bus& bus, ProbeWeave&) {
            Message m(to_value(fake));
            m.provenance = Provenance::attested(Provenance::Kind::DispatchRefusal, 0);
            bus.send(a.id, std::move(m));
        };
        sb.send(forger.id, Message(pong(0)));
        sb.pump_pending();
        sb.pump_pending();
        REQUIRE(heard.size() == 1);
        CHECK_FALSE(heard[0].trusted);
        heard.clear();
        a.weave->on_handle = [&](const Message& in, Bus& bus, ProbeWeave&) {
            REQUIRE(in.provenance.dispatch_refused());
            CHECK_FALSE(in.provenance.answers_ask());
            CHECK_FALSE(bus.make_deferred_answer().valid());
            CHECK_FALSE(bus.answer(Message(pong(1))).valid());
            heard.push_back({from_value<DispatchRefused>(in.payload), in.correlation, true});
        };
        sb.send_as(a.id, WeaveId{800}, Message(ping(1), forger.id, redirect.id, 42));
        sb.pump_pending();
        sb.pump_pending();
        REQUIRE(heard.size() == 1);
        CHECK(redirect.weave->handled_names.empty());
        CHECK(sb.pending() == 0);
    }

    TEST_CASE("zero and reused correlations identify distinct attempts for independent senders") {
        Switchboard sb;
        std::vector<Heard> ah, bh;
        auto a = author(sb, ah);
        auto b = author(sb, bh);
        std::vector<Ticket> at, bt;
        for (auto corr : {std::uint64_t{0}, std::uint64_t{77}, std::uint64_t{77}}) {
            at.push_back(sb.send_as(a.id, WeaveId{800}, Message(ping(1), {}, {}, corr)));
            bt.push_back(sb.send_as(b.id, WeaveId{800}, Message(ping(1), {}, {}, corr)));
        }
        REQUIRE(sb.pending() == 6);
        sb.pump_pending();
        CHECK(ah.empty());
        CHECK(bh.empty());
        REQUIRE(sb.pending() == 6);
        sb.pump_pending();
        REQUIRE(ah.size() == 3);
        REQUIRE(bh.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(ah[i].notice.refused_attempt().seq == at[i].seq);
            CHECK(bh[i].notice.refused_attempt().seq == bt[i].seq);
            CHECK(at[i].seq != bt[i].seq);
        }
        CHECK(ah[0].correlation == 0);
        CHECK(ah[1].correlation == ah[2].correlation);
    }

    TEST_CASE("sender lifetime is checked before decision and again while the notice is queued") {
        for (bool after_decision : {false, true}) {
            for (int transition = 0; transition < 4; ++transition) {
                CAPTURE(after_decision);
                CAPTURE(transition);
                Switchboard sb;
                std::vector<Heard> heard;
                auto a = author(sb, heard);
                sb.send_as(a.id, WeaveId{800}, Message(ping(1), {}, {}, 99));
                if (after_decision) {
                    sb.pump_pending();
                    REQUIRE(sb.pending() == 1);
                }
                const auto state = sb.snapshot_bytes(a.id);
                if (transition == 0) {
                    REQUIRE(sb.swap_state(a.id, state).revived);
                }
                if (transition == 1) {
                    sb.kill(a.id);
                }
                if (transition == 2) {
                    sb.kill(a.id);
                    REQUIRE(sb.reload(a.id, state).revived);
                }
                if (transition == 3) {
                    REQUIRE(sb.unregister_weave(a.id));
                }
                sb.pump_pending();
                CHECK(heard.empty());
                CHECK(sb.pending() == 0);
                if (transition == 0 || transition == 2) {
                    const auto fresh = sb.send_as(a.id, WeaveId{800}, Message(ping(2), {}, {}, 99));
                    sb.pump_pending();
                    sb.pump_pending();
                    REQUIRE(heard.size() == 1);
                    CHECK(heard[0].notice.refused_attempt().seq == fresh.seq);
                }
            }
        }
    }

    TEST_CASE("ordinary queued speech survives live reload while notice destination ignores office "
              "movement") {
        Switchboard sb;
        std::vector<Heard> ah, bh;
        auto a = author(sb, ah, Grant{}.allow_any(), "author.office");
        auto b = author(sb, bh);
        auto target = reg_role(sb, {ping_schema()}, "service.dispatch");
        const auto speech = sb.send_as(a.id, target.id, Message(ping(1)));
        REQUIRE(sb.swap_state(a.id, sb.snapshot_bytes(a.id)).revived);
        sb.pump_pending();
        CHECK(sb.outcome(speech).disposition == Disposition::Delivered);
        const auto refused =
            sb.office_send_as(a.id, "author.office", WeaveId{800}, Message(ping(2)));
        REQUIRE(sb.seal_weave(b.id, a.id));
        REQUIRE(sb.commit_candidate(b.id, a.id, "author.office"));
        sb.pump_pending();
        sb.pump_pending();
        REQUIRE(ah.size() == 1);
        CHECK(bh.empty());
        CHECK(ah[0].notice.refused_attempt().seq == refused.seq);
        const auto routed = sb.send_as_to_role(a.id, "service.dispatch", Message(ping(3)));
        auto other = register_probe(sb, {pong_schema()});
        REQUIRE(sb.seal_weave(other.id, a.id));
        REQUIRE(sb.commit_candidate(other.id, target.id, "service.dispatch"));
        sb.pump_pending();
        sb.pump_pending();
        CHECK(sb.outcome(routed).refusal.reason == RefusalReason::NotAccepted);
        REQUIRE(ah.size() == 2);
        CHECK(ah[1].notice.target.empty());
        CHECK(ah[1].notice.role == "service.dispatch");
        CHECK(other.weave->handled_names.empty());
    }

    TEST_CASE("denied authority cannot distinguish vacant sealed live and dead targets") {
        for (bool granted : {false, true}) {
            for (int state = 0; state < 4; ++state) {
                CAPTURE(granted);
                CAPTURE(state);
                Switchboard sb;
                std::vector<Heard> heard;
                auto target = register_probe(sb, {ping_schema()});
                Grant grant;
                if (granted) {
                    grant.allow("Ping", 1, target.id);
                }
                auto a = author(sb, heard, std::move(grant));
                auto owner = register_probe(sb, {pong_schema()});
                if (state == 0) {
                    REQUIRE(sb.unregister_weave(target.id));
                }
                if (state == 1) {
                    REQUIRE(sb.seal_weave(target.id, owner.id));
                }
                if (state == 3) {
                    sb.kill(target.id);
                }
                sb.send_as(a.id, target.id, Message(ping(1)));
                sb.pump_pending();
                sb.pump_pending();
                if (granted && state == 2) {
                    CHECK(heard.empty());
                } else {
                    REQUIRE(heard.size() == 1);
                    CHECK(heard[0].notice.reason == (!granted     ? "CapabilityDenied"
                                                     : state == 3 ? "TargetUnavailable"
                                                                  : "NoSuchTarget"));
                }
            }
        }
    }

    TEST_CASE("live authority is rechecked after authorship at the actual dispatch boundary") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard, Grant{});
        auto target = register_probe(sb, {ping_schema()});
        const auto grant =
            host_grant_authority(sb, a.id, LiveAuthority{}.allow("Ping", 1, target.id));
        auto admin = register_probe(sb, {pong_schema()});
        bool install = true;
        admin.weave->on_handle = [&](const Message&, Bus& bus, ProbeWeave&) {
            CHECK(static_cast<bool>(bus.delegate_authority(
                grant, install ? LiveAuthority{}.allow("Ping", 1, target.id) : LiveAuthority{})));
        };
        sb.send(admin.id, Message(pong(1)));
        sb.pump_pending();
        install = false;
        sb.send(admin.id, Message(pong(2))); // revocation is ahead of the authored send
        sb.send_as(a.id, target.id, Message(ping(1)));
        sb.pump_pending();
        CHECK(target.weave->handled_names.empty());
        sb.pump_pending();
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].notice.reason == "CapabilityDenied");
    }

    TEST_CASE("delivery without an answer released deferral and handler failure are not dispatch "
              "refusal") {
        for (int kind = 0; kind < 3; ++kind) {
            Switchboard sb;
            std::vector<Heard> heard;
            auto a = author(sb, heard);
            auto target = register_probe(sb, {ping_schema()});
            DeferredAnswer deferred;
            target.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
                if (kind == 1) {
                    deferred = b.make_deferred_answer();
                    REQUIRE(deferred.valid());
                    b.release_deferred(deferred);
                }
                if (kind == 2) {
                    throw std::runtime_error("handler did work before failing");
                }
            };
            auto t = sb.send_as(a.id, target.id, Message(ping(1)));
            if (kind == 2) {
                CHECK_THROWS_AS(sb.pump_pending(), std::runtime_error);
            } else {
                sb.pump_pending();
                CHECK(sb.outcome(t).disposition == Disposition::Delivered);
            }
            CHECK(target.weave->handled_names.size() == 1);
            CHECK(heard.empty());
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE(
        "a failed notice handler is consumed without a recursive notice and later work survives") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard);
        a.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
            throw std::runtime_error("notice failure");
        };
        sb.send_as(a.id, WeaveId{800}, Message(ping(1)));
        sb.pump_pending();
        REQUIRE(sb.pending() == 1);
        auto target = register_probe(sb, {ping_schema()});
        sb.send(target.id, Message(ping(9)));
        CHECK_THROWS_AS(sb.pump_pending(), std::runtime_error);
        REQUIRE(sb.pending() == 1);
        sb.pump_pending();
        CHECK(target.weave->handled_values == std::vector<std::int64_t>{9});
        CHECK(sb.pending() == 0);
    }

    TEST_CASE("notice backlog replaces consumed sends and preserves exact maximum wire names and "
              "unsigned identities") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard);
        for (int i = 0; i < 256; ++i) {
            sb.send_as(a.id, WeaveId{800}, Message(ping(i)));
        }
        REQUIRE(sb.pending() == 256);
        CHECK(sb.pump_pending() == 256);
        CHECK(sb.pending() == 256);
        CHECK(heard.empty());
        CHECK(sb.pump_pending() == 256);
        CHECK(sb.pending() == 0);
        REQUIRE(heard.size() == 256);
        const std::string name(65535, 's'); // serialize()'s actual maximum schema name
        const std::string role(65535, 'r'); // roles have no numeric cap; no truncated approximation
        auto shape = SchemaBuilder(name, std::numeric_limits<std::uint32_t>::max()).build();
        const auto t = sb.send_as_to_role(
            a.id, role, Message(Value(shape), {}, {}, std::numeric_limits<std::uint64_t>::max()));
        sb.pump_pending();
        sb.pump_pending();
        REQUIRE(heard.size() == 257);
        const auto& n = heard.back();
        CHECK(n.notice.role == role);
        CHECK(n.notice.shape == name);
        CHECK(n.notice.refused_attempt().seq == t.seq);
        CHECK(n.notice.version == std::numeric_limits<std::uint32_t>::max());
        CHECK(n.correlation == std::numeric_limits<std::uint64_t>::max());
        DispatchRefused ids;
        ids.attempt = "18446744073709551615";
        ids.target = ids.attempt;
        auto round = from_value<DispatchRefused>(to_value(ids));
        CHECK(round.refused_attempt().seq == std::numeric_limits<std::uint64_t>::max());
        CHECK(round.addressed_weave().value == std::numeric_limits<std::uint64_t>::max());
        ids.attempt = "18446744073709551616";
        CHECK_FALSE(ids.refused_attempt().valid());
        ids.attempt = "01";
        CHECK_FALSE(ids.refused_attempt().valid());
    }
}

TEST_SUITE("dispatch_refusal") {
    TEST_CASE("attempt exhaustion refuses reuse and preserves the original refusal host evidence") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard);
        DispatchRefusalProbe::exhaust_after_next(sb);
        auto last = sb.send_as(a.id, WeaveId{800}, Message(ping(1)));
        REQUIRE(last.seq == std::numeric_limits<std::uint64_t>::max());
        CHECK_THROWS_AS(sb.send_as(a.id, WeaveId{800}, Message(ping(2))), std::overflow_error);
        int original = 0;
        sb.add_observer([&](const BusEvent& e) {
            if (e.seq == last.seq && e.kind == EventKind::Refused)
                ++original;
        });
        CHECK_NOTHROW(sb.pump_pending());
        CHECK(original == 1);
        CHECK(sb.outcome(last).refusal.reason == RefusalReason::NoSuchTarget);
        CHECK(heard.empty());
        CHECK(sb.pending() == 0);
        INFO("envelope bytes: ", DispatchRefusalProbe::envelope_bytes(),
             " pending ask bytes: ", sizeof(PendingAsk));
        CHECK(DispatchRefusalProbe::envelope_bytes() > 0);
    }
}

TEST_SUITE("dispatch_refusal") {
    TEST_CASE(
        "an incompatible notice door refuses without a chain and retains original host evidence") {
        Switchboard sb;
        auto incompatible =
            SchemaBuilder(DispatchRefused::zen_name, 1).field("required_extra", Kind::Int).build();
        auto a = register_probe(sb, {incompatible}, 2, true, Grant{}.allow_any());
        auto t = sb.send_as(a.id, WeaveId{800}, Message(ping(1)));
        int refusals = 0;
        sb.add_observer([&](const BusEvent& e) {
            if (e.kind == EventKind::Refused)
                ++refusals;
        });
        CHECK(sb.pump_pending() == 1);
        REQUIRE(sb.pending() == 1);
        CHECK(sb.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
        CHECK(sb.pump_pending() == 1);
        CHECK(sb.pending() == 0);
        CHECK(a.weave->handled_names.empty());
        CHECK(refusals == 2);
    }
    TEST_CASE("notice carries exact maximum addressed id and metadata without the original large "
              "payload") {
        Switchboard sb;
        std::vector<Heard> heard;
        auto a = author(sb, heard);
        const WeaveId target{std::numeric_limits<std::uint64_t>::max()};
        const auto t = sb.send_as(a.id, target, Message(greet(std::string(8 * 1024 * 1024, 'x'))));
        CHECK(sb.pump_pending() == 1);
        CHECK(sb.pending() == 1);
        CHECK(sb.pump_pending() == 1);
        REQUIRE(heard.size() == 1);
        CHECK(heard.front().notice.addressed_weave() == target);
        CHECK(heard.front().notice.refused_attempt().seq == t.seq);
        CHECK(serialize(to_value(heard.front().notice)).size() < 1024);
        CHECK(sb.pending() == 0);
    }
}
