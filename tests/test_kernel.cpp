#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstddef>
#include <string>

using namespace loom;
using namespace loom;
using namespace loom;
using namespace sbfx;

namespace {

// Decode the live count out of a Weave's snapshot (Counter v1), host-side.
std::int64_t live_count(Switchboard& bus, WeaveId id) {
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, counter_schema());
    REQUIRE(a.ok());
    return a.value().get("count")->as_int();
}

} // namespace

TEST_SUITE("kernel") {

TEST_CASE("the containment note tells the truth for this platform's hosting mode") {
    // The honesty floor for the in-process kernel: it never claims a sandbox
    // anywhere, and the Windows development/demo backend says its weaker
    // nature out loud. Pinned per platform so a wording drift toward a
    // stronger claim is a red test, not a review catch.
    const std::string note = Kernel::containment_note();
#if defined(_WIN32)
    CHECK(note.find("unisolated") != std::string::npos);
    CHECK(note.find("no sandbox") != std::string::npos);
    CHECK(note.find("development/demo") != std::string::npos);
#else
    CHECK(note.find("no OS sandbox") != std::string::npos);
#endif
    // On no platform may this surface claim containment.
    CHECK(note.find("contained") == std::string::npos);
    CHECK(note.find("sandboxed") == std::string::npos);
}

TEST_CASE("a loaded DLL Weave mounts and is indistinguishable; both directions are gated") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("test", ZEN_SO_WEAVE);
    REQUIRE_MESSAGE(lr.ok, lr.error);
    CHECK(bus.alive(lr.id));
    CHECK(kernel.weave_id("test") == lr.id);

    const auto before = gate_invocations();
    bus.send(lr.id, Message(ping(7), /*sender=*/WeaveId{}, /*reply_to=*/recorder.id));
    bus.pump();
    const auto after = gate_invocations();

    // A delivery TO the DLL Weave, plus the Pong it EMITTED (admitted host-side),
    // plus that Pong's delivery to the recorder — all through the one gate.
    CHECK(after >= before + 2);
    REQUIRE(recorder.weave->handled_names.size() == 1);
    CHECK(recorder.weave->handled_names[0] == "Pong");
    CHECK(recorder.weave->handled_values[0] == 7);
}

TEST_CASE("a DLL that emits a malformed message is refused by the host gate, never routed") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("bad", ZEN_SO_BADMSG);
    REQUIRE(lr.ok);

    bus.send(lr.id, Message(ping(1), WeaveId{}, recorder.id));
    bus.pump();

    // The DLL handled the valid Ping, then emitted a Pong missing 'seq'; the host
    // gate refused it, so the recorder received nothing.
    CHECK(recorder.weave->handled_names.empty());
}

TEST_CASE("a DLL whose snapshot is malformed is refused at load by the host gate") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult lr = kernel.load("bs", ZEN_SO_BADSNAP);
    CHECK_FALSE(lr.ok);
    CHECK_FALSE(lr.error.empty());
    CHECK_FALSE(kernel.is_loaded("bs"));
}

TEST_CASE("a descriptor with an unsupported abi_version is rejected cleanly") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult lr = kernel.load("ba", ZEN_SO_BADABI);
    CHECK_FALSE(lr.ok);
    CHECK(lr.error.find("abi_version") != std::string::npos);
    CHECK_FALSE(kernel.is_loaded("ba"));
}

TEST_CASE("hot-reload swaps the library and the state survives the swap") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);
    const WeaveId id = lr.id;

    // Drive the Weave so its state advances to count == 3.
    for (int i = 0; i < 3; ++i) {
        bus.send(id, Message(ping(1), WeaveId{}, recorder.id));
    }
    bus.pump();
    CHECK(live_count(bus, id) == 3);

    // Reload from an identical "rebuilt" library; the WeaveId is unchanged.
    ReloadResult rr = kernel.reload_from("t", ZEN_SO_WEAVE_B);
    REQUIRE_MESSAGE(rr.ok, rr.error);
    CHECK(rr.reloaded);
    CHECK_FALSE(rr.version_mismatch);
    CHECK(kernel.weave_id("t") == id);

    // State round-tripped through host-owned bytes and the gate: still 3.
    CHECK(live_count(bus, id) == 3);

    // And it still works after the swap.
    bus.send(id, Message(ping(9), WeaveId{}, recorder.id));
    bus.pump();
    CHECK(recorder.weave->handled_values.back() == 9);
}

TEST_CASE("intentional hot-reload spends no crash-revival budget: it never exhausts") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);
    const WeaveId id = lr.id;

    // The DLL declares max_reloads = 8. If hot-reload drew from that budget, the
    // 9th swap would be "exhausted". Swap many more times than the budget and
    // require every one to succeed — intentional swap is unbudgeted.
    constexpr int kSwaps = 12;
    for (int i = 0; i < kSwaps; ++i) {
        const char* path = (i % 2 == 0) ? ZEN_SO_WEAVE_B : ZEN_SO_WEAVE;
        ReloadResult rr = kernel.reload_from("t", path);
        REQUIRE_MESSAGE(rr.ok, rr.error);
        CHECK(rr.reloaded);
        CHECK_FALSE(rr.version_mismatch);
    }
    CHECK(kernel.weave_id("t") == id);

    // Still live and serving after a dozen swaps.
    bus.send(id, Message(ping(77), WeaveId{}, recorder.id));
    bus.pump();
    REQUIRE_FALSE(recorder.weave->handled_values.empty());
    CHECK(recorder.weave->handled_values.back() == 77);
}

TEST_CASE("a reload to a newer state-schema version is a clean refusal; the old library runs on") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);
    const WeaveId id = lr.id;

    ReloadResult rr = kernel.reload_from("t", ZEN_SO_V2);
    CHECK(rr.ok);
    CHECK_FALSE(rr.reloaded);
    CHECK(rr.version_mismatch);
    CHECK(kernel.is_loaded("t"));

    // The original (v1) Weave keeps running.
    bus.send(id, Message(ping(5), WeaveId{}, recorder.id));
    bus.pump();
    REQUIRE_FALSE(recorder.weave->handled_values.empty());
    CHECK(recorder.weave->handled_values.back() == 5);
}

// ---- R2A-1: the accept-set query, and reload's whole-contract check ----------

TEST_CASE("the accept-set query answers from the bus's published set, and nowhere else") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult lr = kernel.load("t", ZEN_SO_ACTIVATES);
    REQUIRE_MESSAGE(lr.ok, lr.error);

    // Positive: a door the library's own manifest declared.
    CHECK(kernel.accepts(lr.id, loom::Activated::zen_name, loom::Activated::zen_version));
    CHECK(kernel.accepts(lr.id, "Ping", 1));
    // Negative: a shape it never declared, and the RIGHT name at the WRONG
    // version — version is part of the identity routing selects on, so a v2 door
    // is a different door, not the same one.
    CHECK_FALSE(kernel.accepts(lr.id, "Greet", 1));
    CHECK_FALSE(kernel.accepts(lr.id, loom::Activated::zen_name, 2));
    // Unknown / never-loaded / already-unloaded ids are a clean false, not an
    // error and not a throw: the bus answers an empty set for an id it has never
    // heard of, and this must read that as "declares nothing".
    CHECK_FALSE(kernel.accepts(WeaveId{}, loom::Activated::zen_name, 1));
    CHECK_FALSE(kernel.accepts(WeaveId{999999}, loom::Activated::zen_name, 1));
    REQUIRE(kernel.unload("t"));
    CHECK_FALSE(kernel.accepts(lr.id, loom::Activated::zen_name, 1));

    // A weave that never declared it is the ordinary non-participant.
    LoadResult plain = kernel.load("p", ZEN_SO_WEAVE);
    REQUIRE(plain.ok);
    CHECK(kernel.accepts(plain.id, "Ping", 1));
    CHECK_FALSE(kernel.accepts(plain.id, loom::Activated::zen_name, 1));
}

TEST_CASE("reload refuses a drifted door contract before commit; the incumbent keeps everything") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered recorder = register_probe(bus, {pong_schema()});

    LoadResult lr = kernel.load("t", ZEN_SO_ACTIVATES);
    REQUIRE_MESSAGE(lr.ok, lr.error);
    const WeaveId id = lr.id;
    const std::size_t doors = bus.accepted_schemas(id).size();

    // The candidate's STATE schema is byte-identical to the incumbent's — the
    // only agreement reload checked before this phase, so this would have
    // committed and then routed by a contract the new code no longer speaks.
    ReloadResult rr = kernel.reload_from("t", ZEN_SO_ACTIVATES_DRIFT);
    CHECK(rr.ok);
    CHECK_FALSE(rr.reloaded);
    CHECK_FALSE(rr.version_mismatch); // deliberately NOT the state-schema refusal
    CHECK(rr.error == "accepted schema contract mismatch; reload refused");

    // Refused BEFORE rebind, so nothing about the incumbent moved: same id, same
    // published doors, same live implementation still serving.
    CHECK(kernel.is_loaded("t"));
    CHECK(kernel.weave_id("t") == id);
    CHECK(bus.accepted_schemas(id).size() == doors);
    CHECK_FALSE(kernel.accepts(id, "Greet", 1)); // the candidate's extra door was never published
    CHECK(kernel.accepts(id, "Ping", 1));
    CHECK(kernel.accepts(id, loom::Activated::zen_name, loom::Activated::zen_version));

    bus.send(id, Message(ping(5), WeaveId{}, recorder.id));
    bus.pump();
    REQUIRE_FALSE(recorder.weave->handled_values.empty());
    CHECK(recorder.weave->handled_values.back() == 5);

    // And the drifted shape genuinely cannot be routed to it — the accept-set is
    // the incumbent's, in fact and not only in the query's answer.
    Ticket t = bus.send(id, Message(greet("hi")));
    bus.pump();
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NotAccepted);

    // The refusal is about the DRIFT, not about reload: the same library with an
    // identical contract still reloads in place.
    ReloadResult ok = kernel.reload_from("t", ZEN_SO_ACTIVATES_B);
    REQUIRE_MESSAGE(ok.ok, ok.error);
    CHECK(ok.reloaded);
    CHECK(kernel.weave_id("t") == id);
}

TEST_CASE("unload tears down cleanly: instance destroyed before the library closes") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);
    const WeaveId id = lr.id;

    CHECK(kernel.unload("t"));
    CHECK_FALSE(kernel.is_loaded("t"));
    CHECK_FALSE(bus.alive(id));

    // The weave is gone; a directed send is refused, not delivered into a closed library.
    Ticket t = bus.send(id, Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

TEST_CASE("the kernel unloads everything it still holds at destruction") {
    Switchboard bus;
    {
        Kernel kernel(bus);
        REQUIRE(kernel.load("a", ZEN_SO_WEAVE).ok);
        REQUIRE(kernel.load("b", ZEN_SO_WEAVE_B).ok);
        CHECK(kernel.loaded().size() == 2);
        // kernel goes out of scope here: it must unload both, leaving the bus clean.
    }
    CHECK(bus.list_weaves().empty());
}

} // TEST_SUITE
