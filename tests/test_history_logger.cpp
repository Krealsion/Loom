// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// RTH-1a — the Logger, the durable half of the split.
//
// The cases below prove the four things the split is for: that durability is
// SELECTED rather than accumulated, that it does not depend on the Recorder's
// retention lifetime, that a durable record which did not come from the bus can
// never be mistaken for one that did, and that no traffic anybody did not select
// can consume the horizon a later critical fact needs.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/admission.hpp>
#include <zen/history/dump.hpp>
#include <zen/history/logger.hpp>
#include <zen/history/recorder.hpp>

#include <cstdio>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

Registered reg(Switchboard& bus, std::vector<std::shared_ptr<const Schema>> accept) {
    return register_probe(bus, std::move(accept), 2, true, Grant{}.allow_any());
}

/// A scratch path that does not collide between cases in one run.
std::string scratch_path(const char* stem) {
    static int counter = 0;
    ++counter;
    return std::string("zen-rth1a-") + stem + "-" + std::to_string(counter) + ".log";
}

struct ScratchFile {
    explicit ScratchFile(const char* stem) : path(scratch_path(stem)) {}
    ~ScratchFile() { std::remove(path.c_str()); }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    std::string path;
};

/// A selection that keeps one named shape and nothing structural, so a case can
/// say exactly what it expects to find in the stream.
LoggerSelection only(std::string shape, std::size_t cap = 0) {
    LoggerSelection s;
    s.log_handler_failures = false;
    s.log_lifecycle = false;
    s.shapes.push_back(LogRule{std::move(shape), cap});
    return s;
}

std::size_t count_of(const std::vector<LogRecord>& all, const std::string& shape) {
    std::size_t n = 0;
    for (const LogRecord& r : all) {
        n += (r.origin == LogOrigin::BusObservation && r.observation.shape == shape)
                 ? std::size_t{1}
                 : std::size_t{0};
    }
    return n;
}

} // namespace

TEST_SUITE("logger") {

// ---------------------------------------------------------------------------
// Selection: a whitelist, and it REFUSES by default
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: a logger keeps what was named and nothing else") {
    ScratchFile file("selected");
    Switchboard bus;
    {
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema(), tick_schema()});
        bus.send(r.id, Message(ping(1)));
        for (int i = 0; i < 50; ++i) {
            bus.send(r.id, Message(tick(i))); // ordinary traffic, nobody selected it
        }
        bus.pump();
        CHECK(log.counters().observed == 51);
        CHECK(log.counters().selected == 1);
        CHECK(log.counters().appended == 1);
    }

    std::vector<LogRecord> back;
    std::string error;
    REQUIRE(Logger::read(file.path, &back, &error));
    REQUIRE(back.size() == 1);
    CHECK(back[0].origin == LogOrigin::BusObservation);
    CHECK(back[0].observation.shape == "Ping");
    CHECK(back[0].observation.outcome == RecordedOutcome::Delivered);
    CHECK(count_of(back, "Tick") == 0);
    // A DURABLE RECORD OF A RARE FACT CARRIES THE FACT. The message itself
    // round-tripped, and is re-readable through the ordinary gate.
    CHECK(back[0].observation.payload == PayloadDisposition::Retained);
    CHECK(!back[0].payload_body.empty());
    Unverified u = parse(back[0].payload_body);
    Admission a = admit(u, ping_schema());
    REQUIRE(a.ok());
    CHECK(a.value().get("seq")->as_int() == 1);
    // ...and it carries NO Recorder identity, because a Recorder did not make it.
    CHECK(back[0].observation.record_seq == 0);
    CHECK(back[0].observation.held == 0);
    CHECK(back[0].log_seq == 1);
}

TEST_CASE("RTH-1a: a recorder seeing a fact does not put it in the log") {
    // The clean statement of the ownership split: two lenses on the same tap, and
    // what one KNOWS is not what the other KEEPS.
    ScratchFile file("independent");
    Switchboard bus;
    {
        Recorder rec(bus);              // admits by default
        Logger log(bus, only("Ping"));  // refuses by default
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema(), tick_schema()});
        bus.send(r.id, Message(ping(1)));
        bus.send(r.id, Message(tick(1)));
        bus.pump();

        CHECK(rec.retained() == 2);          // the recorder knows about both
        CHECK(log.counters().appended == 1); // the logger kept one
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    REQUIRE(back.size() == 1);
    CHECK(back[0].observation.shape == "Ping");
}

TEST_CASE("RTH-1a: a durable fact outlives the recorder window that held its live copy") {
    ScratchFile file("outlives");
    Switchboard bus;
    RecorderPolicy policy = default_policy();
    policy.recent_capacity = 4;
    // Zero last-call depth for Ping, so the recorder genuinely has no way to keep
    // the early ones — which is the condition the durable record has to survive.
    policy.rules.push_back(RetentionRule{"Ping", 0, true, true});
    {
        Recorder rec(bus, policy);
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema()});
        for (int i = 0; i < 30; ++i) {
            bus.send(r.id, Message(ping(i)));
        }
        bus.pump();
        CHECK(rec.bounds().forgotten > 0);
        CHECK(rec.snapshot_of("Ping").size() == 4); // the window kept four
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    // ...and all thirty are durable, because the logger CAPTURED each from the
    // observation rather than reading a recorder entry that no longer exists.
    CHECK(count_of(back, "Ping") == 30);
}

TEST_CASE("RTH-1a: a logger works in a process with no recorder at all") {
    ScratchFile file("alone");
    Switchboard bus;
    {
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema()});
        bus.send(r.id, Message(ping(1)));
        bus.pump();
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    CHECK(back.size() == 1);
}

// ---------------------------------------------------------------------------
// Structural selection: the facts no shape can name
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: a failed handler is durable by default, whatever shape it was") {
    ScratchFile file("failure");
    Switchboard bus;
    {
        Logger log(bus); // the shipped default selection
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema()});
        r.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
            throw std::runtime_error("native handler failure");
        };
        bus.send(r.id, Message(ping(1)));
        CHECK_THROWS_AS(bus.pump(), std::runtime_error);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    REQUIRE(back.size() == 1);
    // `Ping` is on no whitelist and never will be; failure is not a property of a
    // shape, so it cannot be selected as one.
    CHECK(back[0].observation.shape == "Ping");
    CHECK(back[0].observation.outcome == RecordedOutcome::HandlerFailed);
    // The full truth the observation carried, durably: who, from whom, in what
    // conversation, and how long the handler held the one mind before it failed.
    CHECK(back[0].observation.target.value != 0);
    CHECK(back[0].observation.shape_version == 1);
    // ...AND THE MESSAGE IT CHOKED ON. A failure record without the input that
    // produced it is the half of the record a maker cannot use. This is the seam a
    // richer failure packet (weave identity, exception text, symbols) later grows
    // from; nothing here has to be undone to add those.
    CHECK(back[0].observation.payload == PayloadDisposition::Retained);
    Unverified u = parse(back[0].payload_body);
    Admission a = admit(u, ping_schema());
    REQUIRE(a.ok());
    CHECK(a.value().get("seq")->as_int() == 1);
}

TEST_CASE("RTH-1a: ordinary refusals are NOT durable by default, and that is measured") {
    // RTH-1's live Workshop run found every KeyReleased reaching nobody, so a
    // refusal is neither rare nor severe and a default that kept them all would
    // make the durable stream ordinary traffic under another name.
    ScratchFile file("refusals");
    Switchboard bus;
    {
        Logger log(bus);
        REQUIRE(!log.selection().log_refusals);
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema()});
        for (int i = 0; i < 20; ++i) {
            bus.send(r.id, Message(greet("nobody accepts this")));
        }
        bus.pump();
        CHECK(log.counters().selected == 0);
        CHECK(log.counters().appended == 0);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    CHECK(back.empty());
}

TEST_CASE("RTH-1a: a death is durable by default; it is rare and it changes who is here") {
    ScratchFile file("death");
    Switchboard bus;
    {
        Logger log(bus);
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema()});
        bus.kill(r.id);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    REQUIRE(back.size() == 1);
    CHECK(back[0].observation.kind == RecordKind::Lifecycle);
    CHECK(back[0].observation.seq == 0);
}

TEST_CASE("RTH-1a: the shipped default keeps weave-lifecycle and authority vocabulary") {
    // The default whitelist is source-traced, not invented: two categories that
    // are rare BY CONSTRUCTION — what code is loaded, and who may speak.
    const LoggerSelection s = default_selection();
    CHECK(s.rule_for("LoadLibrary") != nullptr);
    CHECK(s.rule_for("UnloadLibrary") != nullptr);
    CHECK(s.rule_for("zen.SwapWeave") != nullptr);
    CHECK(s.rule_for("zen.RevokeAuthority") != nullptr);
    CHECK(s.rule_for("zen.AuthorityGranted") != nullptr);
    // ...and deliberately not the QUERIES beside them: a read is not a change.
    CHECK(s.rule_for("ListLibraries") == nullptr);
    CHECK(s.rule_for("QueryRole") == nullptr);
    CHECK(s.rule_for("zen.DescribeAuthority") == nullptr);
    // Every default entry is UNCAPPED — the horizon a rare durable fact needs is
    // not a number somebody has to have guessed right in advance.
    for (const LogRule& r : s.shapes) {
        CHECK(r.cap == 0);
    }
}

// ---------------------------------------------------------------------------
// Caps: per shape, never global
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: a per-shape cap bounds ITS shape and says so, once") {
    ScratchFile file("cap");
    Switchboard bus;
    {
        Logger log(bus, only("Tick", /*cap=*/5));
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {tick_schema()});
        for (int i = 0; i < 40; ++i) {
            bus.send(r.id, Message(tick(i)));
        }
        bus.pump();
        CHECK(log.appended_of("Tick") == 5);
        CHECK(log.counters().capped == 35);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    CHECK(count_of(back, "Tick") == 5);
    // A CAP IS NOT AN ENDING. The last record says which it was, so a reader can
    // never mistake a bounded shape for a dead process.
    REQUIRE(!back.empty());
    CHECK(back.back().origin == LogOrigin::PolicyChange);
    CHECK(back.back().text.find("cap reached") != std::string::npos);
    CHECK(back.back().text.find("Tick") != std::string::npos);
}

TEST_CASE("RTH-1a: a capped shape cannot consume the horizon of an uncapped one") {
    // THE RTH-1 FAILURE THIS PHASE EXISTS TO REMOVE. Under one global byte budget,
    // forty thousand beats made a later weave replacement unwritable. Here the
    // noisy shape stops at its own number and the rare one is untouched.
    ScratchFile file("horizon");
    Switchboard bus;
    {
        LoggerSelection sel;
        sel.log_handler_failures = false;
        sel.log_lifecycle = false;
        sel.shapes.push_back(LogRule{"Tick", 3});
        sel.shapes.push_back(LogRule{"Ping", 0}); // uncapped, and rare
        Logger log(bus, sel);
        REQUIRE(log.open(file.path));
        Registered r = reg(bus, {ping_schema(), tick_schema()});
        for (int i = 0; i < 2000; ++i) {
            bus.send(r.id, Message(tick(i)));
        }
        bus.pump();
        // ...and only NOW, after all that traffic, the fact that matters.
        bus.send(r.id, Message(ping(7)));
        bus.pump();
        CHECK(log.appended_of("Ping") == 1);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    CHECK(count_of(back, "Tick") == 3);
    REQUIRE(count_of(back, "Ping") == 1);
}

// ---------------------------------------------------------------------------
// Origins: a durable record may not pretend it was a message
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: a host diagnostic is durable and is never mistaken for a Loom message") {
    ScratchFile file("diagnostic");
    Switchboard bus;
    {
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        CHECK(log.error("zengine.workshop", "the build tool is missing"));
        Registered r = reg(bus, {ping_schema()});
        bus.send(r.id, Message(ping(1)));
        bus.pump();
        CHECK(log.counters().diagnostics == 1);
        // NOTHING WAS PUBLISHED to carry it. A fake message would have been
        // observable, and a history that observed its own diagnostics would be
        // manufacturing the traffic it exists to select from.
        CHECK(log.counters().observed == 1);
        CHECK(bus.pending() == 0);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    REQUIRE(back.size() == 2);
    CHECK(back[0].origin == LogOrigin::Diagnostic);
    CHECK(back[0].severity == Severity::Error);
    CHECK(back[0].source == "zengine.workshop");
    CHECK(back[0].text == "the build tool is missing");
    // The discriminant is load-bearing: a diagnostic carries NO observation, and
    // nothing about it can be read as one.
    CHECK(back[0].observation.shape.empty());
    CHECK(back[0].observation.seq == 0);
    CHECK(back[0].observation.outcome == RecordedOutcome::None);
    CHECK(back[1].origin == LogOrigin::BusObservation);
    CHECK(back[1].observation.shape == "Ping");
    // ...and the two are separately numbered in one stream.
    CHECK(back[0].log_seq == 1);
    CHECK(back[1].log_seq == 2);
}

TEST_CASE("RTH-1a: changing what is kept is itself kept, and publishes nothing") {
    ScratchFile file("policy");
    Switchboard bus;
    {
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        LoggerSelection next = log.selection();
        next.shapes.push_back(LogRule{"Tick", 4});
        next.log_refusals = true;
        log.select(next);
        CHECK(bus.pending() == 0);
        CHECK(log.counters().observed == 0);
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    REQUIRE(back.size() == 1);
    CHECK(back[0].origin == LogOrigin::PolicyChange);
    CHECK(back[0].text.find("+Tick") != std::string::npos);
    CHECK(back[0].text.find("refusals: on") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The stream itself
// ---------------------------------------------------------------------------

TEST_CASE("RTH-1a: a corrupt durable stream is refused by the gate, not believed") {
    ScratchFile file("corrupt");
    {
        std::ofstream out(file.path, std::ios::binary);
        const char header[4] = {8, 0, 0, 0};
        out.write(header, 4);
        out.write("notavalue", 8);
    }
    std::vector<LogRecord> back;
    std::string error;
    CHECK(!Logger::read(file.path, &back, &error));
    CHECK(!error.empty());
}

TEST_CASE("RTH-1a: a selected fact with nowhere to go is counted, never pretended") {
    Switchboard bus;
    Logger log(bus, only("Ping")); // never opened
    Registered r = reg(bus, {ping_schema()});
    bus.send(r.id, Message(ping(1)));
    bus.pump();
    CHECK(!log.open());
    CHECK(log.counters().selected == 1);
    CHECK(log.counters().appended == 0);
}

TEST_CASE("RTH-1a: logging changes what is remembered and nothing about delivery") {
    Switchboard bus;
    Logger log(bus, only("Ping"));
    Registered r = reg(bus, {ping_schema()});
    const Ticket t = bus.send(r.id, Message(ping(1)));
    bus.pump();
    // The message reached its recipient without passing through the logger: it is
    // a tap consumer, not a stage in the delivery path.
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);
    CHECK(r.weave->count == 1);
}

TEST_CASE("RTH-1a: the log dump names every record's ORIGIN before its content") {
    ScratchFile file("dump");
    Switchboard bus;
    {
        Logger log(bus, only("Ping"));
        REQUIRE(log.open(file.path));
        log.warn("zen.tests", "a note from the host");
        Registered r = reg(bus, {ping_schema()});
        bus.send(r.id, Message(ping(1)));
        bus.pump();
    }
    std::vector<LogRecord> back;
    REQUIRE(Logger::read(file.path, &back));
    std::ostringstream out;
    dump_log(back, out);
    const std::string text = out.str();
    CHECK(text.find("Diagnostic Warning [zen.tests] a note from the host") != std::string::npos);
    CHECK(text.find("BusObservation") != std::string::npos);
    CHECK(text.find("Ping") != std::string::npos);
}

} // TEST_SUITE("logger")
