// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#include "switchboard_fixtures.hpp"
#include "weavelib/dispatch_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/kernel/kernel.hpp>

#include <doctest.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
using namespace loom;
using namespace sbfx;
namespace {
Value mind(Switchboard& sb, WeaveId id) {
    return sb.weave(id)->snapshot();
}
} // namespace
TEST_SUITE("dispatch_loaded") {
    TEST_CASE("ABI v7: loaded role sends and office publications have distinct successful semantics") {
        // Both miswired callbacks can succeed here: the addressed role is also an
        // office the author holds, and all recipients accept the SAME payload.
        // Snapshot receipts do not depend on sending a report through either door.
        for (const std::string mode : {"role", "office-publish", "publish", "direct", "office-denied"}) {
            CAPTURE(mode);
            Switchboard sb;
            Kernel kernel(sb, sbfx::fixture_admission());
            const auto loaded = kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author");
            REQUIRE_MESSAGE(loaded.ok, loaded.error);
            const auto first = register_probe(sb, {schema_of<dispatch_test::Payload>()});
            const auto second = register_probe(sb, {schema_of<dispatch_test::Payload>()});
            struct Delivery {
                std::uint64_t target;
                std::uint64_t attempt;
                std::string office;
            };
            std::vector<Delivery> deliveries;
            sb.add_observer([&](const BusEvent& ev) {
                if (ev.kind == EventKind::Delivered &&
                    ev.schema_name == dispatch_test::Payload::zen_name) {
                    CHECK(ev.sender == loaded.id);
                    deliveries.push_back({ev.target.value, ev.seq, ev.authored_role});
                }
            });
            // Keep any attempt above the three-recipient count, independent of
            // activation traffic or incidental equality at the start of a board.
            for (int i = 0; i < 8; ++i) {
                sb.send(loaded.id, Message(to_value(dispatch_test::Command{"prime", 0, "", "0"})));
            }
            sb.pump_pending();
            REQUIRE(sb.pending() == 0);
            const bool denied = mode == "office-denied";
            const bool office = mode == "office-publish";
            const bool addressed = mode == "role" || mode == "direct";
            sb.send(loaded.id, Message(to_value(dispatch_test::Command{
                denied ? "office-publish" : mode, static_cast<std::int64_t>(first.id.value),
                denied ? "not-held" : "dispatch.author", "77"})));
            sb.pump_pending();
            const auto state = mind(sb, loaded.id);
            CHECK(state.get("authored")->as_bool() == office);
            CHECK(state.get("recipients")->as_int() == (office ? 3 : 0));
            CHECK(state.get("queued")->as_int() == (addressed ? 1 : 0));
            const auto& attempts = state.get("attempts")->as_list();
            REQUIRE(attempts.size() == (addressed ? 1u : 0u));
            const auto attempt = addressed ? std::stoull(attempts.front().as_text()) : 0;
            if (addressed) { CHECK(attempt > 3); }
            sb.drain_until_idle();

            std::vector<std::uint64_t> actual;
            for (const auto& delivery : deliveries) {
                actual.push_back(delivery.target);
                CHECK(delivery.office == (office ? "dispatch.author" : ""));
                if (addressed) { CHECK(delivery.attempt == attempt); }
                if (office) { CHECK(delivery.attempt > 3); }
            }
            std::vector<std::uint64_t> expected;
            if (mode == "role") { expected = {loaded.id.value}; }
            else if (mode == "direct") { expected = {first.id.value}; }
            else if (!denied) { expected = {loaded.id.value, first.id.value, second.id.value}; }
            std::sort(actual.begin(), actual.end());
            std::sort(expected.begin(), expected.end());
            CHECK(actual == expected);
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("a real image authors all addressed forms and receives exact authenticated refusal "
              "attempts") {
        for (const std::string mode : {"direct", "role", "office-direct", "office-role"}) {
            Switchboard sb;
            Kernel kernel(sb, sbfx::fixture_admission());
            auto target = register_probe(sb, {schema_of<dispatch_test::Payload>()});
            const auto loaded =
                kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author", Grant{});
            REQUIRE_MESSAGE(loaded.ok, loaded.error);
            const std::string role(4096, 'r');
            for (const auto corr : {"0", "77", "77", "18446744073709551615"}) {
                sb.send(loaded.id,
                        Message(to_value(dispatch_test::Command{
                            mode, static_cast<std::int64_t>(target.id.value), role, corr})));
            }
            sb.pump_pending();
            REQUIRE(sb.pending() == 4);
            CHECK(mind(sb, loaded.id).get("queued")->as_int() == 4);
            CHECK(mind(sb, loaded.id).get("trusted")->as_int() == 0);
            sb.pump_pending();
            CHECK(target.weave->handled_names.empty());
            REQUIRE(sb.pending() == 4);
            sb.pump_pending();
            const Value state = mind(sb, loaded.id);
            CHECK(state.get("trusted")->as_int() == 4);
            CHECK(state.get("matched")->as_int() == 4);
            CHECK(state.get("attempts")->as_list().empty());
            CHECK_FALSE(state.get("answer_right")->as_bool());
            CHECK(state.get("reason")->as_text() == "CapabilityDenied");
            CHECK(state.get("correlation")->as_text() == "18446744073709551615");
            CHECK(state.get("role")->as_text() ==
                  (mode.find("role") != std::string::npos ? role : ""));
            CHECK(state.get("target")->as_text() ==
                  (mode.find("role") != std::string::npos ? "" : std::to_string(target.id.value)));
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("loaded delivery and synchronous seam rejection do not manufacture later refusal") {
        Switchboard sb;
        Kernel kernel(sb, sbfx::fixture_admission());
        auto loaded = kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author");
        REQUIRE(loaded.ok);
        auto target = register_probe(sb, {schema_of<dispatch_test::Payload>()});
        sb.send(loaded.id, Message(to_value(dispatch_test::Command{
                               "direct", static_cast<std::int64_t>(target.id.value), "", "1"})));
        sb.pump_pending();
        sb.pump_pending();
        CHECK(target.weave->handled_names.size() == 1);
        CHECK(sb.pending() == 0);
        CHECK(mind(sb, loaded.id).get("queued")->as_int() == 1);
        CHECK(mind(sb, loaded.id).get("trusted")->as_int() == 0);
        sb.send(loaded.id, Message(to_value(dispatch_test::Command{
                               "malformed", static_cast<std::int64_t>(target.id.value), "", "1"})));
        sb.pump_pending();
        CHECK(sb.pending() == 0);
        CHECK(mind(sb, loaded.id).get("queued")->as_int() == 1);
        CHECK(target.weave->handled_names.size() == 1);
        // A loaded office request from a non-holder remains an immediate rejection.
        auto other = kernel.load("other", ZEN_SO_DISPATCH);
        REQUIRE(other.ok);
        sb.send(other.id,
                Message(to_value(dispatch_test::Command{
                    "office-direct", static_cast<std::int64_t>(target.id.value), "", "1"})));
        sb.pump_pending();
        CHECK(sb.pending() == 0);
        CHECK(mind(sb, other.id).get("queued")->as_int() == 0);
    }

    TEST_CASE("loaded forgery and native copied provenance remain ordinary at a loaded recipient") {
        Switchboard sb;
        Kernel kernel(sb, sbfx::fixture_admission());
        auto a = kernel.load("a", ZEN_SO_DISPATCH, "dispatch.author");
        REQUIRE(a.ok);
        auto b = kernel.load("b", ZEN_SO_DISPATCH);
        REQUIRE(b.ok);
        sb.send(a.id, Message(to_value(dispatch_test::Command{
                          "forge", static_cast<std::int64_t>(b.id.value), "", "91"})));
        sb.pump_pending();
        sb.pump_pending();
        CHECK(mind(sb, b.id).get("ordinary")->as_int() == 1);
        CHECK(mind(sb, b.id).get("trusted")->as_int() == 0);
        Message fake(to_value(DispatchRefused{}));
        fake.provenance = Provenance::attested(Provenance::Kind::DispatchRefusal, 0);
        sb.send(b.id, std::move(fake));
        sb.pump_pending();
        CHECK(mind(sb, b.id).get("ordinary")->as_int() == 2);
        CHECK(mind(sb, b.id).get("matched")->as_int() == 0);
    }

    TEST_CASE(
        "actual loaded code replacement at either queue boundary cannot inherit an old refusal") {
        for (bool queued : {false, true}) {
            Switchboard sb;
            Kernel kernel(sb, sbfx::fixture_admission());
            auto a = kernel.load("a", ZEN_SO_DISPATCH, "dispatch.author", Grant{});
            REQUIRE(a.ok);
            sb.send(a.id,
                    Message(to_value(dispatch_test::Command{"role", 0, "missing.service", "77"})));
            sb.pump_pending();
            REQUIRE(sb.pending() == 1);
            if (queued) {
                sb.pump_pending();
                REQUIRE(sb.pending() == 1);
            }
            auto changed = kernel.reload_from("a", ZEN_SO_DISPATCH);
            REQUIRE_MESSAGE(changed.ok, changed.error);
            sb.pump_pending();
            CHECK(sb.pending() == 0);
            CHECK(mind(sb, a.id).get("trusted")->as_int() == 0);
            sb.send(a.id,
                    Message(to_value(dispatch_test::Command{"role", 0, "missing.service", "77"})));
            sb.pump_pending();
            sb.pump_pending();
            sb.pump_pending();
            CHECK(mind(sb, a.id).get("trusted")->as_int() == 1);
            CHECK(mind(sb, a.id).get("matched")->as_int() == 1);
        }
    }

    // No exception crosses the seam from a host door (zen/kernel/abi.h): the image that called may
    // carry its own C++ runtime. Each door below meets the end of the host's delivery sequence in
    // the host's own work; it answers refused, the loaded handler runs to its end, and the bus
    // records a delivery, never a handler that failed.
    TEST_CASE("a host door that cannot carry out a request answers refused, and no exception "
              "reaches the loaded image") {
        for (const std::string mode :
             {"direct", "role", "office-direct", "office-role", "publish", "office-publish",
              "answer", "spend-unknown", "claim-unheard", "office-claim-unheard",
              "offer-unheard"}) {
            CAPTURE(mode);
            Switchboard sb;
            Kernel kernel(sb, sbfx::fixture_admission());
            const auto loaded = kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author");
            REQUIRE_MESSAGE(loaded.ok, loaded.error);
            const auto recipient = register_probe(sb, {schema_of<dispatch_test::Payload>()});
            std::vector<EventKind> commands;
            sb.add_observer([&](const BusEvent& ev) {
                if (ev.schema_name == dispatch_test::Command::zen_name) {
                    commands.push_back(ev.kind);
                }
            });
            sb.send(loaded.id, Message(to_value(dispatch_test::Command{
                                   mode, static_cast<std::int64_t>(recipient.id.value),
                                   "dispatch.author", "77"})));
            DispatchRefusalProbe::exhaust_now(sb);
            CHECK_NOTHROW(sb.pump_pending());
            const Value state = mind(sb, loaded.id);
            CHECK(state.get("finished")->as_text() == mode);
            CHECK(state.get("door")->as_text() == "refused");
            CHECK(commands == std::vector<EventKind>{EventKind::Delivered});
        }
    }

    TEST_CASE("an observer that throws while a loaded door is refused costs the door, not the "
              "loaded image") {
        for (const std::string mode :
             {"office-publish", "answer", "spend-unknown", "claim-unheard"}) {
            CAPTURE(mode);
            Switchboard sb;
            Kernel kernel(sb, sbfx::fixture_admission());
            const auto loaded = kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author");
            REQUIRE_MESSAGE(loaded.ok, loaded.error);
            std::vector<EventKind> commands;
            int refusals = 0;
            sb.add_observer([&](const BusEvent& ev) {
                if (ev.schema_name == dispatch_test::Command::zen_name) {
                    commands.push_back(ev.kind);
                }
                if (ev.kind == EventKind::Refused) {
                    ++refusals;
                    throw std::runtime_error("an observer that throws");
                }
            });
            // office-publish names an office the image does not hold.
            sb.send(loaded.id,
                    Message(to_value(dispatch_test::Command{mode, 0, "not.held", "77"})));
            CHECK_NOTHROW(sb.pump_pending());
            const Value state = mind(sb, loaded.id);
            CHECK(refusals == 1);
            CHECK(state.get("finished")->as_text() == mode);
            CHECK(state.get("door")->as_text() == "refused");
            CHECK(commands == std::vector<EventKind>{EventKind::Delivered});
        }
    }

    TEST_CASE("an answer right asked for past the host's limit, with the sequence at its end, "
              "answers no right and costs the loaded image nothing") {
        Switchboard sb;
        Kernel kernel(sb, sbfx::fixture_admission());
        const auto loaded = kernel.load("dispatch", ZEN_SO_DISPATCH, "dispatch.author");
        REQUIRE_MESSAGE(loaded.ok, loaded.error);
        const auto asker = register_probe(sb, {schema_of<dispatch_test::Payload>()});
        const auto ask = [&] {
            sb.send_as(asker.id, loaded.id,
                       Message(to_value(dispatch_test::Command{"defer", 0, "", "0"}), WeaveId{},
                               WeaveId{}, 77));
        };
        for (std::size_t i = 0; i < Switchboard::kMaxDeferredAnswers; ++i) {
            ask();
        }
        sb.pump_pending();
        REQUIRE(mind(sb, loaded.id).get("deferred")->as_int() ==
                static_cast<std::int64_t>(Switchboard::kMaxDeferredAnswers));
        std::vector<EventKind> commands;
        sb.add_observer([&](const BusEvent& ev) {
            if (ev.schema_name == dispatch_test::Command::zen_name) {
                commands.push_back(ev.kind);
            }
        });
        ask();
        DispatchRefusalProbe::exhaust_now(sb);
        CHECK_NOTHROW(sb.pump_pending());
        const Value state = mind(sb, loaded.id);
        CHECK(state.get("finished")->as_text() == "defer");
        CHECK(state.get("door")->as_text() == "refused");
        CHECK(state.get("deferred")->as_int() ==
              static_cast<std::int64_t>(Switchboard::kMaxDeferredAnswers));
        CHECK(commands == std::vector<EventKind>{EventKind::Delivered});
    }

    TEST_CASE("bytes a library hands the host's sink beyond what it can hold refuse the load in "
              "the host's words, and nothing thrown reaches the library") {
        Switchboard sb;
        Kernel kernel(sb, sbfx::fixture_admission());
        const auto loaded = kernel.load("sink", ZEN_SO_SINK_OVERFLOW);
        CHECK_FALSE(loaded.ok);
        CHECK_MESSAGE(loaded.error.find("the host could not hold the bytes library describe() "
                                        "wrote") != std::string::npos,
                      loaded.error);
    }
}
