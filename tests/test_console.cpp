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

// ---- Stage 2 shapes, known only to the tests ----

// label (Text) is OPTIONAL and declared first, count (Int) is required and second — so a lone
// Int fails positional at slot 0 (Text) and is rescued by type-directed into count.
std::shared_ptr<const zen::Schema> tagged_schema() {
    static const auto s = zen::SchemaBuilder("Tagged", 1)
                              .field("label", zen::Kind::Text, /*required=*/false)
                              .field("count", zen::Kind::Int)
                              .build();
    return s;
}
// name (Text, first) then two same-typed Int fields — a lone Int can't go positional (slot 0 is
// Text) and matches BOTH a and b under type-direction: genuine ambiguity.
std::shared_ptr<const zen::Schema> mix_schema() {
    static const auto s = zen::SchemaBuilder("Mix", 1)
                              .field("name", zen::Kind::Text)
                              .field("a", zen::Kind::Int)
                              .field("b", zen::Kind::Int)
                              .build();
    return s;
}
std::shared_ptr<const zen::Schema> note_schema() {
    static const auto s = zen::SchemaBuilder("Note", 1).field("body", zen::Kind::Text).build();
    return s;
}

// ---- Arg builders (what the terminal's lexer will hand the engine) ----
Arg lit(FieldValue v) { return Arg{std::nullopt, std::move(v)}; }
Arg named(std::string n, FieldValue v) { return Arg{std::move(n), std::move(v)}; }
Arg ref(std::string label, std::string field) {
    return Arg{std::nullopt, Ref{std::move(label), std::move(field)}};
}
Arg named_ref(std::string n, std::string label, std::string field) {
    return Arg{std::move(n), Ref{std::move(label), std::move(field)}};
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

// ===================== Stage 2: references + the assumption ladder =====================

TEST_CASE("reference round-trip (the dataflow headline): $m1.field feeds a NEW message") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        if (in.payload.schema().name() == "Ping") {
            b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
        }
    };

    // m1 ← Pong{seq=7}.
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);
    REQUIRE(engine.buffer_at(1)->value.get("seq")->as_int() == 7);

    // The resolver, exercised standalone: $m1.seq → a typed Int Cell carrying 7.
    std::string rerr;
    std::optional<zen::Cell> cell = engine.resolve_ref(Ref{"m1", "seq"}, &rerr);
    REQUIRE_MESSAGE(cell.has_value(), rerr);
    CHECK(cell->kind() == zen::Kind::Int);
    CHECK(cell->as_int() == 7);

    // The wire: compose a NEW Ping whose seq IS $m1.seq — output→input, by reference.
    Composed c = engine.compose(responder.id, "Ping", 1, {ref("m1", "seq")});
    REQUIRE(c.status == Composed::Status::Ready);
    REQUIRE(c.ticket.valid());
    engine.pump();
    CHECK(engine.outcome(c.ticket).delivered);

    // The reply to the referenced send carries m1's value — the wire conducted it end to end.
    REQUIRE(engine.buffer_size() == 2);
    CHECK(engine.buffer_at(2)->name == "Pong");
    CHECK(engine.buffer_at(2)->value.get("seq")->as_int() == 7);
    CHECK(responder.shard->handled_values.back() == 7); // the 2nd Ping actually carried seq=7
}

TEST_CASE("ladder rung 1 — named wins: field=value assigns by name, any order") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered svc = register_probe(bus, {tagged_schema()});
    std::optional<zen::Value> got;
    svc.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) { got = in.payload; };

    // count given before label, by name — the order is irrelevant to a named assignment.
    Composed c = engine.compose(svc.id, "Tagged", 1,
                                {named("count", FieldValue{std::int64_t{9}}),
                                 named("label", FieldValue{std::string("hi")})});
    REQUIRE(c.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.outcome(c.ticket).delivered);
    REQUIRE(got.has_value());
    CHECK(got->get("label")->as_text() == "hi");
    CHECK(got->get("count")->as_int() == 9);
}

TEST_CASE("ladder rung 2 — positional fills open fields in declaration order") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered svc = register_probe(bus, {tagged_schema()});
    std::optional<zen::Value> got;
    svc.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) { got = in.payload; };

    // Bare ["hi"(Text), 5(Int)] → label, count in declaration order; both type-check.
    Composed c = engine.compose(svc.id, "Tagged", 1,
                                {lit(FieldValue{std::string("hi")}), lit(FieldValue{std::int64_t{5}})});
    REQUIRE(c.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.outcome(c.ticket).delivered);
    REQUIRE(got.has_value());
    CHECK(got->get("label")->as_text() == "hi");
    CHECK(got->get("count")->as_int() == 5);
}

TEST_CASE("ladder — positional FAILS THROUGH, type-directed lands the value in its unique field") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered svc = register_probe(bus, {tagged_schema()});
    std::optional<zen::Value> got;
    svc.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) { got = in.payload; };

    // A lone Int 5. Positional would put it in slot 0 (label:Text) — a type mismatch, so
    // positional fails AS A WHOLE and falls through; type-directed sees Int matches only
    // count and lands it there. label (optional) is left open → still Ready.
    Composed c = engine.compose(svc.id, "Tagged", 1, {lit(FieldValue{std::int64_t{5}})});
    REQUIRE(c.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.outcome(c.ticket).delivered);
    REQUIRE(got.has_value());
    CHECK(got->get("count")->as_int() == 5);   // the 5 was rerouted to its unique Int field
    CHECK(got->get("label") == nullptr);        // the optional Text field stayed unset
}

TEST_CASE("ladder rung 4 — genuine ambiguity returns NeedsInput and sends NOTHING") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered svc = register_probe(bus, {mix_schema()});
    bool handled = false;
    svc.shard->on_handle = [&](const Message&, Bus&, ProbeShard&) { handled = true; };

    // A lone Int 5 against Mix{name:Text, a:Int, b:Int}. Positional fails at slot 0 (Text);
    // type-directed finds 5 fits BOTH a and b — ambiguous. The ladder prompts, never guesses.
    Composed c = engine.compose(svc.id, "Mix", 1, {lit(FieldValue{std::int64_t{5}})});
    CHECK(c.status == Composed::Status::NeedsInput);
    CHECK_FALSE(c.ticket.valid());                 // nothing assembled
    CHECK(c.open_fields.size() == 3);              // name, a, b — none were placed
    REQUIRE(c.unplaced.size() == 1);
    CHECK(c.unplaced[0] == "5");                    // the value the ladder could not safely place
    engine.pump();
    CHECK_FALSE(handled);                           // no mis-send: the target saw nothing
    CHECK(engine.buffer_size() == 0);
}

TEST_CASE("gate-backstop: a wrong-typed reference is caught at compose, never mis-sent") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // m1 ← Pong{seq=7} (an Int we will try to misroute into a Text field).
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);

    // Note{body:Text}; feed $m1.seq (an Int) into body — a wrong-typed wire. The engine knows
    // both types and refuses at compose; nothing is assembled, nothing is sent.
    Registered notes = register_probe(bus, {note_schema()});
    bool handled = false;
    notes.shard->on_handle = [&](const Message&, Bus&, ProbeShard&) { handled = true; };

    Composed c = engine.compose(notes.id, "Note", 1, {named_ref("body", "m1", "seq")});
    CHECK(c.status == Composed::Status::Error);
    CHECK_FALSE(c.error.empty());
    CHECK_FALSE(c.ticket.valid());
    engine.pump();
    CHECK_FALSE(handled);                 // the target never received a wrong-typed message
    CHECK(engine.buffer_size() == 1);     // only m1 — no spurious reply
}

TEST_CASE("reference resolution errors are clean: empty buffer, missing entry, missing field") {
    Switchboard bus;
    ConsoleEngine engine(bus);

    // Into an empty buffer: $m1.x → no such entry (a clean error, never a crash).
    std::string e0;
    CHECK_FALSE(engine.resolve_ref(Ref{"m1", "x"}, &e0).has_value());
    CHECK_FALSE(e0.empty());

    // Populate m1 ← Pong{seq=7}.
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);

    std::string e1;
    CHECK_FALSE(engine.resolve_ref(Ref{"m9", "x"}, &e1).has_value()); // no such entry
    CHECK_FALSE(e1.empty());

    std::string e2;
    CHECK_FALSE(engine.resolve_ref(Ref{"m1", "nope"}, &e2).has_value()); // no such field
    CHECK_FALSE(e2.empty());

    // A bad reference inside compose is a hard Error (it never silently drops the arg).
    Composed c = engine.compose(responder.id, "Ping", 1, {ref("m9", "x")});
    CHECK(c.status == Composed::Status::Error);
    CHECK_FALSE(c.ticket.valid());
}

} // TEST_SUITE
