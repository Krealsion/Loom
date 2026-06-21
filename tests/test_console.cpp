#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/console/console.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

// The Console engine, proven with NO terminal — the headline. The engine is the durable
// spine; these tests drive its API directly (discover, gate-send, receive replies), so
// Stage 3's GUI inherits exactly this, only the skin new.

using namespace sbfx;          // Switchboard, ProbeShard, register_probe, ping/pong, RefusalReason, ...
using namespace zen::console;  // ConsoleEngine, ShardInfo, SendOutcome, FieldValue, ...
using zen::sb::Disposition;
using zen::sb::Ticket;

namespace {

// A shape the console code knows NOTHING about — defined only here, in the test.
std::shared_ptr<const zen::Schema> widget_schema() {
    static const auto s = zen::SchemaBuilder("Widget", 1).field("w", zen::Kind::Int).build();
    return s;
}
zen::Value widget(std::int64_t w) {
    zen::Value v(widget_schema());
    v.set("w", zen::Cell::integer(w));
    return v;
}

} // namespace

TEST_SUITE("console") {

TEST_CASE("the full participant loop, with NO terminal: discover, gate-send, reply buffered") {
    Switchboard bus;
    ConsoleEngine engine(bus);

    // A responder that accepts Ping (and Pong, so Pong is a registered shape the console can
    // resolve at wildcard-delivery) and replies Pong{seq} to the sender's reply_to.
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        if (in.payload.schema().name() == "Ping") {
            b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
        }
    };

    // Discovery sees the responder and its shapes — the console knows none of their meaning.
    bool found = false;
    for (const ShardInfo& s : engine.shards()) {
        if (s.id == responder.id) {
            found = true;
        }
    }
    CHECK(found);

    // Compose + gate-send Ping{seq=7}; one pump drives the send AND the reply (FIFO drain).
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();

    CHECK(engine.outcome(t).delivered); // the Ping reached the responder, gated
    REQUIRE(engine.buffer_size() == 1); // the reply landed in the buffer
    auto m1 = engine.buffer_at(1);
    REQUIRE(m1.has_value());
    CHECK(m1->label == "m1");
    CHECK(m1->name == "Pong");
    CHECK(m1->value.get("seq")->as_int() == 7); // read the reply's field back, by index
}

TEST_CASE("gated-send backstop: a malformed command is cleanly refused, no mis-send") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema()});
    bool handled = false;
    responder.shard->on_handle = [&](const Message&, Bus&, ProbeShard&) { handled = true; };

    // Ping{seq} with `seq` (required) left unset — slips compose-time, caught at the gate.
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {}, &err);
    REQUIRE_MESSAGE(t.valid(), err); // compose-time allows an incomplete message

    engine.pump();

    const SendOutcome o = engine.outcome(t);
    CHECK(o.refused);
    CHECK_FALSE(o.delivered);
    CHECK_FALSE(o.reason.empty());            // the gate's verdict is surfaced, not a silent drop
    // Precisely the gate's verdict: a conformance refusal for the missing required field.
    const zen::sb::DeliveryOutcome raw = bus.outcome(t);
    CHECK(raw.refusal.reason == RefusalReason::GateRefused);
    CHECK(raw.refusal.error.kind == zen::ErrorKind::MissingField);
    CHECK_FALSE(handled);                     // no silent mis-send: the responder never saw it
    CHECK(engine.buffer_size() == 0);
}

TEST_CASE("discovery + drive on a shape the console code has never seen") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // Widget is defined only in this test; the console code has no knowledge of it.
    Registered svc = register_probe(bus, {widget_schema()});
    svc.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        b.send(in.reply_to, Message(widget(in.payload.get("w")->as_int() + 1))); // echo + 1
    };

    // describe the unseen shape from the registry.
    auto desc = engine.describe("Widget", 1);
    REQUIRE(desc.has_value());
    REQUIRE(desc->fields.size() == 1);
    CHECK(desc->fields[0].name == "w");
    CHECK(desc->fields[0].type == "Int");
    CHECK(desc->fields[0].required);

    // compose a valid message to it and receive its reply — all without baked-in knowledge.
    std::string err;
    Ticket t = engine.submit(svc.id, "Widget", 1, {{"w", std::int64_t{41}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    CHECK(engine.outcome(t).delivered);
    REQUIRE(engine.buffer_size() == 1);
    auto m1 = engine.buffer_at(1);
    REQUIRE(m1.has_value());
    CHECK(m1->name == "Widget");
    CHECK(m1->value.get("w")->as_int() == 42);
}

TEST_CASE("wildcard-accept buffers a non-pre-declared shape (gated); an unregistered shape is refused") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };

    // The console never pre-declared Pong (its accept-set is empty) — yet the reply lands,
    // gated against Pong's registry-resolved schema.
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{1}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);
    CHECK(engine.buffer_at(1)->name == "Pong");

    // An UNREGISTERED shape sent to the console is refused (resolve_schema finds nothing) —
    // an unknown shape reaches no one, not even the wildcard console.
    auto unreg = zen::SchemaBuilder("Unregistered", 1).field("x", zen::Kind::Int).build();
    zen::Value v(unreg);
    v.set("x", zen::Cell::integer(9));
    Ticket u = bus.send(engine.console_id(), Message(std::move(v)));
    bus.pump();
    CHECK(bus.outcome(u).disposition == Disposition::Refused);
    CHECK(bus.outcome(u).refusal.reason == RefusalReason::NotAccepted);
    CHECK(engine.buffer_size() == 1); // unchanged — the unregistered shape was not buffered
}

TEST_CASE("wildcard-accept gates against the REGISTRY schema, not the payload's self-claim") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // Register the real Pong{seq:Int}, so "Pong" v1 resolves to it.
    Registered anchor = register_probe(bus, {ping_schema(), pong_schema()});
    (void)anchor;

    // A lying payload: it CLAIMS "Pong" v1 but carries a divergent shape (an extra field),
    // i.e. a different content_id. Wildcard-accept resolves "Pong" v1 to the REGISTERED
    // schema and the gate refuses the mismatch — the payload's self-claim buys nothing.
    auto lying = zen::SchemaBuilder("Pong", 1)
                     .field("seq", zen::Kind::Int)
                     .field("lie", zen::Kind::Text)
                     .build();
    zen::Value v(lying);
    v.set("seq", zen::Cell::integer(1));
    v.set("lie", zen::Cell::text("gotcha"));
    Ticket u = bus.send(engine.console_id(), Message(std::move(v)));
    bus.pump();
    CHECK(bus.outcome(u).disposition == Disposition::Refused);
    CHECK(bus.outcome(u).refusal.reason == RefusalReason::GateRefused);
    CHECK(bus.outcome(u).refusal.error.kind == zen::ErrorKind::SchemaMismatch);
    CHECK(engine.buffer_size() == 0); // the lie reached no one — gated against the registry shape
}

} // TEST_SUITE
