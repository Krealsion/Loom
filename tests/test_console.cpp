// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "tui_render.hpp" // the shared renderer's tui_map_key, for the Action::None pin

#include <zen/console/console.hpp>
#include <zen/console/ui.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {
/// The R2F-C observation instrument, applied to console history (see the friend declarations in
/// zen/console/console.hpp): reads the window's OWN backing storage, so "the retained population
/// saturates" and "the storage stops growing" are stated as two separate assertions rather than one
/// inferred from process RSS -- RSS is allocator- and OS-sensitive and cannot tell a bounded ring
/// from a vector that is merely being trimmed. Adds no member and no code path.
struct ConsoleHistoryProbe {
    static std::size_t tap_slots(const ConsoleEngine& e) { return e.tap_.ring_.capacity(); }
    static std::size_t buffer_slots(const ConsoleEngine& e) {
        return e.reply_history().ring_.capacity();
    }
};
} // namespace loom

// The Console engine, proven with NO terminal — the headline. The engine is the durable
// spine; these tests drive its API directly (discover, gate-send, receive replies), so
// Stage 3's GUI inherits exactly this, only the skin new.

using namespace sbfx;          // Switchboard, ProbeWeave, register_probe, ping/pong, RefusalReason, ...
using namespace loom;  // ConsoleEngine, WeaveInfo, SendOutcome, FieldValue, ...
using loom::Disposition;
using loom::Ticket;

namespace {

// A shape the console code knows NOTHING about — defined only here, in the test.
std::shared_ptr<const loom::Schema> widget_schema() {
    static const auto s = loom::SchemaBuilder("Widget", 1).field("w", loom::Kind::Int).build();
    return s;
}
loom::Value widget(std::int64_t w) {
    loom::Value v(widget_schema());
    v.set("w", loom::Cell::integer(w));
    return v;
}

// ---- Stage 2 shapes, known only to the tests ----

// label (Text) is OPTIONAL and declared first, count (Int) is required and second — so a lone
// Int fails positional at slot 0 (Text) and is rescued by type-directed into count.
std::shared_ptr<const loom::Schema> tagged_schema() {
    static const auto s = loom::SchemaBuilder("Tagged", 1)
                              .field("label", loom::Kind::Text, /*required=*/false)
                              .field("count", loom::Kind::Int)
                              .build();
    return s;
}
// name (Text, first) then two same-typed Int fields — a lone Int can't go positional (slot 0 is
// Text) and matches BOTH a and b under type-direction: genuine ambiguity.
std::shared_ptr<const loom::Schema> mix_schema() {
    static const auto s = loom::SchemaBuilder("Mix", 1)
                              .field("name", loom::Kind::Text)
                              .field("a", loom::Kind::Int)
                              .field("b", loom::Kind::Int)
                              .build();
    return s;
}
std::shared_ptr<const loom::Schema> note_schema() {
    static const auto s = loom::SchemaBuilder("Note", 1).field("body", loom::Kind::Text).build();
    return s;
}

// ---- Stage 3 tree helpers ----
const Widget* find_region(const Widget& w, const std::string& id) {
    if (w.region_id == id) {
        return &w;
    }
    for (const Widget& c : w.children) {
        if (const Widget* r = find_region(c, id)) {
            return r;
        }
    }
    return nullptr;
}
int count_focused(const Widget& w) {
    int n = w.focused ? 1 : 0;
    for (const Widget& c : w.children) {
        n += count_focused(c);
    }
    return n;
}
bool any_item_contains(const Widget& w, const std::string& needle) {
    for (const std::string& it : w.items) {
        if (it.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
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
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Ping") {
            b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
        }
    };

    // Discovery sees the responder and its shapes — the console knows none of their meaning.
    bool found = false;
    for (const WeaveInfo& s : engine.weaves()) {
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
    responder.weave->on_handle = [&](const Message&, Bus&, ProbeWeave&) { handled = true; };

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
    const loom::DeliveryOutcome raw = bus.outcome(t);
    CHECK(raw.refusal.reason == RefusalReason::GateRefused);
    CHECK(raw.refusal.error.kind == loom::ErrorKind::MissingField);
    CHECK_FALSE(handled);                     // no silent mis-send: the responder never saw it
    CHECK(engine.buffer_size() == 0);
}

TEST_CASE("discovery + drive on a shape the console code has never seen") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // Widget is defined only in this test; the console code has no knowledge of it.
    Registered svc = register_probe(bus, {widget_schema()});
    svc.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
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
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
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
    auto unreg = loom::SchemaBuilder("Unregistered", 1).field("x", loom::Kind::Int).build();
    loom::Value v(unreg);
    v.set("x", loom::Cell::integer(9));
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
    auto lying = loom::SchemaBuilder("Pong", 1)
                     .field("seq", loom::Kind::Int)
                     .field("lie", loom::Kind::Text)
                     .build();
    loom::Value v(lying);
    v.set("seq", loom::Cell::integer(1));
    v.set("lie", loom::Cell::text("gotcha"));
    Ticket u = bus.send(engine.console_id(), Message(std::move(v)));
    bus.pump();
    CHECK(bus.outcome(u).disposition == Disposition::Refused);
    CHECK(bus.outcome(u).refusal.reason == RefusalReason::GateRefused);
    CHECK(bus.outcome(u).refusal.error.kind == loom::ErrorKind::SchemaMismatch);
    CHECK(engine.buffer_size() == 0); // the lie reached no one — gated against the registry shape
}

// ===================== Stage 2: references + the assumption ladder =====================

TEST_CASE("reference round-trip (the dataflow headline): $m1.field feeds a NEW message") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
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
    std::optional<loom::Cell> cell = engine.resolve_ref(Ref{"m1", "seq"}, &rerr);
    REQUIRE_MESSAGE(cell.has_value(), rerr);
    CHECK(cell->kind() == loom::Kind::Int);
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
    CHECK(responder.weave->handled_values.back() == 7); // the 2nd Ping actually carried seq=7
}

TEST_CASE("ladder rung 1 — named wins: field=value assigns by name, any order") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered svc = register_probe(bus, {tagged_schema()});
    std::optional<loom::Value> got;
    svc.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) { got = in.payload; };

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
    std::optional<loom::Value> got;
    svc.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) { got = in.payload; };

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
    std::optional<loom::Value> got;
    svc.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) { got = in.payload; };

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
    svc.weave->on_handle = [&](const Message&, Bus&, ProbeWeave&) { handled = true; };

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
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
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
    notes.weave->on_handle = [&](const Message&, Bus&, ProbeWeave&) { handled = true; };

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
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
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

// ===================== Stage 3: UI-as-data — the renderer-agnostic widget tree =====================

TEST_CASE("the bet, headless: the engine emits a semantic widget tree, NO renderer involved") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    // A reply in the buffer (m1) and a partial compose command.
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);

    UiState ui;
    ui.focus = Focus::Compose;
    ui.partial_input = std::to_string(responder.id.value) + " Ping 1";
    const Widget tree = emit_ui_tree(engine, ui);

    // The root is a VStack; the Bus region is an HStack of a Weaves List and a Tap Log.
    CHECK(tree.kind == WidgetKind::VStack);
    CHECK(tree.region_id == "root");
    const Widget* bus_region = find_region(tree, "bus");
    REQUIRE(bus_region != nullptr);
    CHECK(bus_region->kind == WidgetKind::Region);
    CHECK(bus_region->title == "Bus");
    REQUIRE(bus_region->children.size() == 1);
    CHECK(bus_region->children[0].kind == WidgetKind::HStack);

    const Widget* weaves = find_region(tree, "weaves");
    REQUIRE(weaves != nullptr);
    CHECK(weaves->kind == WidgetKind::List);
    CHECK(any_item_contains(*weaves, "Ping")); // discovery: the responder's shape, shown as data

    const Widget* tap = find_region(tree, "tap");
    REQUIRE(tap != nullptr);
    CHECK(tap->kind == WidgetKind::Log);

    const Widget* buffer = find_region(tree, "buffer");
    REQUIRE(buffer != nullptr);
    CHECK(buffer->kind == WidgetKind::List);
    CHECK(any_item_contains(*buffer, "m1")); // the reply, as a buffer row
    CHECK(any_item_contains(*buffer, "Pong"));

    const Widget* compose = find_region(tree, "compose");
    REQUIRE(compose != nullptr);
    CHECK(compose->kind == WidgetKind::Field);
    CHECK(compose->focused); // focus is Compose
    CHECK(compose->value == ui.partial_input);
    // The Field's hint IS the engine-produced guidance for this partial — not the renderer's.
    CHECK(compose->hint == guidance_for(engine, ui.partial_input));
    CHECK(count_focused(tree) == 1); // exactly one focused node (the controller is the single writer)
}

TEST_CASE("position is unrepresentable: structurally-identical trees are ==, content changes are !=") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    (void)register_probe(bus, {ping_schema()});

    UiState a;
    a.partial_input = "x";
    // Two emits of identical state produce equal trees — there is provably no hidden positional
    // state that could make them differ (defaulted operator== compares every member, recursively).
    CHECK(emit_ui_tree(engine, a) == emit_ui_tree(engine, a));

    UiState b = a;
    b.partial_input = "y"; // a content change is observable as a tree inequality
    CHECK(emit_ui_tree(engine, a) != emit_ui_tree(engine, b));
    // (The compile-time fence in ui.hpp — has_geometry traits + equality_comparable — makes adding
    // any x/y/w/h member fail to BUILD; that is the type-level half of this proof.)
}

TEST_CASE("two renderers, one tree: the headless outline reflects the SAME tree the TUI lays out") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    (void)register_probe(bus, {ping_schema()});
    UiState ui;
    const Widget tree = emit_ui_tree(engine, ui);

    const Widget before = tree;
    const std::string outline = render_outline(tree);
    CHECK(tree == before); // the renderer takes const& and does not mutate the tree

    CHECK(outline.find("VStack") != std::string::npos);
    CHECK(outline.find("Region \"Bus\"") != std::string::npos);
    CHECK(outline.find("List \"Weaves\"") != std::string::npos);
    CHECK(outline.find("Log \"Tap\"") != std::string::npos);
    CHECK(outline.find("List \"Buffer\"") != std::string::npos);
    CHECK(outline.find("Field") != std::string::npos);

    // The outline renderer IGNORES `weight` (a grow HINT, not a size): two trees differing only
    // in weight render to the SAME outline though they are unequal values.
    Widget w1 = vstack("r", {text_widget("a"), text_widget("b")});
    Widget w2 = w1;
    w2.children[0].weight = 9;
    CHECK(w1 != w2);
    CHECK(render_outline(w1) == render_outline(w2));
}

TEST_CASE("engine-produced guidance advances with the partial command (renderer-agnostic)") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    const std::string id = std::to_string(responder.id.value);

    // empty -> choose a weave; a weave id -> its shapes; a shape+version -> its fields.
    CHECK(guidance_for(engine, "").find("weave") != std::string::npos);
    CHECK(guidance_for(engine, id).find("Ping") != std::string::npos);
    CHECK(guidance_for(engine, id + " Ping 1").find("seq") != std::string::npos);

    // And the emitted Field carries exactly that engine-produced hint.
    UiState ui;
    ui.partial_input = id + " Ping 1";
    const Widget tree = emit_ui_tree(engine, ui);
    const Widget* compose = find_region(tree, "compose");
    REQUIRE(compose != nullptr);
    CHECK(compose->hint == guidance_for(engine, ui.partial_input));
}

TEST_CASE("message-driven: a delivered reply dirties + grows the buffer; a refusal dirties only the tap") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Ping" && in.payload.get("seq") != nullptr) {
            b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
        }
    };
    (void)engine.take_dirty(); // clear any construction-time flags

    // A reply delivered to the console marks the buffer region dirty and grows the buffer list.
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{3}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();
    const Dirty d1 = engine.take_dirty();
    CHECK(d1.buffer);            // the bus drove a buffer change
    CHECK(d1.tap);               // and the tap log
    UiState ui;
    const Widget tree = emit_ui_tree(engine, ui);
    const Widget* buffer = find_region(tree, "buffer");
    REQUIRE(buffer != nullptr);
    CHECK(any_item_contains(*buffer, "m1"));

    // A refused send (missing required field) dirties the tap but NOT the buffer — no reply grew.
    Ticket bad = engine.submit(responder.id, "Ping", 1, {}, &err);
    REQUIRE(bad.valid());
    engine.pump();
    const Dirty d2 = engine.take_dirty();
    CHECK(d2.tap);
    CHECK_FALSE(d2.buffer);      // nothing was delivered to the console
}

TEST_CASE("TUI smoke (headless): scripted semantic actions move focus, compose a guided send, buffer a reply") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    ConsoleUi ui(engine);

    // Always exactly one focused node; FocusNext rotates which region is focused.
    CHECK(count_focused(ui.tree()) == 1);
    CHECK(find_region(ui.tree(), "compose")->focused); // starts on the command field
    ui.dispatch({Action::FocusNext, 0});               // Compose -> Weaves
    CHECK(find_region(ui.tree(), "weaves")->focused);
    CHECK(count_focused(ui.tree()) == 1);

    // Activate on the weaves list prefills the command with the selected weave id and refocuses
    // the command field — the engine-agnostic "begin a send" affordance.
    ui.dispatch({Action::Activate, 0});
    CHECK(ui.state().partial_input == std::to_string(responder.id.value) + " ");
    CHECK(ui.state().focus == Focus::Compose);

    // Type the rest of a guided send via Edit actions (the semantic input seam, no raw keys),
    // then Submit — which composes via the ladder, gate-sends, and pumps.
    for (char ch : std::string("Ping 1 seq=9")) {
        ui.dispatch({Action::Edit, ch});
    }
    ui.dispatch({Action::Submit, 0});

    CHECK(engine.buffer_size() == 1);                 // the send went and the reply landed
    CHECK(ui.state().partial_input.empty());          // a Ready submit consumes the command
    const Widget tree = ui.tree();
    const Widget* buffer = find_region(tree, "buffer");
    REQUIRE(buffer != nullptr);
    CHECK(any_item_contains(*buffer, "Pong"));
    CHECK(count_focused(tree) == 1);
}

TEST_CASE("SelectAt names a row directly (the pointer's act), under the same single-writer clamp") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    (void)register_probe(bus, {ping_schema()});
    (void)register_probe(bus, {mix_schema()});
    (void)register_probe(bus, {note_schema()}); // three weaves -> indices 0..2 are real
    ConsoleUi ui(engine);

    ui.dispatch({Action::FocusNext, 0}); // Compose -> Weaves
    CHECK(ui.state().focus == Focus::Weaves);

    // A pointer names row 2 where keys would walk to it — the shared input vocabulary's one
    // Phase B addition, given real console semantics.
    ui.dispatch({Action::SelectAt, 0, 2});
    CHECK(ui.state().weave_cursor == 2);
    const Widget tree = ui.tree(); // bind first — a pointer into the temporary would dangle
    const Widget* weaves = find_region(tree, "weaves");
    REQUIRE(weaves != nullptr);
    CHECK(weaves->selected_index == 2);

    // The single-writer clamp: an out-of-range index is ignored, never stored (same discipline
    // as SelectDown — the controller is the only UiState writer).
    ui.dispatch({Action::SelectAt, 0, 99});
    CHECK(ui.state().weave_cursor == 2);
    ui.dispatch({Action::SelectAt, 0, -7});
    CHECK(ui.state().weave_cursor == 2);

    // On the buffer list the same action drives the buffer cursor (empty buffer: ignored).
    ui.dispatch({Action::FocusNext, 0}); // Weaves -> Buffer
    ui.dispatch({Action::SelectAt, 0, 0});
    CHECK(ui.state().buffer_cursor == 0); // unchanged default — no row 0 exists to select
}

TEST_CASE("TUI smoke (headless): an ambiguous command surfaces a NeedsInput prompt region, sends nothing") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // Mix{name:Text, a:Int, b:Int}: a lone Int is ambiguous (fits a AND b) — the ladder prompts.
    Registered svc = register_probe(bus, {mix_schema()});
    bool handled = false;
    svc.weave->on_handle = [&](const Message&, Bus&, ProbeWeave&) { handled = true; };
    ConsoleUi ui(engine);

    for (char ch : std::to_string(svc.id.value) + " Mix 1 5") {
        ui.dispatch({Action::Edit, ch});
    }
    ui.dispatch({Action::Submit, 0});

    const Widget tree = ui.tree();
    const Widget* prompt = find_region(tree, "prompt");
    REQUIRE(prompt != nullptr);                 // the tree gained a prompt region
    CHECK(prompt->title == "Needs input");
    engine.pump();
    CHECK_FALSE(handled);                        // nothing was sent — no mis-send
    CHECK(engine.buffer_size() == 0);
}

TEST_CASE("input: an unknown control byte maps to Action::None and changes nothing (headless)") {
    // A stub backend: tui_map_key touches it only for ESC continuations, not for a plain control byte.
    struct StubBackend final : loom::TerminalBackend {
        bool is_interactive() const override { return false; }
        bool size(int&, int&) override { return false; }
        int read_byte() override { return -1; }
        int read_byte_timeout(int) override { return -1; }
        void write(std::string_view) override {}
        void flush() override {}
    } term;

    Switchboard bus;
    ConsoleEngine engine(bus);
    ConsoleUi ui(engine);
    const UiState before = ui.state();

    // Ctrl-A (0x01): an unknown control byte. Before this pass it mapped to FocusNext (Ctrl-A cycled
    // focus); now it is a TRUE no-op — mapped to Action::None, dispatched as nothing.
    InputEvent ev;
    REQUIRE(tui_map_key(1, term, ev));
    CHECK(ev.action == Action::None);
    ui.dispatch(ev);

    const UiState& after = ui.state();
    CHECK(after.focus == before.focus);
    CHECK(after.partial_input == before.partial_input);
    CHECK(after.weave_cursor == before.weave_cursor);
    CHECK(after.buffer_cursor == before.buffer_cursor);
    CHECK(after.pending.has_value() == before.pending.has_value());
}

// ---- COLD-2 C-1: console history is bounded by capacity, never by lifetime throughput ----------
//
// Before this, ConsoleEngine::tap_ and ConsoleWeave::received_ were plain vectors that only ever
// grew: 200,000 bus events retained 200,000 tap entries and 200,000 Values (+42 MB RSS, measured,
// no ceiling). The console is the operator's window, so it is exactly the process left running for
// weeks — the same argument the bus already accepted for kJournalCapacity.
//
// Both surfaces are HISTORY: nothing is owed on them, so the oldest may be discarded. What may NOT
// happen is discarding it silently, or letting a label quietly re-bind to a different reply. Each
// case below therefore asserts three things together: the retained population, the eviction count,
// and the IDENTITY of what is retained.

namespace {

// One tap event per send, each with a UNIQUE identity, without registering a weave per event: a
// send to an unregistered id is refused, and the refusal is a real bus event carrying the target
// the operator asked for. That makes tap entry i recognizable as tap entry i, which is what pins
// off-by-one — a count alone cannot tell a window that slid by one from one that slid by two.
constexpr std::uint64_t kProbeTargetBase = 1'000'000;

void drive_tap_events(Switchboard& bus, ConsoleEngine& engine, WeaveId sender, std::size_t from,
                      std::size_t count) {
    for (std::size_t i = from; i < from + count; ++i) {
        (void)bus.send_as(sender, WeaveId{kProbeTargetBase + i},
                          Message(ping(static_cast<std::int64_t>(i))));
        engine.pump();
    }
}

// The whole retained window, checked as a sequence: entry j must be the (evicted + j)-th event
// ever observed. Any rotation, any dropped-from-the-wrong-end, any duplicated slot fails here.
bool tap_window_is_exact(const ConsoleEngine& engine) {
    const std::vector<TapEvent> window = engine.tap();
    const std::uint64_t base = engine.evicted().tap;
    for (std::size_t j = 0; j < window.size(); ++j) {
        if (window[j].target.value != kProbeTargetBase + base + j) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("C-1: the tap retains a bounded window — every transition across the capacity") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered sender = register_probe(bus, {ping_schema()});

    // 0 — a fresh console has retained nothing and discarded nothing. Its storage is already the
    // whole window: the slots are claimed once, so no insert ever reallocates and the storage claim
    // does not depend on a growth policy.
    CHECK(engine.tap().empty());
    CHECK(engine.evicted().tap == 0);
    CHECK(ConsoleHistoryProbe::tap_slots(engine) == kConsoleTapCapacity);
    CHECK(ConsoleHistoryProbe::buffer_slots(engine) == kConsoleBufferCapacity);

    // 1 — the first observation is retained whole.
    drive_tap_events(bus, engine, sender.id, 0, 1);
    CHECK(engine.tap().size() == 1);
    CHECK(engine.evicted().tap == 0);
    CHECK(engine.tap().front().kind == "Refused"); // a real bus event, not a synthesized one
    CHECK(tap_window_is_exact(engine));

    // capacity - 1 — still complete history: nothing discarded yet.
    drive_tap_events(bus, engine, sender.id, 1, kConsoleTapCapacity - 2);
    CHECK(engine.tap().size() == kConsoleTapCapacity - 1);
    CHECK(engine.evicted().tap == 0);
    CHECK(tap_window_is_exact(engine));

    // capacity — exactly full, and STILL complete: the boundary is inclusive, so the entry that
    // fills the window is not the one that evicts.
    drive_tap_events(bus, engine, sender.id, kConsoleTapCapacity - 1, 1);
    CHECK(engine.tap().size() == kConsoleTapCapacity);
    CHECK(engine.evicted().tap == 0);
    CHECK(engine.tap().front().target.value == kProbeTargetBase); // event 0 is still here
    CHECK(tap_window_is_exact(engine));

    // capacity + 1 — the first eviction: the count stops rising, the oldest is gone by exactly one,
    // the newest is the event just emitted.
    drive_tap_events(bus, engine, sender.id, kConsoleTapCapacity, 1);
    CHECK(engine.tap().size() == kConsoleTapCapacity); // did NOT grow
    CHECK(engine.evicted().tap == 1);
    CHECK(engine.tap().front().target.value == kProbeTargetBase + 1); // event 0 discarded
    CHECK(engine.tap().back().target.value == kProbeTargetBase + kConsoleTapCapacity);
    CHECK(tap_window_is_exact(engine));

    // capacity + several — the window keeps sliding, one in one out, order intact throughout.
    drive_tap_events(bus, engine, sender.id, kConsoleTapCapacity + 1, 7);
    CHECK(engine.tap().size() == kConsoleTapCapacity);
    CHECK(engine.evicted().tap == 8);
    CHECK(engine.tap().front().target.value == kProbeTargetBase + 8);
    CHECK(engine.tap().back().target.value == kProbeTargetBase + kConsoleTapCapacity + 7);
    CHECK(tap_window_is_exact(engine));

    // The storage claim, not merely the logical one: after saturation the window's own slot count
    // is the capacity and stays there. A fix that kept size() bounded while the backing store grew
    // with lifetime throughput would pass every check above and still be the defect.
    CHECK(ConsoleHistoryProbe::tap_slots(engine) == kConsoleTapCapacity);
}

TEST_CASE("C-1: the reply buffer retains a bounded window, and mN stays a stable identity") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    // A responder that answers Ping{seq} with Pong{seq}: the reply's payload carries the number
    // that its label must agree with, so "m17 holds reply 17" is checkable, not assumed.
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    const auto deliver = [&](std::int64_t seq) {
        std::string err;
        const Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", seq}}, &err);
        REQUIRE_MESSAGE(t.valid(), err);
        engine.pump();
    };

    CHECK(engine.buffer_size() == 0);
    CHECK(engine.evicted().buffer == 0);

    // Fill to exactly the capacity: labels m1..mCapacity, each holding its own seq.
    for (std::size_t i = 1; i <= kConsoleBufferCapacity; ++i) {
        deliver(static_cast<std::int64_t>(i));
    }
    REQUIRE(engine.buffer_size() == kConsoleBufferCapacity);
    CHECK(engine.evicted().buffer == 0);
    CHECK(engine.buffer_at(1)->value.get("seq")->as_int() == 1); // m1 still means reply 1
    CHECK(engine.buffer_at(kConsoleBufferCapacity)->value.get("seq")->as_int() ==
          static_cast<std::int64_t>(kConsoleBufferCapacity));

    // One past: the oldest reply is evicted and its LABEL refuses. It does not answer with the
    // reply that now occupies that slot — the whole point of a stable identity.
    deliver(static_cast<std::int64_t>(kConsoleBufferCapacity) + 1);
    CHECK(engine.buffer_size() == kConsoleBufferCapacity);
    CHECK(engine.evicted().buffer == 1);
    CHECK_FALSE(engine.buffer_at(1).has_value());
    REQUIRE(engine.buffer_at(2).has_value());
    CHECK(engine.buffer_at(2)->label == "m2");
    CHECK(engine.buffer_at(2)->value.get("seq")->as_int() == 2); // unmoved, un-renamed

    // Several more, then read the ENTIRE retained window back: contiguous, chronological, and
    // label N holds payload N for every one of them.
    for (std::size_t i = 0; i < 9; ++i) {
        deliver(static_cast<std::int64_t>(kConsoleBufferCapacity + 2 + i));
    }
    const std::uint64_t gone = engine.evicted().buffer;
    CHECK(gone == 10);
    CHECK(engine.buffer_size() == kConsoleBufferCapacity);
    CHECK_FALSE(engine.buffer_at(static_cast<std::size_t>(gone)).has_value()); // last evicted
    for (std::uint64_t n = gone + 1; n <= gone + kConsoleBufferCapacity; ++n) {
        const std::optional<BufferEntry> e = engine.buffer_at(static_cast<std::size_t>(n));
        REQUIRE_MESSAGE(e.has_value(), "retained label m" << n << " must resolve");
        CHECK(e->label == "m" + std::to_string(n));
        CHECK(e->name == "Pong");
        CHECK(e->value.get("seq")->as_int() == static_cast<std::int64_t>(n));
    }
    // One past the newest label was never received — a different absence from an evicted one.
    CHECK_FALSE(engine.buffer_at(static_cast<std::size_t>(gone + kConsoleBufferCapacity + 1))
                    .has_value());

    CHECK(ConsoleHistoryProbe::buffer_slots(engine) == kConsoleBufferCapacity);
}

TEST_CASE("C-1: a reference to an evicted reply refuses and SAYS SO; a retained one is unchanged") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    const auto deliver = [&](std::int64_t seq) {
        std::string err;
        (void)engine.submit(responder.id, "Ping", 1, {{"seq", seq}}, &err);
        engine.pump();
    };

    for (std::size_t i = 1; i <= 3; ++i) {
        deliver(static_cast<std::int64_t>(i));
    }
    // What $m3.seq means BEFORE any eviction — the value an operator would have written down.
    std::string err;
    const std::optional<Cell> before = engine.resolve_ref(Ref{"m3", "seq"}, &err);
    REQUIRE(before.has_value());
    CHECK(before->as_int() == 3);

    // Saturate past m3 so it is evicted.
    for (std::size_t i = 4; i <= kConsoleBufferCapacity + 5; ++i) {
        deliver(static_cast<std::int64_t>(i));
    }
    REQUIRE(engine.evicted().buffer >= 3);

    err.clear();
    CHECK_FALSE(engine.resolve_ref(Ref{"m3", "seq"}, &err).has_value());
    CHECK_MESSAGE(err.find("evicted") != std::string::npos, err); // not "no such entry" — the truth
    CHECK(err.find("m3") != std::string::npos);

    // A never-received label is a DIFFERENT absence, and still reads as one.
    err.clear();
    CHECK_FALSE(engine.resolve_ref(Ref{"m99999", "seq"}, &err).has_value());
    CHECK_MESSAGE(err.find("no such buffer entry") != std::string::npos, err);

    // And a still-retained reference means exactly what it always meant.
    err.clear();
    const std::optional<Cell> still = engine.resolve_ref(Ref{"m50", "seq"}, &err);
    REQUIRE_MESSAGE(still.has_value(), err);
    CHECK(still->as_int() == 50);
}

TEST_CASE("C-1: long run — retained population is independent of lifetime throughput") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered sender = register_probe(bus, {ping_schema()});

    // 10x the tap capacity, in laps, checking after each that the window has stopped growing and
    // that the books balance: retained + discarded == observed, always.
    constexpr std::size_t kLaps = 10;
    for (std::size_t lap = 1; lap <= kLaps; ++lap) {
        drive_tap_events(bus, engine, sender.id, (lap - 1) * kConsoleTapCapacity,
                         kConsoleTapCapacity);
        const std::uint64_t observed = static_cast<std::uint64_t>(lap * kConsoleTapCapacity);
        CHECK(engine.tap().size() == kConsoleTapCapacity);            // constant after saturation
        CHECK(engine.evicted().tap == observed - kConsoleTapCapacity); // advances truthfully
        CHECK(engine.tap().back().target.value == kProbeTargetBase + observed - 1); // newest advances
        CHECK(tap_window_is_exact(engine));
        CHECK(ConsoleHistoryProbe::tap_slots(engine) == kConsoleTapCapacity); // storage flat too
    }
    // The headline, stated as one assertion: ten thousand events, one thousand and twenty-four
    // retained. The window is a function of the capacity, not of how long the process has run.
    CHECK(engine.evicted().tap + engine.tap().size() == kLaps * kConsoleTapCapacity);
}

TEST_CASE("C-1: a fresh console starts a fresh retention window") {
    // There is no clear/reset operation on console history — the reset IS object lifetime, and this
    // pins that the eviction counters are a property of THIS window rather than a lifetime statistic
    // some future console would inherit.
    Switchboard bus;
    {
        ConsoleEngine engine(bus);
        Registered sender = register_probe(bus, {ping_schema()});
        drive_tap_events(bus, engine, sender.id, 0, kConsoleTapCapacity + 5);
        REQUIRE(engine.evicted().tap == 5);
    } // the console leaves the bus, taking its window with it

    ConsoleEngine fresh(bus);
    CHECK(fresh.tap().empty());
    CHECK(fresh.buffer_size() == 0);
    CHECK(fresh.evicted().tap == 0);
    CHECK(fresh.evicted().buffer == 0);
    CHECK_FALSE(fresh.buffer_at(1).has_value()); // labels restart, because the history did
}

TEST_CASE("C-1: the operator can SEE that older evidence was discarded") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    UiState ui;

    // Complete history: the panes claim nothing about eviction, because there was none.
    {
        const Widget tree = emit_ui_tree(engine, ui);
        CHECK(find_region(tree, "tap")->title == "Tap");
        CHECK(find_region(tree, "buffer")->title == "Buffer");
    }

    for (std::size_t i = 1; i <= kConsoleBufferCapacity + 3; ++i) {
        std::string err;
        (void)engine.submit(responder.id, "Ping", 1, {{"seq", static_cast<std::int64_t>(i)}}, &err);
        engine.pump();
    }
    REQUIRE(engine.evicted().buffer == 3);

    const Widget tree = emit_ui_tree(engine, ui);
    const Widget* buffer = find_region(tree, "buffer");
    REQUIRE(buffer != nullptr);
    // The heading, not an item: a Log/List shows its TAIL when it overflows, so a note pushed in as
    // the oldest row would be the first thing to scroll off the screen it exists to warn.
    CHECK(buffer->title == "Buffer (3 evicted)");
    REQUIRE_FALSE(buffer->items.empty());
    CHECK(buffer->items.front().rfind("m4:", 0) == 0);  // the window starts at the oldest RETAINED
    CHECK(buffer->items.back().rfind("m67:", 0) == 0);  // ... and ends at the newest
    CHECK(buffer->items.size() == kConsoleBufferCapacity);

    // The tap saw more events than the buffer saw replies, so it evicted too, and says so.
    const Widget* tap = find_region(tree, "tap");
    REQUIRE(tap != nullptr);
    CHECK(tap->title == "Tap");                 // still under capacity here: no claim of eviction
    CHECK(engine.evicted().tap == 0);
}

} // TEST_SUITE
