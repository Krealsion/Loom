// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "terminal.hpp"   // TerminalBackend, for the capture-backend TUI pin
#include "tui_render.hpp" // tui_draw — the TUI laying out the evolved vocabulary

#include <zen/console/console.hpp>
#include <zen/console/ui.hpp>
#include <zen/ui/component.hpp>
#include <zen/gate.hpp>
#include <zen/registry.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The UI-Builder component vocabulary (Phase A): components-as-gated-Values, the ONE-tree
// unification with the console's widget tree, tree-ness as the vocabulary's own decode check
// AFTER the gate, stress placeholders, and the view/presenter split as separable data.

using namespace sbfx;
using namespace loom;

namespace {

// A contract shape the vocabulary code knows NOTHING about — defined only here, in the test
// (the same discipline as the console suite: the vocabulary must be schema-agnostic).
std::shared_ptr<const loom::Schema> task_schema() {
    static const auto s = loom::SchemaBuilder("Task", 1)
                              .field("title", loom::Kind::Text)
                              .field("count", loom::Kind::Int)
                              .list("tags", loom::type_of(loom::Kind::Text))
                              .field("blob", loom::Kind::Bytes)
                              .list("chunks", loom::type_of(loom::Kind::Bytes))
                              .build();
    return s;
}

// A representative schematic: a card FOR the Task shape — bound fields previewing stress
// values, an open component slot, and a route-slotted nav hole.
Widget task_card_tree() {
    Widget title = bound_text("card-title", "title", loom::Kind::Text);
    Widget count = bound_field("count>", "count", loom::Kind::Int);
    Widget tags = bound_list("card-tags", "Tags", "tags");
    Widget actions = open_slot("actions", "Component");
    Widget nav = open_slot("nav", "Route");
    std::vector<Widget> children;
    children.push_back(std::move(title));
    children.push_back(std::move(count));
    children.push_back(std::move(tags));
    children.push_back(std::move(actions));
    children.push_back(std::move(nav));
    return region("card", "Task", vstack("card-body", std::move(children)));
}

// Well-formed wire nodes, for forging hostile node lists around. A leaf (Text) carries no
// children by the arity rule; a stack may carry any — hostile child wiring uses stacks.
UiNode leaf_node(std::string content) {
    UiNode n;
    n.kind = "Text";
    n.content = std::move(content);
    n.overflow = "Grow";
    return n;
}
UiNode stack_node() {
    UiNode n;
    n.kind = "VStack";
    n.overflow = "Grow";
    return n;
}

// The full wire path: struct -> Value -> canonical bytes -> Unverified -> the SAME gate as the
// bus -> struct. REQUIREs each step so a failure names its stage.
UiComponent gate_round_trip(const UiComponent& c) {
    const std::string bytes = loom::serialize(loom::to_value(c));
    const loom::Unverified u = loom::parse(bytes);
    REQUIRE(u.well_formed());
    loom::Admission a = loom::admit(u, loom::schema_of<UiComponent>());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return loom::from_value<UiComponent>(std::move(a).value());
}

// A capture backend: tui_draw's frames land in `out` instead of a terminal.
struct CaptureBackend final : loom::TerminalBackend {
    std::string out;
    bool is_interactive() const override { return false; }
    bool size(int&, int&) override { return false; }
    int read_byte() override { return -1; }
    int read_byte_timeout(int) override { return -1; }
    void write(std::string_view s) override { out.append(s); }
    void flush() override {}
};

} // namespace

TEST_SUITE("component") {

TEST_CASE("the zen.ui.* schemas are byte-identical to hand-built twins (same door, same content-id)") {
    // zen.ui.Node — the flat wire node. Field order is the frozen v1 contract.
    const auto hand_node = loom::SchemaBuilder("zen.ui.Node", 1)
                               .field("kind", loom::Kind::Text)
                               .field("region_id", loom::Kind::Text)
                               .field("title", loom::Kind::Text)
                               .field("content", loom::Kind::Text)
                               .field("prompt", loom::Kind::Text)
                               .field("value", loom::Kind::Text)
                               .field("hint", loom::Kind::Text)
                               .list("items", loom::type_of(loom::Kind::Text))
                               .field("selected_index", loom::Kind::Int)
                               .field("activatable", loom::Kind::Bool)
                               .field("editable", loom::Kind::Bool)
                               .field("reorderable", loom::Kind::Bool)
                               .field("focused", loom::Kind::Bool)
                               .field("weight", loom::Kind::Int)
                               .field("overflow", loom::Kind::Text)
                               .field("from_field", loom::Kind::Text)
                               .field("route_to", loom::Kind::Text)
                               .field("slot_name", loom::Kind::Text)
                               .field("slot_accepts", loom::Kind::Text)
                               .list("children", loom::type_of(loom::Kind::Int))
                               .build();
    CHECK(loom::schema_of<UiNode>()->content_id() == hand_node->content_id());

    // zen.ui.Component — the schematic: identity/address, contract, the flat tree.
    const auto hand_component = loom::SchemaBuilder("zen.ui.Component", 1)
                                    .field("name", loom::Kind::Text)
                                    .field("contract_name", loom::Kind::Text)
                                    .field("contract_version", loom::Kind::Int)
                                    .list("nodes", loom::type_message(hand_node))
                                    .build();
    CHECK(loom::schema_of<UiComponent>()->content_id() == hand_component->content_id());

    // zen.ui.Presenter — the OTHER half of the split.
    const auto hand_presenter = loom::SchemaBuilder("zen.ui.Presenter", 1)
                                    .field("view", loom::Kind::Text)
                                    .field("source_role", loom::Kind::Text)
                                    .build();
    CHECK(loom::schema_of<UiPresenter>()->content_id() == hand_presenter->content_id());
}

TEST_CASE("a schematic is data: a component round-trips through the gate and rebuilds the identical tree") {
    const Widget original = task_card_tree();
    const UiComponent card = make_component("task-card", "Task", 1, original);

    // Wire: serialize -> parse -> the SAME structural validator as the live bus -> struct.
    const std::string bytes = loom::serialize(loom::to_value(card));
    const loom::Unverified u = loom::parse(bytes);
    REQUIRE(u.well_formed());
    CHECK(u.claimed_name() == "zen.ui.Component");
    loom::Admission a = loom::admit(u, loom::schema_of<UiComponent>());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    const UiComponent back = loom::from_value<UiComponent>(std::move(a).value());

    CHECK(back.name == "task-card");
    CHECK(back.contract_name == "Task");
    CHECK(back.contract_version == 1);

    // The rebuilt tree is the SAME value — flatten/tree_of are a lossless pair.
    const TreeResult rebuilt = tree_of(back);
    REQUIRE_MESSAGE(rebuilt.root.has_value(), rebuilt.error);
    CHECK(*rebuilt.root == original);

    // Canonical bytes: one byte representation — re-serializing the round-trip is identical.
    CHECK(loom::serialize(loom::to_value(back)) == bytes);
}

TEST_CASE("a component admits against a Registry like any shared value (the load-a-toy path)") {
    loom::Registry reg;
    (void)reg.register_schema(loom::schema_of<UiNode>());
    (void)reg.register_schema(loom::schema_of<UiComponent>());
    (void)reg.register_schema(loom::schema_of<UiPresenter>());

    const UiComponent card = make_component("task-card", "Task", 1, task_card_tree());
    const loom::Unverified u = loom::parse(loom::serialize(loom::to_value(card)));
    loom::Admission a = loom::admit(u, reg);
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    CHECK(a.value().schema().name() == "zen.ui.Component");
}

TEST_CASE("ONE tree, not two: the console's own emitted tree crosses the wire as a component and comes back identical") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    Registered responder = register_probe(bus, {ping_schema(), pong_schema()});
    responder.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };
    std::string err;
    Ticket t = engine.submit(responder.id, "Ping", 1, {{"seq", std::int64_t{7}}}, &err);
    REQUIRE_MESSAGE(t.valid(), err);
    engine.pump();

    UiState ui;
    ui.partial_input = std::to_string(responder.id.value) + " Ping 1";
    const Widget live = emit_ui_tree(engine, ui);

    // The live console tree — focus marker, cursor, buffer rows and all — is expressible in the
    // component vocabulary and survives the full gated wire path bit-for-bit.
    const UiComponent snapshot = make_component("console", "", 0, live);
    const UiComponent back = gate_round_trip(snapshot);
    const TreeResult rebuilt = tree_of(back);
    REQUIRE_MESSAGE(rebuilt.root.has_value(), rebuilt.error);
    CHECK(*rebuilt.root == live);

    // And both renderers consume the REBUILT tree exactly as the original: the TUI still
    // renders the evolved vocabulary (the live second-projection agnosticism check).
    CHECK(render_outline(*rebuilt.root) == render_outline(live));
    CaptureBackend term;
    tui_draw(*rebuilt.root, 24, 80, term);
    CHECK(term.out.find("Weaves") != std::string::npos);
    CHECK(term.out.find("compose>") != std::string::npos);
}

TEST_CASE("the gate cannot see tree-ness: a shape-conforming cycle passes the gate and is refused by tree_of") {
    // Forge the hostile frame directly in wire form — the honest Widget constructors cannot
    // express a cycle, so the test builds what the API makes unsayable (a detached A<->B loop).
    UiComponent hostile;
    hostile.name = "trap";
    hostile.nodes.push_back(leaf_node("root"));
    hostile.nodes.push_back(stack_node());
    hostile.nodes.push_back(stack_node());
    hostile.nodes[1].children.push_back(2);
    hostile.nodes[2].children.push_back(1);

    // The gate ADMITS it — every field present and well-kinded is all a schema can promise.
    const loom::Unverified u = loom::parse(loom::serialize(loom::to_value(hostile)));
    REQUIRE(u.well_formed());
    loom::Admission a = loom::admit(u, loom::schema_of<UiComponent>());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));

    // The vocabulary's own decode layer refuses it, naming the offense.
    const TreeResult r = tree_of(loom::from_value<UiComponent>(std::move(a).value()));
    CHECK_FALSE(r.root.has_value());
    CHECK(r.error.find("unreachable") != std::string::npos);
}

TEST_CASE("EVERY malformed-frame class passes the real gate and is refused by tree_of with a reason") {
    // Each subcase runs the full wire path (serialize -> parse -> the same admit as the bus) —
    // gate_round_trip REQUIREs the admission succeeded — and only THEN is refused by tree_of.
    // That pins, per class, both halves of the honest layering: the gate cannot see tree-ness,
    // and the vocabulary's own decode never lets a malformed frame become a silent blank.
    UiComponent c;
    c.name = "trap";
    std::string expect;

    SUBCASE("no nodes at all") {
        expect = "no nodes";
    }
    SUBCASE("a child index out of range") {
        c.nodes.push_back(stack_node());
        c.nodes[0].children.push_back(7);
        expect = "out of range";
    }
    SUBCASE("a negative child index") {
        c.nodes.push_back(stack_node());
        c.nodes[0].children.push_back(-1);
        expect = "out of range";
    }
    SUBCASE("one node claimed by two parents (the reached-twice detector)") {
        c.nodes.push_back(stack_node());
        c.nodes.push_back(leaf_node("shared"));
        c.nodes[0].children.push_back(1);
        c.nodes[0].children.push_back(1);
        expect = "reached twice";
    }
    SUBCASE("the root referenced as a child (a back-edge)") {
        c.nodes.push_back(stack_node());
        c.nodes.push_back(stack_node());
        c.nodes[0].children.push_back(1);
        c.nodes[1].children.push_back(0);
        expect = "reached twice";
    }
    SUBCASE("an unknown kind spelling") {
        c.nodes.push_back(leaf_node("root"));
        c.nodes[0].kind = "Blob";
        expect = "unknown kind 'Blob'";
    }
    SUBCASE("an unknown overflow spelling") {
        c.nodes.push_back(leaf_node("root"));
        c.nodes[0].overflow = "Explode";
        expect = "unknown overflow 'Explode'";
    }
    SUBCASE("selected_index below -1") {
        c.nodes.push_back(leaf_node("root"));
        c.nodes[0].selected_index = -5;
        expect = "selected_index";
    }
    SUBCASE("weight beyond the relative-hint range") {
        c.nodes.push_back(leaf_node("root"));
        c.nodes[0].weight = 70000;
        expect = "weight";
    }
    SUBCASE("a contract_version no schema version can hold (negative)") {
        c.nodes.push_back(leaf_node("root"));
        c.contract_version = -1;
        expect = "contract_version";
    }
    SUBCASE("a contract_version no schema version can hold (past u32)") {
        c.nodes.push_back(leaf_node("root"));
        c.contract_version = 0x1'0000'0000ll;
        expect = "contract_version";
    }
    SUBCASE("a Region wrapping two children (the TUI draws one, the outline both — refused)") {
        UiNode r;
        r.kind = "Region";
        r.title = "R";
        r.overflow = "Grow";
        r.children.push_back(1);
        r.children.push_back(2);
        c.nodes.push_back(r);
        c.nodes.push_back(leaf_node("a"));
        c.nodes.push_back(leaf_node("b"));
        expect = "exactly one child";
    }
    SUBCASE("a Region wrapping nothing") {
        UiNode r;
        r.kind = "Region";
        r.title = "R";
        r.overflow = "Grow";
        c.nodes.push_back(r);
        expect = "exactly one child";
    }
    SUBCASE("a leaf kind smuggling children (renderers would silently diverge)") {
        c.nodes.push_back(leaf_node("root"));
        c.nodes.push_back(leaf_node("stowaway"));
        c.nodes[0].children.push_back(1);
        expect = "carries no children";
    }
    SUBCASE("a hostile deep chain is bounded by the depth cap, not the C++ stack") {
        for (int i = 0; i < 300; ++i) {
            c.nodes.push_back(stack_node());
        }
        for (int i = 0; i < 299; ++i) {
            c.nodes[static_cast<std::size_t>(i)].children.push_back(i + 1);
        }
        expect = "deeper than";
    }

    const UiComponent back = gate_round_trip(c); // the gate ADMITS the forged frame...
    const TreeResult r = tree_of(back);          // ...and the decode layer refuses it.
    CHECK_FALSE(r.root.has_value());
    REQUIRE(!expect.empty());
    CHECK_MESSAGE(r.error.find(expect) != std::string::npos, r.error);
}

TEST_CASE("accept-side boundaries: documented-legal edges rebuild rather than refuse") {
    // weight at its ceiling, a cursor over an EMPTY list (the live console emits exactly that
    // when a focused list has no rows), and selected_index at INT_MAX (the type boundary).
    Widget edge = list("edge", "Edge", {}, /*selected_index=*/0, /*activatable=*/true,
                       /*focused=*/true);
    edge.weight = 65535;
    Widget root = vstack("r", {edge});
    const UiComponent back = gate_round_trip(make_component("edges", "", 0, root));
    const TreeResult r = tree_of(back);
    REQUIRE_MESSAGE(r.root.has_value(), r.error);
    CHECK(*r.root == root);

    UiComponent c;
    c.nodes.push_back(leaf_node("root"));
    c.nodes[0].selected_index = INT_MAX;
    const TreeResult r2 = tree_of(c);
    REQUIRE_MESSAGE(r2.root.has_value(), r2.error);
    CHECK(r2.root->selected_index == INT_MAX);
}

TEST_CASE("the lossless pair is BOUNDED and says so: flatten past the depth cap yields a component tree_of refuses") {
    // flatten() takes locally-built (trusted) trees and has no cap; the pair's losslessness is
    // scoped to kMaxUiDepth. This pins the documented bound instead of hiding it.
    Widget deep = text_widget("leaf");
    for (int i = 0; i < kMaxUiDepth + 5; ++i) {
        deep = vstack("", {std::move(deep)});
    }
    const UiComponent too_deep = make_component("deep", "", 0, deep);
    const TreeResult r = tree_of(too_deep);
    CHECK_FALSE(r.root.has_value());
    CHECK(r.error.find("deeper than") != std::string::npos);
}

TEST_CASE("every wire field crosses non-default: a maximal node pair round-trips bit-for-bit") {
    // One List and one Slot between them exercise all 20 zen.ui.Node fields with non-default
    // values (deleting either direction's copy of ANY field turns this red — the mutation pin
    // for node_of/build_node omissions).
    Widget rows = list("rows-id", "Rows Title", {"row one", "row two"}, 1, /*activatable=*/true,
                       /*focused=*/true, Overflow::Wrap);
    rows.reorderable = true;
    rows.from_field = "tags";
    rows.route_to = "detail-view";
    rows.weight = 65535;

    Widget hole = slot("hole-name", "Int", {text_widget("preview")});
    hole.region_id = "hole-id";
    hole.title = "Hole Title";
    hole.content = "hole content";
    hole.prompt = "hole prompt";
    hole.value = "hole value";
    hole.hint = "hole hint";
    hole.editable = true;
    hole.overflow = Overflow::Truncate;

    const Widget root = hstack("max-root", {rows, hole});
    const UiComponent back = gate_round_trip(make_component("maximal", "Task", 1, root));
    const TreeResult r = tree_of(back);
    REQUIRE_MESSAGE(r.root.has_value(), r.error);
    CHECK(*r.root == root);
}

TEST_CASE("stress placeholders are the DEFAULT: a schematic previews the value that reveals the seam") {
    // The canon: widest int, longest text with an unbreakable word, the empty list, deep nesting.
    CHECK(stress_number() == "-9223372036854775808"); // sign + 19 digits, the widest Int spelling
    CHECK(stress_text().size() >= 200);
    const std::string text = stress_text();
    const std::string::size_type last_space = text.rfind(' ');
    REQUIRE(last_space != std::string::npos);
    CHECK(text.size() - last_space - 1 >= 64); // one unbroken >=64-char word defeats wrapping
    CHECK(stress_rows().empty());              // the zero-case: does layout survive nothing?

    // The design-time constructors pick these BY DEFAULT — no happy-path preview exists to pick.
    CHECK(bound_text("t", "title", loom::Kind::Text).content == stress_text());
    CHECK(bound_field("count>", "count", loom::Kind::Int).value == stress_number());
    CHECK(bound_field("count>", "count", loom::Kind::Int).editable); // a Field's declared nature
    CHECK(bound_list("l", "Tags", "tags").items.empty());

    // A component slot previews the deeply-nested case; a scalar slot its kind's stress value.
    const Widget actions = open_slot("actions", "Component");
    CHECK(actions.kind == WidgetKind::Slot);
    CHECK(actions.slot_accepts == "Component");
    REQUIRE(actions.children.size() == 1);
    int depth = 0;
    for (const Widget* w = &actions.children[0]; !w->children.empty();
         w = &w->children.back()) {
        ++depth;
    }
    CHECK(depth >= 8); // the ladder really is deep
    const Widget count_slot = open_slot("count", "Int");
    REQUIRE(count_slot.children.size() == 1);
    CHECK(count_slot.children[0].content == stress_number());
}

TEST_CASE("the vocabulary reads as intent: outline of a schematic names bindings, slots, routes, interactions") {
    Widget card = task_card_tree();
    // A routed activatable row: "activating this goes to the settings view."
    Widget nav_row = list("nav-row", "Open settings", {"settings"}, -1, /*activatable=*/true,
                          /*focused=*/false);
    nav_row.route_to = "settings-view";
    card.children[0].children.push_back(std::move(nav_row)); // into the card's body stack

    const std::string outline = render_outline(card);
    CHECK(outline.find("from=title") != std::string::npos);            // data binding
    CHECK(outline.find("from=count") != std::string::npos);
    CHECK(outline.find("Slot \"actions\" accepts=Component") != std::string::npos); // typed hole
    CHECK(outline.find("Slot \"nav\" accepts=Route") != std::string::npos);
    CHECK(outline.find("(activatable)") != std::string::npos);         // interaction intent
    CHECK(outline.find("(editable)") != std::string::npos);
    CHECK(outline.find("-> settings-view") != std::string::npos);      // navigation intent

    // Interaction intent and routes are CONTENT (the outline shows them); weight AND overflow
    // stay HINTS (the outline ignores both) — the two-renderer discipline distinguishing
    // meaning from medium.
    Widget plain = list("l", "L", {"row"}, -1, false, false);
    Widget acting = plain;
    acting.activatable = true;
    CHECK(render_outline(plain) != render_outline(acting));
    Widget heavy = plain;
    heavy.weight = 9;
    CHECK(render_outline(plain) == render_outline(heavy));
    Widget wrapped = plain;
    wrapped.overflow = Overflow::Wrap;
    CHECK(plain != wrapped);
    CHECK(render_outline(plain) == render_outline(wrapped));
}

TEST_CASE("the TUI projects the evolved vocabulary: slots and stress previews land in cells") {
    const Widget card = task_card_tree();
    // Round-trip first — the TUI draws what came back through the gate, not a local shortcut.
    const UiComponent back = gate_round_trip(make_component("task-card", "Task", 1, card));
    const TreeResult rebuilt = tree_of(back);
    REQUIRE_MESSAGE(rebuilt.root.has_value(), rebuilt.error);

    CaptureBackend term;
    tui_draw(*rebuilt.root, 40, 120, term);
    CHECK(term.out.find("[ Task ]") != std::string::npos);                       // the region
    CHECK(term.out.find("[slot actions: accepts Component]") != std::string::npos); // the hole
    CHECK(term.out.find("[slot nav: accepts Route]") != std::string::npos);
    CHECK(term.out.find("-9223372036854775808") != std::string::npos); // the stress preview
    CHECK(term.out.find("Lorem ipsum") != std::string::npos);
}

TEST_CASE("the view/presenter split is separable data: a view carries no source; the presenter binds one") {
    // The component schema declares NO feeding source — pinned at the schema level, so the
    // split cannot silently erode: display data and update-logic are different shapes.
    CHECK(loom::schema_of<UiComponent>()->find("source_role") == nullptr);
    CHECK(loom::schema_of<UiComponent>()->find("presenter") == nullptr);
    CHECK(loom::schema_of<UiPresenter>()->find("nodes") == nullptr); // and no tree in the logic

    // A view is complete without any presenter (renders from placeholders alone) ...
    const UiComponent view = make_component("task-card", "Task", 1, task_card_tree());
    const TreeResult alone = tree_of(view);
    REQUIRE_MESSAGE(alone.root.has_value(), alone.error);
    CHECK_FALSE(render_outline(*alone.root).empty());

    // ... and the presenter is its own gated value, referencing the view by ADDRESS (name),
    // its source by ROLE — never by containment, never by a session-scoped id.
    UiPresenter p;
    p.view = "task-card";
    p.source_role = "task-store";
    const loom::Unverified u = loom::parse(loom::serialize(loom::to_value(p)));
    loom::Admission a = loom::admit(u, loom::schema_of<UiPresenter>());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    const UiPresenter back = loom::from_value<UiPresenter>(std::move(a).value());
    CHECK(back.view == "task-card");
    CHECK(back.source_role == "task-store");
}

TEST_CASE("typed slots are checkable: bindings verify against the contract's actual schema") {
    // Clean: every binding of the task card names a real Task field of a displayable kind.
    const UiComponent card = make_component("task-card", "Task", 1, task_card_tree());
    CHECK(check_bindings(card, *task_schema()).empty());

    SUBCASE("a binding to a field the contract does not declare") {
        Widget w = bound_text("t", "subtitle", loom::Kind::Text); // Task has no 'subtitle'
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("'subtitle' is not a field of Task v1") != std::string::npos);
    }
    SUBCASE("a scalar node bound to a List field") {
        Widget w = bound_text("t", "tags", loom::Kind::Text); // tags is List<Text>
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("displays a scalar") != std::string::npos);
    }
    SUBCASE("a List node bound to a scalar field") {
        Widget w = bound_list("l", "Counts", "count"); // count is Int
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("binds a List field") != std::string::npos);
    }
    SUBCASE("a scalar node bound to a Bytes field (no display form)") {
        Widget w = bound_text("t", "blob", loom::Kind::Text);
        const UiComponent c = make_component("bad", "Task", 1, w);
        CHECK(check_bindings(c, *task_schema()).size() == 1);
    }
    SUBCASE("a container cannot bind data") {
        Widget w = vstack("v", {});
        w.from_field = "title";
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("cannot bind data") != std::string::npos);
    }
    SUBCASE("a slot accepting an unknown type") {
        const Widget w = slot("hole", "Pixels", {});
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("Pixels") != std::string::npos);
    }
    SUBCASE("a List bound to a list of non-displayable elements (rows are text)") {
        Widget w = bound_list("l", "Chunks", "chunks"); // chunks is List<Bytes>
        const UiComponent c = make_component("bad", "Task", 1, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("rows display scalars") != std::string::npos);
    }
    SUBCASE("a route on a node that can never Activate (a dead declaration)") {
        Widget w = text_widget("go somewhere");
        w.route_to = "settings-view"; // but a plain Text is not activatable
        const UiComponent c = make_component("bad", "", 0, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("never fire") != std::string::npos);
    }
    SUBCASE("a nameless slot (cannot be filled)") {
        const Widget w = slot("", "Component", {});
        const UiComponent c = make_component("bad", "", 0, w);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("no name") != std::string::npos);
    }
    SUBCASE("duplicate slot names (filling by name would be ambiguous)") {
        const Widget root =
            vstack("v", {slot("actions", "Component", {}), slot("actions", "Route", {})});
        const UiComponent c = make_component("bad", "", 0, root);
        const std::vector<std::string> problems = check_bindings(c, *task_schema());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].find("duplicate slot name 'actions'") != std::string::npos);
    }
}

TEST_CASE("interaction intent and routes survive the wire (declared, never medium)") {
    Widget rows = list("rows", "Rows", {"a", "b"}, 1, /*activatable=*/true, /*focused=*/true);
    rows.reorderable = true; // declared intent; no built renderer consumes it yet
    rows.route_to = "detail-view";
    const UiComponent back = gate_round_trip(make_component("rows", "", 0, rows));
    const TreeResult r = tree_of(back);
    REQUIRE_MESSAGE(r.root.has_value(), r.error);
    CHECK(r.root->activatable);
    CHECK(r.root->reorderable);
    CHECK(r.root->focused);
    CHECK(r.root->selected_index == 1);
    CHECK(r.root->route_to == "detail-view");
    CHECK(*r.root == rows);
}

} // TEST_SUITE
