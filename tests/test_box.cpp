// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The kernel's WebAssembly backend (LOOM_ENABLE_WASM), through real modules loom_wasm_weave()
// built: a box opens, answers, refuses and supervises as src/kernel/wasm_box.hpp says, and a
// native weave reloads into its module and back. Suite `box`, gate `box`.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include "box_reload/counter_shapes.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/box_probe_shapes.hpp"

#include <zen/kernel/kernel.hpp>

#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;
using namespace box_probe;

namespace {

/// A Kernel with one boxed probe and a native asker that asks it on each Tick: what it heard,
/// whether Loom attested each answer, and how many of the box's deliveries failed.
struct Rig {
    Switchboard bus;
    Kernel kernel{bus, fixture_admission()};
    LoadResult box;
    Registered asker{};
    std::vector<BoxPong> answers;
    bool attested = true;
    std::size_t failed = 0;

    explicit Rig(const char* module, const Grant* grant = nullptr) {
        box = grant != nullptr ? kernel.load("box", module, "", *grant)
                               : kernel.load("box", module);
        asker = register_probe(bus, {schema_of<BoxPong>(), tick_schema()});
        asker.weave->on_handle = [this](const Message& in, Bus& b, ProbeWeave&) {
            if (in.payload.schema().name() == "Tick") {
                b.send(box.id, Message(to_value(BoxPing{in.payload.get("n")->as_int()}),
                                       WeaveId{}, WeaveId{}, 7));
                return;
            }
            attested = attested && in.provenance.answers_ask();
            answers.push_back(BoxPong{in.payload.get("seq")->as_int(),
                                      in.payload.get("count")->as_int(),
                                      in.payload.get("memory_refused")->as_bool()});
        };
        bus.add_observer([this](const BusEvent& ev) {
            if (ev.kind == EventKind::HandlerFailed && ev.target == box.id) {
                ++failed;
            }
        });
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    /// One ask of the boxed probe, delivered and settled.
    void ask(std::int64_t seq) {
        bus.send(asker.id, Message(tick(seq)));
        bus.drain_until_idle();
    }
};

std::int64_t live_count(Switchboard& bus, WeaveId id) {
    return bus.weave(id)->snapshot().get("count")->as_int();
}

/// A native counter that asks, for the reload cases.
struct CounterRig {
    Switchboard bus;
    Kernel kernel{bus, fixture_admission()};
    LoadResult counter;
    Registered asker{};
    std::vector<box_reload::CountAnswer> answers;
    std::int64_t next = 0;

    explicit CounterRig(const Grant* grant = nullptr) {
        counter = grant != nullptr ? kernel.load("c", ZEN_SO_BOX_COUNTER, "", *grant)
                                   : kernel.load("c", ZEN_SO_BOX_COUNTER);
        asker = register_probe(bus, {schema_of<box_reload::CountAnswer>(), tick_schema()});
        asker.weave->on_handle = [this](const Message& in, Bus& b, ProbeWeave&) {
            if (in.payload.schema().name() == "Tick") {
                b.send(counter.id, Message(to_value(box_reload::CountAsk{++next})));
                return;
            }
            answers.push_back(box_reload::CountAnswer{
                in.payload.get("seq")->as_int(), in.payload.get("count")->as_int(),
                in.payload.get("sum")->as_int(), in.payload.get("build")->as_text()});
        };
    }
    CounterRig(const CounterRig&) = delete;
    CounterRig& operator=(const CounterRig&) = delete;

    void ask() {
        bus.send(asker.id, Message(tick(0)));
        bus.drain_until_idle();
    }
};

} // namespace

TEST_SUITE("box") {

TEST_CASE("a boxed weave answers through Loom's gate, and its asker sees each answer attested") {
    Rig r(ZEN_BOX_PROBE);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    CHECK(r.kernel.status("box") == ArtifactStatus::Live);
    for (std::int64_t i = 1; i <= 20; ++i) {
        r.ask(i);
    }
    REQUIRE(r.answers.size() == 20);
    for (std::size_t i = 0; i < r.answers.size(); ++i) {
        CHECK(r.answers[i].seq == static_cast<std::int64_t>(i) + 1);
        CHECK(r.answers[i].count == static_cast<std::int64_t>(i) + 1);
    }
    CHECK(r.attested);
    CHECK(r.failed == 0);
    CHECK(live_count(r.bus, r.box.id) == 20);
}

TEST_CASE("a module importing anything outside the box's set is refused at load, by name, "
          "before any of its code runs") {
    struct Case {
        const char* path;
        const char* import;
    };
    for (const Case c : {Case{ZEN_BOX_PROBE_PATH_OPEN, "wasi_snapshot_preview1.path_open"},
                         Case{ZEN_BOX_PROBE_ENV, "env.x"}}) {
        CAPTURE(c.import);
        Switchboard bus;
        Kernel kernel(bus, fixture_admission());
        const KernelLifetimeCounts before = kernel_lifetime_counts();
        const LoadResult lr = kernel.load("reach", c.path);
        const KernelLifetimeCounts after = kernel_lifetime_counts();
        CHECK_FALSE(lr.ok);
        CHECK(lr.error.find(c.import) != std::string::npos);
        CHECK(lr.error.find("never grants") != std::string::npos);
        // No store was opened, so no instance existed and none of the module's code ran.
        CHECK(after.stores_opened == before.stores_opened);
        CHECK(after.instances_created == before.instances_created);
        CHECK_FALSE(kernel.is_loaded("reach"));
    }
}

TEST_CASE("a module built against another ABI is refused at load, both versions named, before "
          "its weave is made") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const LoadResult lr = kernel.load("stale", ZEN_BOX_STALE_ABI);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK_FALSE(lr.ok);
    CHECK(lr.error.find("abi_version " + std::to_string(ZEN_ABI_VERSION - 1u)) !=
          std::string::npos);
    CHECK(lr.error.find("host supports " + std::to_string(ZEN_ABI_VERSION)) != std::string::npos);
    CHECK(after.instances_created == before.instances_created);
    // The one check made when a module is opened ran in a store of its own, closed again.
    CHECK(after.stores_opened - before.stores_opened == after.stores_closed - before.stores_closed);
}

TEST_CASE("a door named bytes outside the module's memory traps, and the next ask is answered by "
          "an instance revived from the state before it") {
    Rig r(ZEN_BOX_PROBE);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    r.ask(1);
    r.ask(2);
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    r.ask(kForge);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK(r.failed == 1);
    CHECK(r.answers.size() == 2);
    CHECK(after.stores_opened - before.stores_opened == 1); // the revived instance's
    r.ask(3);
    REQUIRE(r.answers.size() == 3);
    CHECK(r.answers.back().seq == 3);
    CHECK(r.answers.back().count == 3); // two counted before the trap, this one after it
    CHECK(r.attested);
}

TEST_CASE("a handler that never returns is ended by the step bound, and its delivery is "
          "HandlerFailed") {
    Rig r(ZEN_BOX_PROBE);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    r.ask(1);
    const auto started = std::chrono::steady_clock::now();
    r.ask(kSpin);
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(r.failed == 1);
    CHECK(r.answers.size() == 1);
    CHECK(took < std::chrono::seconds(30));
    r.ask(2);
    REQUIRE(r.answers.size() == 2);
    CHECK(r.answers.back().count == 2);
}

TEST_CASE("memory past the box's bound is refused inside the module, which answers") {
    Rig r(ZEN_BOX_PROBE);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    r.ask(kGrow);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    REQUIRE(r.answers.size() == 1);
    CHECK(r.answers.back().memory_refused);
    CHECK(r.failed == 0);
    CHECK(after.stores_opened == before.stores_opened); // no trap, no revival
}

TEST_CASE("the box's memory is the grant's: a grant naming more lets the module have it") {
    ResourceLimits more;
    more.memory_bytes = std::int64_t{1} << 30;
    const Grant grant = Grant{}.allow_any().with_resources(more);
    Rig r(ZEN_BOX_PROBE, &grant);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    r.ask(kGrow);
    REQUIRE(r.answers.size() == 1);
    CHECK_FALSE(r.answers.back().memory_refused);
    CHECK(r.failed == 0);
}

TEST_CASE("a trapped instance is quarantined after its weave's own max_reloads") {
    Rig r(ZEN_BOX_PROBE);
    REQUIRE_MESSAGE(r.box.ok, r.box.error);
    r.ask(1);
    for (std::int64_t reload = 1; reload <= kMaxReloads; ++reload) {
        r.ask(kTrap);
        r.ask(1 + reload);
        CAPTURE(reload);
        REQUIRE(r.answers.size() == static_cast<std::size_t>(1 + reload));
        CHECK(r.answers.back().count == 1 + reload); // revived each time, its state kept
    }
    const std::size_t failed = r.failed;
    r.ask(kTrap); // past the budget: quarantined
    r.ask(100);
    CHECK(r.answers.size() == static_cast<std::size_t>(1 + kMaxReloads));
    CHECK(r.failed == failed + 2); // the trap, then the ask a quarantined box cannot take
    // Quarantined, it still says its last good state, so a reload can carry the weave away.
    CHECK(live_count(r.bus, r.box.id) == 1 + kMaxReloads);
}

TEST_CASE("unloading and loading the same module fifty times compiles nothing new and leaves no "
          "store open") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    {
        const LoadResult warm = kernel.load("box", ZEN_BOX_PROBE);
        REQUIRE_MESSAGE(warm.ok, warm.error);
        REQUIRE(kernel.unload("box"));
    }
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    for (int i = 0; i < 50; ++i) {
        const LoadResult lr = kernel.load("box", ZEN_BOX_PROBE);
        REQUIRE_MESSAGE(lr.ok, lr.error);
        bus.send(lr.id, Message(to_value(BoxPing{i})));
        bus.drain_until_idle();
        REQUIRE(kernel.unload("box"));
    }
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK(after.modules_compiled == before.modules_compiled);
    CHECK(after.stores_opened - before.stores_opened == after.stores_closed - before.stores_closed);
    CHECK(after.libraries_opened - before.libraries_opened == 50);
    CHECK(after.libraries_closed - before.libraries_closed == 50);
    CHECK(after.instances_created - before.instances_created == 50);
    CHECK(after.instances_destroyed - before.instances_destroyed == 50);
}

TEST_CASE("a grant the box cannot carry is refused at load, in words, and narrowed by nothing") {
    struct Case {
        Grant grant;
        const char* named;
    };
    ResourceLimits weighted;
    weighted.cpu_weight = 200;
    for (const Case& c :
         {Case{Grant{}.allow_any().with_filesystem(FsAccess::ReadOnly, "."), "filesystem"},
          Case{Grant{}.allow_any().with_os_capabilities(os_cap::Network), "network"},
          Case{Grant{}.allow_any().with_resources(weighted), "cpu weight"}}) {
        CAPTURE(c.named);
        Switchboard bus;
        Kernel kernel(bus, fixture_admission());
        const KernelLifetimeCounts before = kernel_lifetime_counts();
        const LoadResult lr = kernel.load("box", ZEN_BOX_PROBE, "", c.grant);
        const KernelLifetimeCounts after = kernel_lifetime_counts();
        CHECK_FALSE(lr.ok);
        CHECK(lr.error.find(c.named) != std::string::npos);
        CHECK(lr.error.find("narrows none") != std::string::npos);
        CHECK_FALSE(kernel.is_loaded("box"));
        // The instance made before the grant was judged is gone, its store with it.
        CHECK(after.instances_destroyed - before.instances_destroyed ==
              after.instances_created - before.instances_created);
        CHECK(after.stores_opened - before.stores_opened ==
              after.stores_closed - before.stores_closed);
    }
}

TEST_CASE("a native weave reloads into its module and back, keeping its id and its state") {
    CounterRig r;
    REQUIRE_MESSAGE(r.counter.ok, r.counter.error);
    const WeaveId id = r.counter.id;
    r.ask();
    r.ask();
    const ReloadResult into = r.kernel.reload_from("c", ZEN_BOX_COUNTER_WASM);
    REQUIRE_MESSAGE(into.reloaded, into.error);
    r.ask();
    const ReloadResult back = r.kernel.reload_from("c", ZEN_SO_BOX_COUNTER);
    REQUIRE_MESSAGE(back.reloaded, back.error);
    r.ask();
    REQUIRE(r.answers.size() == 4);
    CHECK(r.answers[1].build == "native");
    CHECK(r.answers[2].build == "wasm32");
    CHECK(r.answers[3].build == "native");
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(r.answers[i].count == static_cast<std::int64_t>(i) + 1);
    }
    CHECK(r.answers[3].sum == 1 + 2 + 3 + 4);
    CHECK(r.kernel.weave_id("c") == id);
}

TEST_CASE("a reload the box refuses leaves the native weave answering, untouched") {
    SUBCASE("a module importing what a box never grants") {
        CounterRig r;
        REQUIRE_MESSAGE(r.counter.ok, r.counter.error);
        r.ask();
        const ReloadResult rr = r.kernel.reload_from("c", ZEN_BOX_COUNTER_REACHING_WASM);
        CHECK_FALSE(rr.reloaded);
        CHECK(rr.error.find("wasi_snapshot_preview1.path_open") != std::string::npos);
        r.ask();
        REQUIRE(r.answers.size() == 2);
        CHECK(r.answers[1].build == "native");
        CHECK(r.answers[1].count == 2);
    }
    SUBCASE("a grant the box cannot carry") {
        const Grant grant = Grant{}.allow_any().with_filesystem(FsAccess::ReadOnly, ".");
        CounterRig r(&grant);
        REQUIRE_MESSAGE(r.counter.ok, r.counter.error);
        r.ask();
        const KernelLifetimeCounts before = kernel_lifetime_counts();
        const ReloadResult rr = r.kernel.reload_from("c", ZEN_BOX_COUNTER_WASM);
        const KernelLifetimeCounts after = kernel_lifetime_counts();
        CHECK_FALSE(rr.reloaded);
        CHECK(rr.error.find("filesystem") != std::string::npos);
        CHECK(after.instances_destroyed - before.instances_destroyed == 1); // the candidate's
        r.ask();
        REQUIRE(r.answers.size() == 2);
        CHECK(r.answers[1].build == "native");
        CHECK(r.answers[1].count == 2);
    }
}

} // TEST_SUITE("box")
