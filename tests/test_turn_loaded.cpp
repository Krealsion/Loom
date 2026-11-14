// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE TURN'S TIME ACROSS THE SEAM: a loaded image reads the turn's time and date as a native
// weave does, whatever its grant; an image built against ABI 15's header, which has no turn,
// loads and works, handed none; and a box is handed them only where its grant gives it time.

#include <doctest.h>

#include "box_twin.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/addressed_protocol.hpp"
#include "weavelib/turn_protocol.hpp"

#include <zen/kernel/kernel.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <string>

using namespace loom;
using namespace sbfx;
using namespace turning;

namespace {

constexpr std::int64_t kDate = 1794600000000;

TurnHeard heard(Switchboard& sb, WeaveId id) {
    return from_value<TurnHeard>(sb.weave(id)->snapshot());
}

/// Knock once in a turn a wait began, and once in a turn none did.
void knock_twice(Switchboard& sb, const std::string& role, TurnHeard* in_turn, WeaveId id) {
    sb.begin_turn(TurnFacts{777, kDate});
    sb.send_to_role(role, Message(to_value(TurnKnock{1})));
    (void)sb.pump_pending();
    *in_turn = heard(sb, id);
    sb.send_to_role(role, Message(to_value(TurnKnock{2})));
    (void)sb.pump_pending();
}

#if defined(LOOM_ENABLE_WASM)
/// A box is handed the turn's time only with a grant that gives it time.
void a_box_is_told_the_time_only_with_its_grant(const char* path) {
    for (const bool given : {false, true}) {
        CAPTURE(given);
        Switchboard sb;
        Kernel kernel(sb, fixture_admission());
        Grant grant = Grant{}.allow_any();
        if (given) {
            grant.with_time();
        }
        const LoadResult r = kernel.load("turn", path, "turn.a", grant);
        REQUIRE_MESSAGE(r.ok, r.error);
        TurnHeard first;
        knock_twice(sb, "turn.a", &first, r.id);
        CHECK(first.knocks == 1);
        CHECK(first.told == (given ? 1 : 0));
        CHECK(first.turn_ms == (given ? 777 : 0));
        CHECK(first.turn_date_ms == (given ? kDate : 0));
        const TurnHeard second = heard(sb, r.id);
        CHECK(second.knocks == 2);
        CHECK(second.told == 0); // a turn no wait began, given or not
    }
}
#endif

} // namespace

TEST_SUITE("turn_loaded") {

TEST_CASE("a loaded image reads the turn's time and date as a native weave does, whatever its "
          "grant") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("turn", ZEN_SO_TURN_PROBE, "turn.a", Grant{}.allow_any());
    REQUIRE_MESSAGE(r.ok, r.error);
    TurnHeard first;
    knock_twice(sb, "turn.a", &first, r.id);
    CHECK(first.told == 1);
    CHECK(first.turn_ms == 777);
    CHECK(first.turn_date_ms == kDate);
    const TurnHeard second = heard(sb, r.id);
    CHECK(second.knocks == 2);
    CHECK(second.told == 0);
}

TEST_CASE("an image built against ABI 15's header, which has no turn, loads and works here, "
          "handed none") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult door = kernel.load("door", ZEN_SO_ABI_V15_IMAGE, "door.a");
    REQUIRE_MESSAGE(door.ok, door.error);
    sb.begin_turn(TurnFacts{777, kDate});
    sb.send_to_role("door.a", Message(to_value(addressed::AddressedKnock{1})));
    sb.drain_until_idle();
    const auto knocked = from_value<addressed::AddressedHeard>(sb.weave(door.id)->snapshot());
    CHECK(knocked.knocks == 1);
    CHECK(knocked.addressed == "door.a");
}

} // TEST_SUITE("turn_loaded")

ZEN_BOXED_TWIN("a box is handed the turn's time only where its grant gives it time",
               a_box_is_told_the_time_only_with_its_grant, ZEN_BOX_TURN_PROBE)
