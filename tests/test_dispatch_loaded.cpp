// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#include "switchboard_fixtures.hpp"
#include "weavelib/dispatch_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/kernel/kernel.hpp>

#include <doctest.h>
#include <limits>
using namespace loom;
using namespace sbfx;
namespace {
Value mind(Switchboard& sb, WeaveId id) {
    return sb.weave(id)->snapshot();
}
} // namespace
TEST_SUITE("dispatch_loaded") {
    TEST_CASE("a real image authors all addressed forms and receives exact authenticated refusal "
              "attempts") {
        for (const std::string mode : {"direct", "role", "office-direct", "office-role"}) {
            Switchboard sb;
            Kernel kernel(sb);
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
        Kernel kernel(sb);
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
        Kernel kernel(sb);
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
            Kernel kernel(sb);
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
}
