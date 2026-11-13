// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE TURN'S TIME ON EVERY DELIVERY, in process: the host's wait hands the bus what it read when
// it ended, every delivery of the turn it began carries both readings, a turn no wait began
// carries none, no sender can set them, a suite's clock sets both, and history keeps what each
// delivery was handed, read back from the durable log as it was.

#include <doctest.h>

#include "weavelib/turn_protocol.hpp"

#include <zen/history/logger.hpp>
#include <zen/history/recorder.hpp>
#include <zen/host/clock.hpp>
#include <zen/host/reactor.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace loom;
using namespace turning;

namespace {

/// A native reader: each knock's turn, as its delivery handed it.
class Reader final : public WeaveBase<Reader, TurnHeard, Accept<TurnKnock>> {
public:
    void on(const TurnKnock&, Mail& mail) {
        ms.push_back(mail.turn_ms());
        date.push_back(mail.turn_date_ms());
    }
    std::vector<std::optional<std::int64_t>> ms;
    std::vector<std::optional<std::int64_t>> date;
};

Reader* mount_reader(Switchboard& bus, WeaveId* id) {
    auto owned = std::make_unique<Reader>();
    Reader* raw = owned.get();
    *id = bus.register_weave(std::move(owned), Grant{}, "turn.reader");
    raw->zen_set_self(*id);
    return raw;
}

constexpr std::int64_t kDate = 1794600000000; // a calendar reading the suite's clock is given

} // namespace

TEST_SUITE("turn") {

TEST_CASE("a delivery in a turn the wait began carries its time and date; one in a turn no wait "
          "began carries none") {
    Switchboard bus;
    host::ManualClock clock(1000, kDate);
    host::Reactor wait(bus, clock);
    WeaveId id;
    Reader* r = mount_reader(bus, &id);

    bus.send(id, Message(to_value(TurnKnock{1})));
    (void)bus.pump_pending(); // no wait has ended: no turn's time
    REQUIRE(r->ms.size() == 1);
    CHECK_FALSE(r->ms[0].has_value());
    CHECK_FALSE(r->date[0].has_value());

    (void)wait.after_turn(0); // the wait looks, reads the clock, and hands the bus its reading
    bus.send(id, Message(to_value(TurnKnock{2})));
    bus.send(id, Message(to_value(TurnKnock{3})));
    (void)bus.pump_pending();
    REQUIRE(r->ms.size() == 3);
    CHECK(r->ms[1] == 1000);
    CHECK(r->date[1] == kDate);
    CHECK(r->ms[2] == 1000); // every delivery of that turn, the same reading
    CHECK(r->date[2] == kDate);

    bus.send(id, Message(to_value(TurnKnock{4})));
    (void)bus.pump_pending(); // the turn after it followed no wait
    CHECK_FALSE(r->ms[3].has_value());

    // The suite's clock moves both: a wait with nothing ready moves it to its deadline.
    (void)wait.after_turn(500);
    CHECK(clock.now_ms() == 1500);
    bus.send(id, Message(to_value(TurnKnock{5})));
    bus.drain_until_idle();
    CHECK(r->ms[4] == 1500);
    CHECK(r->date[4] == kDate + 500);
}

TEST_CASE("no sender sets the turn's time: what a message carries in is never what it is handed") {
    Switchboard bus;
    WeaveId id;
    Reader* r = mount_reader(bus, &id);
    Message forged(to_value(TurnKnock{1}));
    forged.turn = TurnFacts{7, 8};
    bus.send(id, std::move(forged));
    (void)bus.pump_pending();
    REQUIRE(r->ms.size() == 1);
    CHECK_FALSE(r->ms[0].has_value());

    bus.begin_turn(TurnFacts{100, 200});
    Message again(to_value(TurnKnock{2}));
    again.turn = TurnFacts{7, 8};
    bus.send(id, std::move(again));
    (void)bus.pump_pending();
    CHECK(r->ms[1] == 100);
    CHECK(r->date[1] == 200);
}

TEST_CASE("history keeps the turn a delivery was handed, and the durable log reads it back") {
    const std::string path = "zen-turn-history.log";
    std::remove(path.c_str());
    std::optional<TurnFacts> recorded;
    {
        Switchboard bus;
        Recorder rec(bus);
        LoggerSelection only;
        only.log_handler_failures = false;
        only.log_lifecycle = false;
        only.shapes.push_back(LogRule{TurnKnock::zen_name, 0});
        Logger log(bus, only);
        REQUIRE(log.open(path));
        WeaveId id;
        Reader* r = mount_reader(bus, &id);
        bus.begin_turn(TurnFacts{4242, kDate});
        bus.send(id, Message(to_value(TurnKnock{1})));
        (void)bus.pump_pending();
        REQUIRE(r->ms.size() == 1);
        const Lookup last = rec.last_of(TurnKnock::zen_name);
        REQUIRE(last.record != nullptr);
        recorded = last.record->turn;
        REQUIRE(recorded.has_value());
        CHECK(recorded->ms == *r->ms[0]);
        CHECK(recorded->date_ms == *r->date[0]);
    }
    std::vector<LogRecord> back;
    std::string error;
    REQUIRE_MESSAGE(Logger::read(path, &back, &error), error);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].observation.turn.has_value());
    CHECK(back[0].observation.turn->ms == 4242);
    CHECK(back[0].observation.turn->date_ms == kDate);
    std::remove(path.c_str());
}

} // TEST_SUITE("turn")
