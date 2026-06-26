#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/console/console.hpp>
#include <zen/console/ui.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

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

} // TEST_SUITE
