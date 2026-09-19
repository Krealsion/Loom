// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "fixtures.hpp"

#include <zen/schema.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loom;

TEST_SUITE("schema") {

TEST_CASE("a built schema reports its name, version, and fields") {
    auto s = SchemaBuilder("Move", 3).field("dx", Kind::Float).field("dy", Kind::Float).build();
    CHECK(s->name() == "Move");
    CHECK(s->version() == 3);
    REQUIRE(s->fields().size() == 2);
    CHECK(s->fields()[0].name == "dx");
    CHECK(s->fields()[1].name == "dy");
    CHECK(s->find("dx") != nullptr);
    CHECK(s->find("dy") != nullptr);
    CHECK(s->find("dz") == nullptr);
}

TEST_CASE("content identity is stable across separately-built identical schemas") {
    auto a = SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    auto b = SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    CHECK(a->content_id() == b->content_id());
    CHECK(same_identity(*a, *b));
}

TEST_CASE("same_identity is true identity (name, version, content_id), not bare content_id") {
    auto base = SchemaBuilder("Foo", 1).field("a", Kind::Int).build();
    auto twin = SchemaBuilder("Foo", 1).field("a", Kind::Int).build(); // separately built
    auto diff_name = SchemaBuilder("Bar", 1).field("a", Kind::Int).build();
    auto diff_version = SchemaBuilder("Foo", 2).field("a", Kind::Int).build();

    CHECK(same_identity(*base, *twin)); // structurally identical => same identity
    // The (name, version) discrimination is the testable, meaningful part (a forced
    // FNV collision is not constructible): same_identity must reject a name or
    // version difference, where bare content_id equality would not be asked to.
    CHECK_FALSE(same_identity(*base, *diff_name));
    CHECK_FALSE(same_identity(*base, *diff_version));
}

TEST_CASE("content identity changes with name, version, field name, kind, order, or requiredness") {
    auto base = SchemaBuilder("S", 1).field("a", Kind::Int).field("b", Kind::Text).build();

    auto diff_name = SchemaBuilder("T", 1).field("a", Kind::Int).field("b", Kind::Text).build();
    auto diff_ver = SchemaBuilder("S", 2).field("a", Kind::Int).field("b", Kind::Text).build();
    auto diff_fname = SchemaBuilder("S", 1).field("a", Kind::Int).field("c", Kind::Text).build();
    auto diff_kind = SchemaBuilder("S", 1).field("a", Kind::Float).field("b", Kind::Text).build();
    auto diff_order = SchemaBuilder("S", 1).field("b", Kind::Text).field("a", Kind::Int).build();
    auto diff_req =
        SchemaBuilder("S", 1).field("a", Kind::Int).field("b", Kind::Text, false).build();

    CHECK(base->content_id() != diff_name->content_id());
    CHECK(base->content_id() != diff_ver->content_id());
    CHECK(base->content_id() != diff_fname->content_id());
    CHECK(base->content_id() != diff_kind->content_id());
    CHECK(base->content_id() != diff_order->content_id());
    CHECK(base->content_id() != diff_req->content_id());
}

TEST_CASE("nested-message structure participates in identity") {
    auto inner1 = SchemaBuilder("Inner", 1).field("x", Kind::Int).build();
    auto inner2 = SchemaBuilder("Inner", 1).field("x", Kind::Text).build(); // different shape

    auto outer1 = SchemaBuilder("Outer", 1).message("in", inner1).build();
    auto outer2 = SchemaBuilder("Outer", 1).message("in", inner2).build();
    CHECK(outer1->content_id() != outer2->content_id());
}

TEST_CASE("malformed type references are rejected at construction") {
    // A Message field with no schema.
    CHECK_THROWS_AS((void)Schema("Bad", 1, {Field{"m", TypeRef{Kind::Message, nullptr, nullptr}, true}}),
                    std::invalid_argument);
    // A List field with no element.
    CHECK_THROWS_AS((void)Schema("Bad", 1, {Field{"l", TypeRef{Kind::List, nullptr, nullptr}, true}}),
                    std::invalid_argument);
    // type_of refuses non-primitive kinds.
    CHECK_THROWS_AS((void)type_of(Kind::Message), std::invalid_argument);
    CHECK_THROWS_AS((void)type_of(Kind::List), std::invalid_argument);
    // type_message refuses a null schema.
    CHECK_THROWS_AS((void)type_message(nullptr), std::invalid_argument);
}

TEST_CASE("duplicate and empty field names are rejected") {
    CHECK_THROWS_AS((void)SchemaBuilder("Dup", 1).field("a", Kind::Int).field("a", Kind::Text).build(),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)SchemaBuilder("Empty", 1).field("", Kind::Int).build(),
                    std::invalid_argument);
}

TEST_CASE("list element types nest and contribute to identity") {
    auto list_of_int = SchemaBuilder("L", 1).list("xs", type_of(Kind::Int)).build();
    auto list_of_text = SchemaBuilder("L", 1).list("xs", type_of(Kind::Text)).build();
    auto list_of_list = SchemaBuilder("L", 1).list("xs", type_list(type_of(Kind::Int))).build();
    CHECK(list_of_int->content_id() != list_of_text->content_id());
    CHECK(list_of_int->content_id() != list_of_list->content_id());
}

// ---- the component closure (collect_referenced) ---------------------------------
//
// The one traversal every declaration shares: a weave's registration, a library's
// manifest, a described accept-set. It lives here, with the schemas, and its dedup
// rule is the whole point — by identity, never by name alone.
// docs/decisions/declared-vocabulary-is-agreed-at-admission.md

TEST_CASE("collect_referenced walks the closure in post-order, carrying a shared component "
          "once, whichever root reaches it and however deep") {
    auto leaf = SchemaBuilder("Leaf", 1).field("v", Kind::Int).build();
    auto mid = SchemaBuilder("Mid", 1).message("leaf", leaf).build();
    auto outer = SchemaBuilder("Outer", 1).message("mid", mid).build();
    auto listy = SchemaBuilder("Listy", 1)
                     .list("rows", type_list(type_message(mid))) // List<List<Mid>>
                     .build();

    std::vector<std::shared_ptr<const Schema>> out;
    collect_referenced(*outer, out);
    collect_referenced(*listy, out);
    REQUIRE(out.size() == 2); // Leaf, Mid — once each, roots not included
    CHECK(out[0]->name() == "Leaf");
    CHECK(out[1]->name() == "Mid");

    // A flat schema references nothing.
    std::vector<std::shared_ptr<const Schema>> none;
    collect_referenced(*leaf, none);
    CHECK(none.empty());
}

TEST_CASE("collect_referenced deduplicates by IDENTITY: an equal definition from a second "
          "object is carried once, a different definition under a kept name is carried too") {
    auto part_a = SchemaBuilder("Part", 1).field("a", Kind::Int).build();
    auto part_a_twin = SchemaBuilder("Part", 1).field("a", Kind::Int).build(); // equal content
    auto part_b = SchemaBuilder("Part", 1).field("a", Kind::Int).field("b", Kind::Bool).build();
    auto box = SchemaBuilder("Box", 1).message("part", part_a).build();
    auto crate = SchemaBuilder("Crate", 1).message("part", part_a_twin).build();
    auto box2 = SchemaBuilder("Box2", 1).message("part", part_b).build();

    SUBCASE("two objects, one identity: one entry") {
        std::vector<std::shared_ptr<const Schema>> out;
        collect_referenced(*box, out);
        collect_referenced(*crate, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].get() == part_a.get()); // the first object seen stands for both
    }
    SUBCASE("two definitions under one (name, version): BOTH survive, in order") {
        // This is the entry the old name-keyed walk dropped, which let a weave load
        // advertising a Box2 whose Part was silently the first one's. Whoever reads
        // the result — a Registry claim, a manifest's loader — now sees the
        // contradiction and refuses it.
        std::vector<std::shared_ptr<const Schema>> out;
        collect_referenced(*box, out);
        collect_referenced(*box2, out);
        REQUIRE(out.size() == 2);
        CHECK(out[0]->content_id() == part_a->content_id());
        CHECK(out[1]->content_id() == part_b->content_id());
        CHECK(out[0]->name() == out[1]->name());
        CHECK(out[0]->version() == out[1]->version());
    }
}

TEST_CASE("collect_referenced expands a shared component once, however many paths reach it: a "
          "deep graph of two-field sharing is walked in one pass, not as a tree") {
    // THE SHAPE THAT STALLED A HOST. Node[i] holds two message fields, `left` and `right`,
    // both of Node[i-1]; the closure is a chain of depth+1 distinct schemas, but a walk that
    // descends BEFORE checking what it has already carried expands it as a binary tree —
    // 2^depth visits. At depth 26 that was a registration that never returned (an
    // eight-second timeout, independently measured); at this depth it is astronomically
    // more, so a wrong walk cannot finish here and a right one finishes at once.
    constexpr int depth = 200;
    auto leaf = SchemaBuilder("Shared.Leaf", 1).field("v", Kind::Int).build();
    std::shared_ptr<const Schema> node = leaf;
    for (int i = 1; i <= depth; ++i) {
        node = SchemaBuilder("Shared.Node" + std::to_string(i), 1)
                   .message("left", node, /*required=*/false)
                   .message("right", node, /*required=*/false)
                   .build();
    }
    const std::shared_ptr<const Schema> root = node;

    // THE BOUNDED-COMPLETION GUARD, generous by orders of magnitude: the corrected walk
    // takes microseconds; the old one would take longer than the machine will exist. A
    // walk that has not returned in 30 seconds is not slow, it is the exponential — and
    // its thread is left to the process's exit rather than joined, since it never would.
    std::promise<std::vector<std::shared_ptr<const Schema>>> done;
    std::future<std::vector<std::shared_ptr<const Schema>>> result = done.get_future();
    std::thread walker([root, done = std::move(done)]() mutable {
        std::vector<std::shared_ptr<const Schema>> out;
        collect_referenced(*root, out);
        done.set_value(std::move(out));
    });
    if (result.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        walker.detach();
        FAIL("collect_referenced did not finish a 201-schema shared graph within 30 s: the "
             "walk is expanding an already-carried component again (2^depth work)");
        return;
    }
    walker.join();
    const std::vector<std::shared_ptr<const Schema>> out = result.get();

    // THE RIGHT CLOSURE: every distinct identity exactly once, post-order (the leaf first,
    // each node after the node it nests), the root itself not included.
    REQUIRE(out.size() == static_cast<std::size_t>(depth)); // Leaf + Node1..Node(depth-1)
    CHECK(out.front()->name() == "Shared.Leaf");
    for (int i = 1; i < depth; ++i) {
        CHECK(out[static_cast<std::size_t>(i)]->name() == "Shared.Node" + std::to_string(i));
    }
    // Repeated calls keep using `out` as the visited set: a second root over the same
    // graph adds only what is new, and a root reached twice is carried once.
    std::vector<std::shared_ptr<const Schema>> again = out;
    collect_referenced(*root, again);
    CHECK(again.size() == out.size());
    auto sibling = SchemaBuilder("Shared.Sibling", 1).message("a", root).message("b", root).build();
    collect_referenced(*sibling, again);
    CHECK(again.size() == out.size() + 1); // exactly the root joined
    CHECK(again.back().get() == root.get());
    // ...and identity, not name, is still the rule: a different Leaf under the same key
    // survives beside the carried one for the registry to refuse.
    auto other_leaf = SchemaBuilder("Shared.Leaf", 1).field("w", Kind::Text).build();
    auto carrier = SchemaBuilder("Shared.Carrier", 1).message("l", other_leaf).build();
    collect_referenced(*carrier, again);
    CHECK(again.size() == out.size() + 2);
    CHECK(again.back()->content_id() == other_leaf->content_id());
}

} // TEST_SUITE
