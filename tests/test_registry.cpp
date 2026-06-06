#include <doctest.h>

#include "fixtures.hpp"

#include <zen/registry.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace zen;

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

} // TEST_SUITE
