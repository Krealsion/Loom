// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/registry.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loom;

TEST_SUITE("registry") {

TEST_CASE("register then look up by (name, version)") {
    Registry reg;
    auto reps = reg.register_schema(fx::PlayerState());
    CHECK(reps.inserted);
    CHECK(reg.contains("PlayerState", 1));
    CHECK(reg.size() == 1);

    auto found = reg.lookup("PlayerState", 1);
    REQUIRE(found != nullptr);
    CHECK(found->content_id() == fx::PlayerState()->content_id());
    CHECK(reg.lookup("PlayerState", 2) == nullptr);
    CHECK(reg.lookup("Nope", 1) == nullptr);
}

TEST_CASE("a schema discovered at runtime is registerable (the DLL case)") {
    Registry reg;
    // Imagine this arrived from a freshly loaded module.
    auto discovered = SchemaBuilder("JustArrived", 7).field("x", Kind::Int).build();
    CHECK(reg.lookup("JustArrived", 7) == nullptr);
    reg.register_schema(discovered);
    REQUIRE(reg.lookup("JustArrived", 7) != nullptr);
}

TEST_CASE("re-registering identical content is an idempotent no-op") {
    Registry reg;
    auto first = reg.register_schema(fx::PlayerState());
    // A separately-built but structurally identical schema.
    auto twin = SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    auto second = reg.register_schema(twin);
    CHECK_FALSE(second.inserted);
    CHECK(reg.size() == 1);
    // The canonical owner is the originally-registered one.
    CHECK(second.schema.get() == first.schema.get());
}

TEST_CASE("re-registering the same key with different content is a conflict") {
    Registry reg;
    reg.register_schema(fx::PlayerState());
    auto impostor = SchemaBuilder("PlayerState", 1).field("hp", Kind::Float).build();
    CHECK_THROWS_AS(reg.register_schema(impostor), SchemaConflict);
    // The published schema is unchanged.
    CHECK(reg.lookup("PlayerState", 1)->content_id() == fx::PlayerState()->content_id());
}

TEST_CASE("a new version coexists with the old") {
    Registry reg;
    reg.register_schema(SchemaBuilder("S", 1).field("a", Kind::Int).build());
    reg.register_schema(SchemaBuilder("S", 2).field("a", Kind::Int).field("b", Kind::Text).build());
    CHECK(reg.size() == 2);
    CHECK(reg.lookup("S", 1) != nullptr);
    CHECK(reg.lookup("S", 2) != nullptr);
}

TEST_CASE("null schema registration is rejected") {
    Registry reg;
    CHECK_THROWS_AS(reg.register_schema(nullptr), std::invalid_argument);
}

TEST_CASE("reads are safe under concurrent registration") {
    Registry reg;
    reg.register_schema(fx::PlayerState()); // a stable schema readers will look up

    std::atomic<bool> go{false};
    std::atomic<int> read_failures{0};
    std::atomic<int> reads{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < 20000; ++i) {
                if (reg.lookup("PlayerState", 1) == nullptr) {
                    read_failures.fetch_add(1);
                }
                reads.fetch_add(1);
            }
        });
    }

    std::thread writer([&] {
        while (!go.load()) {
        }
        for (std::uint32_t v = 100; v < 100 + 2000; ++v) {
            reg.register_schema(SchemaBuilder("Churn", v).field("x", Kind::Int).build());
        }
    });

    go.store(true);
    for (auto& r : readers) {
        r.join();
    }
    writer.join();

    CHECK(read_failures.load() == 0);
    CHECK(reads.load() == 4 * 20000);
    CHECK(reg.contains("Churn", 2099));
}

// ---- BL-0: a schema is discoverable while a live claim requires it ----------
//
// The append-only registry was the demonstrated C-10/F-9 shape: a host meeting a
// stream of distinct schemas retained every one of them forever, because
// registration had no counterpart. What follows pins the replacement — not
// "schemas can be deleted", but "a schema is retained by a live NEED and by
// nothing else". Every case below is written against externally meaningful
// behaviour (does it resolve?) rather than against a claim count, because the
// count is bookkeeping and resolvability is the promise.

namespace {
std::shared_ptr<const Schema> shape(const char* name, std::uint32_t v = 1) {
    return SchemaBuilder(name, v).field("x", Kind::Int).build();
}
} // namespace

TEST_CASE("BL-0: a claimed schema resolves; the final release takes it out of lookup") {
    Registry reg;
    {
        SchemaClaimScope scope = reg.claim({shape("Foo")});
        CHECK(reg.contains("Foo", 1));
        CHECK(reg.size() == 1);
    }
    CHECK_FALSE(reg.contains("Foo", 1));
    CHECK(reg.lookup("Foo", 1) == nullptr);
    CHECK(reg.size() == 0);
}

TEST_CASE("BL-0: two claims, one definition — the first release changes nothing") {
    Registry reg;
    SchemaClaimScope a = reg.claim({shape("Foo")});
    // A SEPARATELY BUILT twin, so this proves convergence and not pointer reuse.
    SchemaClaimScope b = reg.claim({shape("Foo")});
    CHECK(reg.size() == 1); // one canonical definition, whatever the claim count
    const Schema* canonical = reg.lookup("Foo", 1).get();

    a.release();
    REQUIRE(reg.contains("Foo", 1));
    CHECK(reg.lookup("Foo", 1).get() == canonical); // and still the SAME one

    b.release();
    CHECK_FALSE(reg.contains("Foo", 1));
}

TEST_CASE("BL-0: a claim on a conflicting definition refuses, and changes nothing") {
    Registry reg;
    SchemaClaimScope held = reg.claim({SchemaBuilder("Foo", 1).field("x", Kind::Int).build()});
    const ContentId before = reg.lookup("Foo", 1)->content_id();

    auto impostor = SchemaBuilder("Foo", 1).field("x", Kind::Text).build();
    CHECK_THROWS_AS(reg.claim({impostor}), SchemaConflict);

    // The incumbent definition, and the incumbent claim, are exactly as they were.
    CHECK(reg.lookup("Foo", 1)->content_id() == before);
    CHECK(reg.size() == 1);
    held.release();
    CHECK_FALSE(reg.contains("Foo", 1));
}

TEST_CASE("BL-0: a multi-schema acquisition is all or nothing") {
    Registry reg;
    SchemaClaimScope incumbent = reg.claim({SchemaBuilder("Baz", 1).field("x", Kind::Int).build()});

    // Foo and Bar are fine; Baz disagrees with what is already published. The
    // whole request must refuse, and the two innocent shapes must not be left
    // claimed by a caller that never got a scope back.
    CHECK_THROWS_AS(reg.claim({shape("Foo"), shape("Bar"),
                               SchemaBuilder("Baz", 1).field("x", Kind::Text).build()}),
                    SchemaConflict);
    CHECK_FALSE(reg.contains("Foo", 1));
    CHECK_FALSE(reg.contains("Bar", 1));
    CHECK(reg.size() == 1); // only Baz, the one that was already there

    // ...and the failure did not disturb the incumbent's own lifetime either.
    incumbent.release();
    CHECK(reg.size() == 0);
}

TEST_CASE("BL-0: the same shape twice in one request is one claim") {
    Registry reg;
    auto foo = shape("Foo");
    // Asked for twice — a manifest may legitimately name a component in two
    // places. One release must still be enough to end it.
    SchemaClaimScope scope = reg.claim({foo, foo, shape("Foo")});
    CHECK(reg.size() == 1);
    scope.release();
    CHECK_FALSE(reg.contains("Foo", 1));
}

TEST_CASE("BL-0: a null schema in a claim refuses before anything is published") {
    Registry reg;
    CHECK_THROWS_AS(reg.claim({shape("Foo"), nullptr}), std::invalid_argument);
    CHECK(reg.size() == 0);
}

TEST_CASE("BL-0: register_schema is a claim nobody releases") {
    Registry reg;
    reg.register_schema(shape("Core"));
    {
        // A scope claiming the same shape does not take the permanence away...
        SchemaClaimScope scope = reg.claim({shape("Core")});
        CHECK(reg.size() == 1);
    }
    // ...and releasing it cannot reclaim what was published for the Registry's
    // lifetime. This is how core/host vocabulary stays put without an "immortal"
    // flag: it is ordinary lifetime, held by a claim with no end.
    CHECK(reg.contains("Core", 1));
    CHECK(reg.size() == 1);
}

TEST_CASE("BL-0: claim_known pins what exists and skips what does not") {
    Registry reg;
    SchemaClaimScope definer = reg.claim({shape("Spoken")});

    // A producer: it names the shape it may emit, and offers no definition.
    SchemaClaimScope producer;
    reg.claim_known(producer, {{"Spoken", 1}, {"NeverHeardOf", 7}});
    CHECK(reg.size() == 1); // claim_known can never publish anything new

    definer.release();
    // The definer is gone and the shape survives, because something live still
    // needs to be able to say it.
    CHECK(reg.contains("Spoken", 1));
    producer.release();
    CHECK_FALSE(reg.contains("Spoken", 1));
}

TEST_CASE("BL-0: an admitted Value outlives the Registry membership of its schema") {
    Registry reg;
    std::optional<Value> held;
    {
        SchemaClaimScope scope = reg.claim({fx::PlayerState()});
        Value made(reg.lookup("PlayerState", 1));
        made.set("hp", Cell::integer(7));
        made.set("name", Cell::text("kit"));
        Admission a = loom::admit(std::move(made), *reg.lookup("PlayerState", 1));
        REQUIRE(a.ok());
        held = std::move(a).value();
    }
    REQUIRE(held.has_value());
    const Value& v = *held;
    // Registry membership ended. The value did not: schemas are shared owners,
    // so "forgotten" means undiscoverable, never dangling. (ASan/UBSan is what
    // makes this case worth more than the CHECKs below.)
    CHECK_FALSE(reg.contains("PlayerState", 1));
    REQUIRE(v.schema_ptr() != nullptr);
    CHECK(v.schema().name() == "PlayerState");
    CHECK(v.get("hp")->as_int() == 7);
    CHECK(v.get("name")->as_text() == "kit");
    // And it is still a well-formed instance of the shape it remembers.
    CHECK(loom::admit(v, v.schema()).ok());
}

TEST_CASE("BL-0: a lookup taken before the final release stays valid after it") {
    Registry reg;
    std::shared_ptr<const Schema> held;
    {
        SchemaClaimScope scope = reg.claim({shape("Reader")});
        held = reg.lookup("Reader", 1);
    }
    REQUIRE(held != nullptr);
    CHECK(held->name() == "Reader"); // a reader mid-decode is not cut off
    CHECK_FALSE(reg.contains("Reader", 1));
}

TEST_CASE("BL-0: a claim scope is move-only, and moving it moves the lifetime") {
    Registry reg;
    SchemaClaimScope outer;
    {
        SchemaClaimScope inner = reg.claim({shape("Moved")});
        CHECK_FALSE(inner.empty());
        outer = std::move(inner);
        CHECK(inner.empty()); // NOLINT(bugprone-use-after-move) — the point of the check
    }
    // `inner` died holding nothing, so the schema is still here.
    CHECK(reg.contains("Moved", 1));
    outer.release();
    CHECK_FALSE(reg.contains("Moved", 1));
}

TEST_CASE("BL-0: move-assignment acquires before it releases — a handoff has no gap") {
    Registry reg;
    SchemaClaimScope live = reg.claim({shape("Shared"), shape("OldOnly")});
    // The successor's claim is taken FIRST, exactly as a replacement takes it.
    SchemaClaimScope successor = reg.claim({shape("Shared"), shape("NewOnly")});
    CHECK(reg.contains("Shared", 1));

    live = std::move(successor); // release-the-old happens inside, after the acquire
    CHECK(reg.contains("Shared", 1));  // never absent, not for an instant
    CHECK(reg.contains("NewOnly", 1)); // the successor's own shape arrived
    CHECK_FALSE(reg.contains("OldOnly", 1)); // and the predecessor's went
}

TEST_CASE("BL-0: release is idempotent, and a scope may be reused") {
    Registry reg;
    SchemaClaimScope scope = reg.claim({shape("Once")});
    scope.release();
    scope.release(); // saying it twice must not decrement anything twice
    CHECK(reg.size() == 0);

    reg.claim(scope, {shape("Twice")});
    CHECK(reg.contains("Twice", 1));
    scope.release();
    CHECK(reg.size() == 0);
}

TEST_CASE("BL-0: a scope belongs to one Registry, and outliving it is harmless") {
    SchemaClaimScope orphan;
    {
        Registry reg;
        orphan = reg.claim({shape("Orphan")});
        Registry other;
        // Its claims mean nothing in another Registry's bookkeeping.
        CHECK_THROWS_AS(other.claim(orphan, {shape("Elsewhere")}), std::invalid_argument);
    }
    // The Registry is gone; the scope's destructor has nothing to tell and must
    // not reach for it. (ASan is the real assertion here.)
    orphan.release();
    CHECK(orphan.empty());
}

TEST_CASE("BL-0: a long run of distinct claimants does not grow the population") {
    // THE C-10 BOUNDEDNESS PROOF, in the narrowest form that still means it: a
    // host that meets 500 distinct vocabularies one after another, keeping none,
    // ends where it started. Before BL-0 this ended at 500.
    Registry reg;
    reg.register_schema(shape("HostVocabulary")); // the permanent baseline
    const std::size_t baseline = reg.size();
    REQUIRE(baseline == 1);

    for (int i = 0; i < 500; ++i) {
        SchemaClaimScope scope =
            reg.claim({shape(("Transient" + std::to_string(i)).c_str()),
                       shape(("TransientB" + std::to_string(i)).c_str())});
        CHECK(reg.size() == baseline + 2);
    }
    CHECK(reg.size() == baseline);
    CHECK(reg.contains("HostVocabulary", 1)); // ...and the baseline is untouched

    // Holding them all at once is what population growth actually looks like, so
    // the check above is measuring the release and not a failure to claim.
    std::vector<SchemaClaimScope> held;
    for (int i = 0; i < 500; ++i) {
        held.push_back(reg.claim({shape(("Held" + std::to_string(i)).c_str())}));
    }
    CHECK(reg.size() == baseline + 500);
    held.clear();
    CHECK(reg.size() == baseline);
}

TEST_CASE("BL-0: reads stay safe while claims are acquired AND released") {
    // The existing concurrency case only ever grew the map. Removal is the new
    // half: a reader must never see a torn map, and must never be handed a
    // pointer that dies under it.
    Registry reg;
    reg.register_schema(fx::PlayerState()); // permanent: readers may rely on it

    std::atomic<bool> go{false};
    std::atomic<int> read_failures{0};
    std::atomic<int> use_after_release{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < 20000; ++i) {
                if (reg.lookup("PlayerState", 1) == nullptr) {
                    read_failures.fetch_add(1);
                }
                // Whatever churn we catch, holding it must keep it alive.
                if (auto s = reg.lookup("Churn", static_cast<std::uint32_t>(i % 64))) {
                    if (s->name() != "Churn") {
                        use_after_release.fetch_add(1);
                    }
                }
            }
        });
    }

    std::thread writer([&] {
        while (!go.load()) {
        }
        for (int round = 0; round < 200; ++round) {
            std::vector<SchemaClaimScope> scopes;
            for (std::uint32_t v = 0; v < 64; ++v) {
                scopes.push_back(
                    reg.claim({SchemaBuilder("Churn", v).field("x", Kind::Int).build()}));
            }
            scopes.clear(); // every one of them removed again
        }
    });

    go.store(true);
    for (auto& r : readers) {
        r.join();
    }
    writer.join();

    CHECK(read_failures.load() == 0);
    CHECK(use_after_release.load() == 0);
    CHECK(reg.size() == 1); // the permanent one, and nothing the churn left behind
}

} // TEST_SUITE
