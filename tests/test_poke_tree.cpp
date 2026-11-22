// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

// The nested poke read: zen.PokeRead version 2 names a place inside a weave's state by a path,
// and zen.PokeTree answers every readable leaf below it, each hidden field withheld by its path.
// The state here is a run of layouts as an owner of layouts would keep it: lists, shapes within
// shapes, optionals, a union and an enumeration, and a pane's settings held back by ZEN_HIDE.

#include <zen/console/console.hpp>
#include <zen/gate.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include "switchboard_fixtures.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace loom;
namespace au = loom;

namespace {

enum class Fold { open, shut, hidden };
ZEN_ENUM(Fold, 1, open, shut, hidden);

struct Setting {
    std::string key;
    std::string value;
    ZEN_SHAPE(Setting, 1, ZEN_FIELD(key), ZEN_FIELD(value));
};

struct Place {
    std::string mode;
    std::optional<std::int64_t> x;
    std::optional<std::int64_t> y;
    ZEN_SHAPE(Place, 1, ZEN_FIELD(mode), ZEN_FIELD(x), ZEN_FIELD(y));
};

struct Row {
    std::string provider;
    std::string pane;
    Place place;
    Fold fold = Fold::open;
    std::vector<Setting> settings; ///< the pane's own, handed to it alone
    ZEN_SHAPE(Row, 1, ZEN_FIELD(provider), ZEN_FIELD(pane), ZEN_FIELD(place), ZEN_FIELD(fold),
              ZEN_HIDE(settings));
};

struct Desk {
    std::string name;
    std::vector<Row> panes;
    ZEN_SHAPE(Desk, 1, ZEN_FIELD(name), ZEN_FIELD(panes));
};

struct Unlinked {
    ZEN_SHAPE(Unlinked, 1);
};
struct Linked {
    std::string path;
    ZEN_SHAPE(Linked, 1, ZEN_FIELD(path));
};
struct Link {
    std::variant<Unlinked, Linked> is;
    ZEN_UNION(Link, 1, is);
};

struct Layout {
    std::int64_t id = 0;
    Desk desk;
    Link link;
    ZEN_SHAPE(Layout, 1, ZEN_FIELD(id), ZEN_FIELD(desk), ZEN_FIELD(link));
};

struct Run {
    std::vector<Layout> layouts;
    std::int64_t active = 0;
    std::int64_t next_id = 1;
    ZEN_SHAPE(Run, 1, ZEN_FIELD(layouts), ZEN_FIELD(active), ZEN_FIELD(next_id));
};

struct OwnerState {
    std::int64_t edits = 0;
    Run run;
    std::string token; ///< a value no reader may scrape
    ZEN_SHAPE(OwnerState, 1, ZEN_FIELD(edits), ZEN_FIELD(run), ZEN_HIDE(token));
};

OwnerState two_layouts() {
    OwnerState s;
    s.edits = 3;
    s.token = "s3cret";
    Layout a;
    a.id = 1;
    a.desk.name = "write";
    a.desk.panes.push_back(Row{"zengine.editor", "editor", Place{"at", 4, 2}, Fold::open,
                               {Setting{"theme", "night"}}});
    a.desk.panes.push_back(Row{"zengine.terminal", "terminal", Place{"default", {}, {}},
                               Fold::shut, {Setting{"api-key", "s3cret"}}});
    a.link.is = Linked{"/home/w/write.setup"};
    Layout b;
    b.id = 2;
    b.desk.name = "read";
    s.run.layouts = {a, b};
    s.run.active = 0;
    s.run.next_id = 3;
    return s;
}

const PokeEntry* entry(const PokeTree& t, const std::string& path) {
    for (const PokeEntry& e : t.entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

std::string refusal(const std::variant<PokeTree, Refused>& r) {
    REQUIRE(std::holds_alternative<Refused>(r));
    return std::get<Refused>(r).reason;
}

PokeTree tree(const std::variant<PokeTree, Refused>& r) {
    if (std::holds_alternative<Refused>(r)) {
        FAIL("refused: " << std::get<Refused>(r).reason);
    }
    return std::get<PokeTree>(r);
}

class OwnerWeave
    : public au::WeaveBase<OwnerWeave, OwnerState, au::Accept<>, au::Emit<>> {
public:
    void seed(OwnerState s) { state_ = std::move(s); }
};

struct TreeLog {
    sbfx::Registered reg{};
    std::vector<loom::Value> got;
};
TreeLog register_tree_probe(Switchboard& bus) {
    TreeLog log;
    log.reg = sbfx::register_probe(
        bus, {au::schema_of<au::PokeTree>(), au::schema_of<au::Refused>(),
              au::schema_of<au::Result>()});
    log.reg.weave->on_handle = [&log](const Message& in, Bus&, sbfx::ProbeWeave&) {
        log.got.push_back(in.payload);
    };
    return log;
}

struct Nudge {
    ZEN_SHAPE(Nudge, 1);
};
struct AskerState {
    std::int64_t target = 0;
    ZEN_SHAPE(AskerState, 1, ZEN_FIELD(target));
};
/// An ordinary participant that asks a tree read when nudged, under whatever grant it is given.
class TreeAsker : public au::WeaveBase<TreeAsker, AskerState, au::Accept<Nudge, au::PokeTree>,
                                       au::Emit<au::PokeReadPath>> {
public:
    WeaveId target{};
    std::vector<PokeTree> answers;
    void on(const Nudge&, au::Mail& mail) {
        mail.send(target, au::PokeReadPath{{"run", "layouts", "1", "desk", "name"}});
    }
    void on(const au::PokeTree& t, au::Mail&) { answers.push_back(t); }
};

} // namespace

TEST_SUITE("poke_tree") {

TEST_CASE("a path reads one leaf inside the state: a field, a list's element, a shape's field") {
    const OwnerState s = two_layouts();
    const PokeTree t = tree(au::poke_read_tree(s, {"run", "layouts", "1", "desk", "name"}));
    CHECK(t.path == "run.layouts[1].desk.name");
    CHECK(t.type == "Text");
    REQUIRE(t.entries.size() == 1);
    CHECK(t.entries[0].path == "run.layouts[1].desk.name");
    CHECK(t.entries[0].value == "read");
    CHECK(t.withheld.empty());

    const PokeTree x = tree(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes", "0",
                                                    "place", "x"}));
    REQUIRE(x.entries.size() == 1);
    CHECK(x.entries[0].type == "Int");
    CHECK(x.entries[0].value == "4");
}

TEST_CASE("a place that holds more is answered as every readable leaf below it, each hidden "
          "field withheld by its path and nothing of its value said") {
    const OwnerState s = two_layouts();
    const PokeTree t = tree(au::poke_read_tree(s, {"run", "layouts", "0", "desk"}));
    CHECK(t.type == "Desk");
    // A list says its length, then its elements; an absent optional is an entry not present.
    const PokeEntry* panes = entry(t, "run.layouts[0].desk.panes");
    REQUIRE(panes != nullptr);
    CHECK(panes->type == "List<Row>");
    CHECK(panes->value == "2");
    const PokeEntry* y = entry(t, "run.layouts[0].desk.panes[1].place.y");
    REQUIRE(y != nullptr);
    CHECK_FALSE(y->present);
    CHECK(y->value.empty());
    // An enumeration reads as its spelling.
    const PokeEntry* fold = entry(t, "run.layouts[0].desk.panes[1].fold");
    REQUIRE(fold != nullptr);
    CHECK(fold->type == "Fold");
    CHECK(fold->value == "shut");
    // Each row's settings are withheld, and no entry says them.
    CHECK(t.withheld == std::vector<std::string>{"run.layouts[0].desk.panes[0].settings",
                                                 "run.layouts[0].desk.panes[1].settings"});
    for (const PokeEntry& e : t.entries) {
        CHECK(e.path.find("settings") == std::string::npos);
        CHECK(e.value.find("s3cret") == std::string::npos);
        CHECK(e.value.find("night") == std::string::npos);
    }
}

TEST_CASE("the whole state reads from the empty path, its own hidden field withheld") {
    const OwnerState s = two_layouts();
    const PokeTree t = tree(au::poke_read_tree(s, {}));
    CHECK(t.path.empty());
    CHECK(t.type == "OwnerState");
    CHECK(entry(t, "edits")->value == "3");
    CHECK(entry(t, "run.next_id")->value == "3");
    CHECK(entry(t, "token") == nullptr);
    CHECK(t.withheld.front() == "run.layouts[0].desk.panes[0].settings");
    CHECK(t.withheld.back() == "token");
    // A union says the case it holds, then what that case carries under its name.
    const PokeEntry* link = entry(t, "run.layouts[0].link");
    REQUIRE(link != nullptr);
    CHECK(link->type == "Link");
    CHECK(link->value == "Linked");
    CHECK(entry(t, "run.layouts[0].link.Linked.path")->value == "/home/w/write.setup");
    CHECK(entry(t, "run.layouts[1].link")->value == "Unlinked");
}

TEST_CASE("a union is stepped into by the case it holds, and refused by another") {
    const OwnerState s = two_layouts();
    const PokeTree t =
        tree(au::poke_read_tree(s, {"run", "layouts", "0", "link", "Linked", "path"}));
    CHECK(t.entries.at(0).value == "/home/w/write.setup");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "1", "link", "Linked"})) ==
          "'run.layouts[1].link' holds another case than 'Linked'");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "1", "link", "Elsewhere"})) ==
          "'run.layouts[1].link' has no case 'Elsewhere'");
}

TEST_CASE("a path that names what may not be read, or nothing, is refused, naming the step") {
    const OwnerState s = two_layouts();
    CHECK(refusal(au::poke_read_tree(s, {"token"})) ==
          "field 'token' is hidden (ZEN_HIDE): its value is message-only — ask the weave "
          "through its own interface");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes", "1",
                                         "settings", "0", "value"})) ==
          "field 'run.layouts[0].desk.panes[1].settings' is hidden (ZEN_HIDE): its value is "
          "message-only — ask the weave through its own interface");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "2"})) ==
          "'run.layouts' holds 2 elements: index 2 is past its end");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "first"})) ==
          "'run.layouts' is a list: step 'first' is not an index");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "-1"})) ==
          "'run.layouts' is a list: step '-1' is not an index");
    CHECK(refusal(au::poke_read_tree(s, {"run", "active", "x"})) ==
          "'run.active' is Int: nothing is below it");
    CHECK(refusal(au::poke_read_tree(s, {"run", "shelf"})) ==
          "no field 'shelf' in 'run' — zen.PokeDescribe lists the structure");
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes", "1", "place",
                                         "y"})) ==
          "'run.layouts[0].desk.panes[1].place.y' is optional and absent");
    std::vector<std::string> deep(au::kMaxPokePathSteps + 1, "run");
    CHECK(refusal(au::poke_read_tree(s, deep)) ==
          "a path of 65 steps: the most is 64 (kMaxPokePathSteps)");
}

TEST_CASE("a tree read is bounded: past kMaxPokeTreeEntries leaves it is refused, naming the "
          "bound, and at the bound it is answered") {
    OwnerState s;
    // Each layout answers four entries (its id, its desk's name, its desk's empty list of panes
    // and its link's case) and withholds nothing, so the entries alone meet the bound.
    const std::size_t layouts = (au::kMaxPokeTreeEntries - 1) / 4;
    s.run.layouts.resize(layouts);
    const PokeTree t = tree(au::poke_read_tree(s, {"run", "layouts"}));
    CHECK(t.entries.size() == 1 + layouts * 4);
    CHECK(t.entries.size() <= au::kMaxPokeTreeEntries);
    CHECK(t.withheld.empty());

    s.run.layouts.resize(layouts + 1);
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts"})) ==
          "the state at 'run.layouts' holds more than 8192 leaves (kMaxPokeTreeEntries): read a "
          "narrower path");
}

TEST_CASE("on the bus a tree read is answered by message, and its largest answer crosses the "
          "gate") {
    Switchboard bus;
    WeaveId owner = au::mount<OwnerWeave>(bus);
    static_cast<OwnerWeave*>(bus.weave(owner))->seed(two_layouts());
    TreeLog asker = register_tree_probe(bus);

    bus.send(owner, Message(au::to_value(au::PokeReadPath{{"run", "layouts", "0", "desk"}}),
                            WeaveId{}, asker.reg.id, 7));
    bus.send(owner, Message(au::to_value(au::PokeReadPath{{"token"}}), WeaveId{},
                            asker.reg.id, 8));
    // Version 1 is unchanged: one scalar field by name, an aggregate refused by its kind.
    bus.send(owner, Message(au::to_value(au::PokeRead{"run"}), WeaveId{}, asker.reg.id, 9));
    bus.drain_until_idle();

    REQUIRE(asker.got.size() == 3);
    CHECK(asker.got[0].schema().name() == "zen.PokeTree");
    const PokeTree t = au::from_value<au::PokeTree>(asker.got[0]);
    CHECK(entry(t, "run.layouts[0].desk.name")->value == "write");
    CHECK(t.withheld.size() == 2);
    CHECK(asker.got[1].schema().name() == "zen.Refused");
    CHECK(asker.got[2].schema().name() == "zen.Refused");
    CHECK(asker.got[2].get("reason")->as_text() ==
          "field 'run' has kind Run — only scalar fields are message-readable");

    // The largest tree a read answers is admitted by the gate on its way: no TooLarge.
    OwnerState big;
    Layout l;
    l.desk.panes.resize((au::kMaxPokeTreeEntries - 1) / 7);
    for (Row& r : l.desk.panes) {
        r.provider = std::string(64, 'p');
    }
    big.run.layouts = {l};
    static_cast<OwnerWeave*>(bus.weave(owner))->seed(big);
    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    bus.send(owner, Message(au::to_value(au::PokeReadPath{{"run", "layouts", "0", "desk", "panes"}}),
                            WeaveId{}, asker.reg.id, 10));
    bus.drain_until_idle();
    REQUIRE(asker.got.size() == 4);
    CHECK(asker.got[3].schema().name() == "zen.PokeTree");
    for (const auto& r : tap) {
        CHECK(r.kind != EventKind::Refused);
    }
}

TEST_CASE("a grant scopes a tree read by its target and its shape, as any send: an asker not "
          "granted the read is refused at the send, and a target not granted the answer stays "
          "silent") {
    Switchboard bus;
    WeaveId owner = au::mount<OwnerWeave>(bus);
    static_cast<OwnerWeave*>(bus.weave(owner))->seed(two_layouts());

    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });

    // Granted version 1 only: the version-2 read is not this asker's to send.
    Grant v1_only;
    v1_only.allow_to_any("zen.PokeRead", 1);
    WeaveId narrow = au::mount_granted<TreeAsker>(bus, v1_only);
    auto* n = static_cast<TreeAsker*>(bus.weave(narrow));
    n->target = owner;
    bus.send(narrow, Message(au::to_value(Nudge{}), WeaveId{}, WeaveId{}));
    bus.drain_until_idle();
    CHECK(n->answers.empty());
    bool read_denied = false;
    for (const auto& r : tap) {
        read_denied = read_denied || (r.schema == "zen.PokeRead" && r.kind == EventKind::Refused &&
                                      r.reason == RefusalReason::CapabilityDenied);
    }
    CHECK(read_denied);

    // Granted the read: answered, by the target's own grant (mount adds the poke answers).
    Grant v2_read;
    v2_read.allow_to_any("zen.PokeRead", 2);
    WeaveId wide = au::mount_granted<TreeAsker>(bus, v2_read);
    auto* w = static_cast<TreeAsker*>(bus.weave(wide));
    w->target = owner;
    bus.send(wide, Message(au::to_value(Nudge{}), WeaveId{}, WeaveId{}));
    bus.drain_until_idle();
    REQUIRE(w->answers.size() == 1);
    CHECK(w->answers[0].entries.at(0).value == "read");

    // A target granted nothing answers nothing: the tree is CapabilityDenied at delivery.
    WeaveId silent = au::mount_granted<OwnerWeave>(bus, Grant::nothing());
    static_cast<OwnerWeave*>(bus.weave(silent))->seed(two_layouts());
    w->target = silent;
    tap.clear();
    bus.send(wide, Message(au::to_value(Nudge{}), WeaveId{}, WeaveId{}));
    bus.drain_until_idle();
    CHECK(w->answers.size() == 1);
    bool tree_denied = false;
    for (const auto& r : tap) {
        tree_denied = tree_denied || (r.schema == "zen.PokeTree" && r.kind == EventKind::Refused &&
                                      r.reason == RefusalReason::CapabilityDenied);
    }
    CHECK(tree_denied);
}

TEST_CASE("every woven weave declares the read at both versions and its answer") {
    Switchboard bus;
    WeaveId owner = au::mount<OwnerWeave>(bus);
    bool v1 = false, v2 = false;
    for (const auto& s : bus.weave(owner)->accepted_schemas()) {
        v1 = v1 || (s->name() == "zen.PokeRead" && s->version() == 1);
        v2 = v2 || (s->name() == "zen.PokeRead" && s->version() == 2);
    }
    CHECK(v1);
    CHECK(v2);
    bool tree = false;
    for (const auto& s : au::poke_answer_schemas()) {
        tree = tree || s->name() == "zen.PokeTree";
    }
    CHECK(tree);
}

TEST_CASE("withheld paths count against the leaf bound with the entries, so a state of hidden "
          "fields cannot pass it") {
    OwnerState s;
    Layout l;
    // A row answers six entries and withholds its settings: seven against the bound.
    const std::size_t rows = (au::kMaxPokeTreeEntries - 1) / 7;
    l.desk.panes.resize(rows);
    s.run.layouts = {l};
    const PokeTree t = tree(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes"}));
    CHECK(t.withheld.size() == rows);
    CHECK(t.entries.size() + t.withheld.size() == 1 + rows * 7);

    s.run.layouts[0].desk.panes.resize(rows + 1);
    CHECK(refusal(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes"})) ==
          "the state at 'run.layouts[0].desk.panes' holds more than 8192 leaves "
          "(kMaxPokeTreeEntries): read a narrower path");
}

TEST_CASE("the largest tree a read answers weighs no more than the gate's decode budget") {
    OwnerState s;
    Layout l;
    // As many rows as the bound allows, entries and withheld paths together.
    l.desk.panes.resize((au::kMaxPokeTreeEntries - 1) / 7);
    s.run.layouts = {l};
    const PokeTree t = tree(au::poke_read_tree(s, {"run", "layouts", "0", "desk", "panes"}));
    const std::uint64_t cells = au::weigh(au::to_value(t)).cells;
    // The tree's four fields, five cells an entry and one a withheld path: what the bound's
    // compile-time check in poke.hpp counts, measured here so the check cannot drift from it.
    CHECK(cells == 4 + 5 * t.entries.size() + t.withheld.size());
    CHECK(cells <= au::kMaxDecodedCells);
}

TEST_CASE("a spelling names the steps a path takes, and one that names no steps cleanly is "
          "refused, saying where") {
    using Steps = std::vector<std::string>;
    const auto steps = [](std::string_view spelling) {
        auto r = au::poke_path_steps(spelling);
        if (std::holds_alternative<Refused>(r)) {
            FAIL("refused: " << std::get<Refused>(r).reason);
        }
        return std::get<Steps>(r);
    };
    const auto refused = [](std::string_view spelling) {
        auto r = au::poke_path_steps(spelling);
        REQUIRE(std::holds_alternative<Refused>(r));
        return std::get<Refused>(r).reason;
    };
    CHECK(steps("").empty());
    CHECK(steps("token") == Steps{"token"});
    CHECK(steps("run.layouts[0].desk.panes[12].place.x") ==
          Steps{"run", "layouts", "0", "desk", "panes", "12", "place", "x"});
    CHECK(steps("[3]") == Steps{"3"});
    CHECK(steps("run.layouts[0].link.Linked.path") ==
          Steps{"run", "layouts", "0", "link", "Linked", "path"});
    // The spelling and poke_path_text are each other's inverse.
    const Steps path{"run", "layouts", "1", "desk", "panes", "0", "fold"};
    CHECK(steps(au::poke_path_text(path)) == path);

    CHECK(refused(".run") == "the path '.run' begins with a dot");
    CHECK(refused("run..layouts") == "the path 'run..layouts' has an empty step at 4");
    CHECK(refused("run.") == "the path 'run.' has an empty step at 4");
    CHECK(refused("run.layouts[0") ==
          "the path 'run.layouts[0' opens a bracket at 11 and never closes it");
    CHECK(refused("run.layouts[first]") ==
          "the path 'run.layouts[first]' holds 'first' in brackets at 11, which is not an index");
    CHECK(refused("run.layouts[]") ==
          "the path 'run.layouts[]' holds '' in brackets at 11, which is not an index");
    CHECK(refused("run.layouts[0]desk") ==
          "the path 'run.layouts[0]desk' has a step at 14 that follows no dot or bracket");
    CHECK(refused("run]") == "the path 'run]' closes a bracket at 3 it never opened");
    std::string deep;
    for (std::size_t i = 0; i <= au::kMaxPokePathSteps; ++i) {
        deep += "[0]";
    }
    CHECK(refused(deep) == "the path '" + deep + "' takes more than 64 steps (kMaxPokePathSteps)");
}

TEST_CASE("the Poke weave relays a tree read: the spelling forwarded as its steps, the tree "
          "back to the asker under the asker's own correlation") {
    Switchboard bus;
    WeaveId owner = au::mount<OwnerWeave>(bus);
    static_cast<OwnerWeave*>(bus.weave(owner))->seed(two_layouts());
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    TreeLog asker = register_tree_probe(bus);
    std::vector<std::uint64_t> corr;
    asker.reg.weave->on_handle = [&](const Message& in, Bus&, sbfx::ProbeWeave&) {
        asker.got.push_back(in.payload);
        corr.push_back(in.correlation);
    };
    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    const auto tid = static_cast<std::int64_t>(owner.value);

    bus.send(poke, Message(au::to_value(au::PokeGetTree{tid, "run.layouts[0].desk.panes[0]"}),
                           WeaveId{}, asker.reg.id, 21));
    bus.send(poke, Message(au::to_value(au::PokeGetTree{
                               tid, "run.layouts[0].desk.panes[1].settings"}),
                           WeaveId{}, asker.reg.id, 22));
    bus.send(poke, Message(au::to_value(au::PokeGetTree{tid, "run.layouts[x]"}), WeaveId{},
                           asker.reg.id, 23));
    bus.drain_until_idle();

    // The Poke weave refuses the third at once, before the target has answered the first two,
    // so each answer is found by the correlation its asker put on it.
    REQUIRE(asker.got.size() == 3);
    const auto answer_to = [&](std::uint64_t c) -> const loom::Value& {
        for (std::size_t i = 0; i < corr.size(); ++i) {
            if (corr[i] == c) {
                return asker.got[i];
            }
        }
        FAIL("no answer under correlation " << c);
        return asker.got[0];
    };
    REQUIRE(answer_to(21).schema().name() == "zen.PokeTree");
    const PokeTree t = au::from_value<au::PokeTree>(answer_to(21));
    CHECK(t.path == "run.layouts[0].desk.panes[0]");
    CHECK(t.type == "Row");
    CHECK(entry(t, "run.layouts[0].desk.panes[0].place.x")->value == "4");
    CHECK(t.withheld == std::vector<std::string>{"run.layouts[0].desk.panes[0].settings"});
    // The target refuses a hidden path, and its refusal relays like an answer.
    CHECK(answer_to(22).schema().name() == "zen.Refused");
    CHECK(answer_to(22).get("reason")->as_text() ==
          "field 'run.layouts[0].desk.panes[1].settings' is hidden (ZEN_HIDE): its value is "
          "message-only — ask the weave through its own interface");
    // A spelling that names no steps is refused by the Poke weave, and nothing reaches the
    // target: the owner was sent only the two forwards before it.
    CHECK(answer_to(23).schema().name() == "zen.Refused");
    CHECK(answer_to(23).get("reason")->as_text() ==
          "the path 'run.layouts[x]' holds 'x' in brackets at 11, which is not an index");
    int reads_at_owner = 0;
    for (const auto& r : tap) {
        if (r.schema == "zen.PokeRead" && r.target == owner && r.kind == EventKind::Delivered) {
            ++reads_at_owner;
        }
    }
    CHECK(reads_at_owner == 2);
    CHECK(bus.weave(poke)->snapshot().get("pending")->as_list().empty());
}

TEST_CASE("the console reads a place by its spelling, through the Poke weave") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    WeaveId owner = au::mount<OwnerWeave>(bus);
    static_cast<OwnerWeave*>(bus.weave(owner))->seed(two_layouts());
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    const auto tid = static_cast<std::int64_t>(owner.value);

    Composed c = engine.compose(
        poke, "zen.PokeGetTree", 1,
        {Arg{std::nullopt, FieldValue{tid}},
         Arg{std::nullopt, FieldValue{std::string("run.layouts[1].desk.name")}}});
    REQUIRE(c.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);
    CHECK(engine.buffer_at(1)->name == "zen.PokeTree");
    const PokeTree t = au::from_value<au::PokeTree>(engine.buffer_at(1)->value);
    REQUIRE(t.entries.size() == 1);
    CHECK(t.entries[0].path == "run.layouts[1].desk.name");
    CHECK(t.entries[0].value == "read");
}

} // TEST_SUITE
