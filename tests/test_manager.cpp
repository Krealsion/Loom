#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/console/console.hpp>
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/kernel/manager.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

// Compose-arg helpers (mirroring test_poke.cpp / test_console.cpp's style).
loom::Arg lit(const char* s) { return loom::Arg{std::nullopt, loom::FieldValue{std::string(s)}}; }

std::string text_field(const loom::Value& v, const char* field) {
    const loom::Cell* c = v.get(field);
    REQUIRE_MESSAGE(c != nullptr, "missing field: " << field);
    return c->as_text();
}

// Drive one console-composed command at `target` and return the buffer entry it
// produced. The operator's whole gesture, in one line.
struct Answer {
    std::string name;
    loom::Value value;
};
Answer drive(ConsoleEngine& engine, WeaveId target, const char* shape,
             std::vector<loom::Arg> args) {
    const std::size_t before = engine.buffer_size();
    Composed c = engine.compose(target, shape, 1, std::move(args));
    REQUIRE_MESSAGE(c.status == Composed::Status::Ready, shape);
    engine.pump();
    REQUIRE_MESSAGE(engine.buffer_size() == before + 1,
                    "expected exactly one answer from " << shape);
    const auto entry = engine.buffer_at(before + 1);
    REQUIRE(entry.has_value());
    return Answer{entry->name, entry->value};
}

// Tap predicates: the bus's own record of what actually happened.
std::size_t delivered_count(const std::vector<TapRecord>& tap, const std::string& shape,
                            WeaveId target) {
    std::size_t n = 0;
    for (const TapRecord& t : tap) {
        if (t.kind == EventKind::Delivered && t.schema == shape && t.target == target) {
            ++n;
        }
    }
    return n;
}
std::size_t refused_count(const std::vector<TapRecord>& tap, const std::string& shape,
                          RefusalReason why) {
    std::size_t n = 0;
    for (const TapRecord& t : tap) {
        if (t.kind == EventKind::Refused && t.schema == shape && t.reason == why) {
            ++n;
        }
    }
    return n;
}

// Decode the live count out of a loaded Weave's snapshot (Counter v1), host-side.
std::int64_t live_count(Switchboard& bus, WeaveId id) {
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, counter_schema());
    REQUIRE(a.ok());
    return a.value().get("count")->as_int();
}

// A consumer that reaches its provider BY ROLE — the addressing that survives a
// replacement. Its grant permits exactly one shape to exactly one role.
struct Go {
    std::int64_t n;
    ZEN_SHAPE(Go, 1, ZEN_FIELD(n));
};
struct GoState {
    std::int64_t n;
    ZEN_SHAPE(GoState, 1, ZEN_FIELD(n));
};

// The lifecycle stack a case needs, wired the way a host wires it.
struct Rig {
    Switchboard bus;
    ConsoleEngine engine{bus};
    Kernel kernel{bus};
    WeaveId control = mount_control(kernel, bus);
    WeaveId manager = mount_manager(control, bus);
};

} // namespace

TEST_SUITE("manager") {

// ---- the kernel door answers, and the Manager relays it ---------------------

TEST_CASE("driven from the existing console: load, list, and every outcome comes back") {
    Rig r;

    // "load this .so under this name, into this role" — composed AT the Manager,
    // with zero console code: the Manager is an ordinary participant the console
    // discovers and addresses like any other.
    bool listed = false;
    for (const auto& w : r.engine.weaves()) {
        if (w.id == r.manager) {
            listed = true;
        }
    }
    CHECK(listed);

    Answer loaded = drive(r.engine, r.manager, "zen.LoadWeave",
                          {lit("spawner"), lit(ZEN_SO_WEAVE), lit("spawner")});
    CHECK(loaded.name == "zen.Result");
    CHECK(r.kernel.is_loaded("spawner"));
    // The Result carries the new weave's id — the kernel's own answer, relayed.
    CHECK(text_field(loaded.value, "value") ==
          std::to_string(r.kernel.weave_id("spawner").value));

    // ListLoaded is answered from the kernel's live map, never a Manager ledger:
    // there is no cache here that could drift from the loading authority's truth.
    Answer list = drive(r.engine, r.manager, "zen.ListLoaded", {});
    CHECK(list.name == "zen.Result");
    CHECK(text_field(list.value, "value") == "spawner@spawner");

    // A second load with no role listed alongside it, still from the same map.
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("plain"), lit(ZEN_SO_WEAVE_B), lit("")});
    Answer list2 = drive(r.engine, r.manager, "zen.ListLoaded", {});
    CHECK(text_field(list2.value, "value") == "plain,spawner@spawner");
}

TEST_CASE("a load failure is a Refused with its why, relayed to the asker — not a dark fate") {
    Rig r;
    Answer a = drive(r.engine, r.manager, "zen.LoadWeave",
                     {lit("bad"), lit("/nonexistent/definitely-not-a.so"), lit("")});
    CHECK(a.name == "zen.Refused");
    const std::string why = text_field(a.value, "reason");
    CHECK(why.find("open failed") != std::string::npos);
    CHECK_FALSE(r.kernel.is_loaded("bad"));
}

// ---- reload-in-place: the refusal that used to die as an unread return value -

TEST_CASE("reload-in-place keeps the WeaveId and transplants the state") {
    Rig r;
    Registered recorder = register_probe(r.bus, {pong_schema()});
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("t"), lit(ZEN_SO_WEAVE), lit("")});
    const WeaveId id = r.kernel.weave_id("t");

    for (int i = 0; i < 3; ++i) {
        r.bus.send(id, Message(ping(1), WeaveId{}, recorder.id));
    }
    r.bus.pump();
    REQUIRE(live_count(r.bus, id) == 3);

    Answer a = drive(r.engine, r.manager, "zen.ReloadWeave", {lit("t"), lit(ZEN_SO_WEAVE_B)});
    CHECK(a.name == "zen.Ack"); // the contentless "done" — the correlation says what

    // Reload-in-place PRESERVES state; it does not reset. Same id, same count.
    CHECK(r.kernel.weave_id("t") == id);
    CHECK(live_count(r.bus, id) == 3);
}

TEST_CASE("reload-in-place refuses a differently-shaped library, and the asker finally hears why") {
    Rig r;
    Registered recorder = register_probe(r.bus, {pong_schema()});
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("t"), lit(ZEN_SO_WEAVE), lit("")});
    const WeaveId id = r.kernel.weave_id("t");

    // Before this phase this refusal existed and was correct — and was discarded
    // at the control door as an unread C++ return value. Now it reaches the
    // operator, self-contained.
    Answer a = drive(r.engine, r.manager, "zen.ReloadWeave", {lit("t"), lit(ZEN_SO_V2)});
    CHECK(a.name == "zen.Refused");
    CHECK(text_field(a.value, "reason") == "state schema version mismatch; reload refused");

    // And the old library keeps running — a clean refusal costs the incumbent nothing.
    CHECK(r.kernel.is_loaded("t"));
    CHECK(r.kernel.weave_id("t") == id);
    r.bus.send(id, Message(ping(5), WeaveId{}, recorder.id));
    r.bus.pump();
    REQUIRE_FALSE(recorder.weave->handled_values.empty());
    CHECK(recorder.weave->handled_values.back() == 5);
}

// ---- swap: replace the role holder -----------------------------------------

TEST_CASE("role continuity: a consumer addresses the role and reaches the successor after a swap") {
    Rig r;
    // The recorder notes WHICH weave answered — the successor is a different
    // weave with a different id, so this is what makes the swap visible.
    std::vector<WeaveId> answered_by;
    Registered recorder = register_probe(r.bus, {pong_schema()});
    recorder.weave->on_handle = [&answered_by](const Message& in, Bus&, ProbeWeave&) {
        answered_by.push_back(in.sender);
    };

    // A consumer granted exactly "Ping to whoever holds 'spawner'" — role-first
    // addressing, the only kind that survives its provider being replaced.
    Grant cg;
    cg.allow_to_role("Ping", 1, "spawner");
    Registered consumer = register_probe(r.bus, {schema_of<Go>()}, 2, true, cg);
    consumer.weave->on_handle = [rid = recorder.id](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("spawner", Message(ping(1), WeaveId{}, rid));
    };
    const Value go = to_value(Go{1});

    drive(r.engine, r.manager, "zen.LoadWeave",
          {lit("spawn_v1"), lit(ZEN_SO_WEAVE), lit("spawner")});
    const WeaveId incumbent = r.kernel.weave_id("spawn_v1");

    r.bus.send(consumer.id, Message(go));
    r.bus.pump();
    REQUIRE(answered_by.size() == 1);
    CHECK(answered_by[0] == incumbent);

    // Swap the role holder for a DIFFERENTLY-SHAPED successor (Counter v2). A
    // different state shape is the normal case for a replacement, not an error —
    // the exact thing reload-in-place refuses one case above.
    Answer swapped = drive(r.engine, r.manager, "zen.SwapWeave",
                           {lit("spawner"), lit("spawn_v2"), lit(ZEN_SO_V2)});
    CHECK(swapped.name == "zen.Result");
    CHECK_FALSE(r.kernel.is_loaded("spawn_v1")); // the incumbent was unloaded
    CHECK(r.kernel.is_loaded("spawn_v2"));
    const WeaveId successor = r.kernel.weave_id("spawn_v2");
    CHECK_FALSE(successor == incumbent);
    CHECK(text_field(swapped.value, "value") == std::to_string(successor.value));

    // The consumer's code is unchanged and its grant is unchanged: it names the
    // role, and the role now resolves to the successor.
    r.bus.send(consumer.id, Message(go));
    r.bus.pump();
    REQUIRE(answered_by.size() == 2);
    CHECK(answered_by[1] == successor);
}

TEST_CASE("one request, one answer: the swap's unload reply is unsolicited and is dropped") {
    Rig r;
    drive(r.engine, r.manager, "zen.LoadWeave",
          {lit("spawn_v1"), lit(ZEN_SO_WEAVE), lit("spawner")});

    // A swap issues TWO messages to the door (UnloadRole, then LoadLibrary) and
    // both are answered. Only the load's answer is correlated to the asker's
    // request; the unload's rides correlation 0, which no relay sequence can ever
    // equal (they start at 1), so the consumer obligation drops it. `drive`
    // REQUIREs exactly one new buffer entry — that is the pin.
    Answer a = drive(r.engine, r.manager, "zen.SwapWeave",
                     {lit("spawner"), lit("spawn_v2"), lit(ZEN_SO_WEAVE_B)});
    CHECK(a.name == "zen.Result");

    // Pumping again produces no straggler: the dropped Ack is gone, not queued.
    const std::size_t settled = r.engine.buffer_size();
    r.engine.pump();
    CHECK(r.engine.buffer_size() == settled);
}

TEST_CASE("a swap takes effect behind queued traffic — and the incumbent's in-flight replies die "
          "with it") {
    Rig r;
    std::vector<WeaveId> answered_by;
    Registered recorder = register_probe(r.bus, {pong_schema()});
    recorder.weave->on_handle = [&answered_by](const Message& in, Bus&, ProbeWeave&) {
        answered_by.push_back(in.sender);
    };
    drive(r.engine, r.manager, "zen.LoadWeave",
          {lit("spawn_v1"), lit(ZEN_SO_WEAVE), lit("spawner")});
    const WeaveId incumbent = r.kernel.weave_id("spawn_v1");

    std::vector<TapRecord> tap;
    r.bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    // Queue a role-send, THEN the swap command, THEN another role-send — and
    // drain once. The swap's own two messages go to the TAIL of the queue, so
    // both role-sends resolve while the incumbent is still the holder.
    r.bus.send_to_role("spawner", Message(ping(1), WeaveId{}, recorder.id));
    Composed c = r.engine.compose(r.manager, "zen.SwapWeave", 1,
                                  {lit("spawner"), lit("spawn_v2"), lit(ZEN_SO_V2)});
    REQUIRE(c.status == Composed::Status::Ready);
    r.bus.send_to_role("spawner", Message(ping(2), WeaveId{}, recorder.id));
    r.bus.pump();

    // INBOUND: both role-sends reached the incumbent. A swap does not steal
    // traffic that was already addressed to the role before it landed.
    CHECK(delivered_count(tap, "Ping", incumbent) == 2);

    // OUTBOUND, and this is the honest half: the incumbent replied to both, but
    // only the FIRST Pong was delivered before the unload. The second was still
    // queued when the incumbent was unregistered, and a gated message is
    // authorized by looking its sender up at DELIVERY time — a sender that no
    // longer exists cannot be authorized, so the bus refuses it CapabilityDenied
    // (fail-closed; switchboard.cpp's `sender != nullptr` term). An unloaded
    // weave's in-flight answers die with it.
    //
    // This is a property of unregistering ANY weave mid-queue, not something the
    // swap invented — the swap is simply the first op that makes it routine. It
    // is the real texture of the swap window, and the concrete thing an
    // invisible/atomic rebind would have to solve if the window is ever felt.
    REQUIRE(answered_by.size() == 1);
    CHECK(answered_by[0] == incumbent);
    CHECK(refused_count(tap, "Pong", RefusalReason::CapabilityDenied) == 1);

    // The replacement did happen, and a send issued after the drain reaches the
    // successor — the role slot carried the consumer's reach across.
    const WeaveId successor = r.kernel.weave_id("spawn_v2");
    CHECK_FALSE(successor == incumbent);
    r.bus.send_to_role("spawner", Message(ping(3), WeaveId{}, recorder.id));
    r.bus.pump();
    REQUIRE(answered_by.size() == 2);
    CHECK(answered_by[1] == successor);
}

TEST_CASE("a failed swap leaves the role unheld: the asker hears why, and the slot refuses "
          "cleanly") {
    Rig r;
    drive(r.engine, r.manager, "zen.LoadWeave",
          {lit("spawn_v1"), lit(ZEN_SO_WEAVE), lit("spawner")});
    REQUIRE(r.kernel.is_loaded("spawn_v1"));

    // The felt friction, admitted at floor tier: swap unloads the incumbent
    // first, so a successor that fails to load leaves the slot empty.
    Answer a = drive(r.engine, r.manager, "zen.SwapWeave",
                     {lit("spawner"), lit("spawn_v2"), lit("/nonexistent/nope.so")});
    CHECK(a.name == "zen.Refused");
    CHECK(text_field(a.value, "reason").find("open failed") != std::string::npos);
    CHECK_FALSE(r.kernel.is_loaded("spawn_v1")); // the incumbent is gone
    CHECK_FALSE(r.kernel.is_loaded("spawn_v2")); // and the successor never arrived

    // The empty slot degrades exactly like an unmounted provider: a clean
    // refusal, never a crash and never a silent swallow. This IS the
    // optional-participation floor, and it is what makes the window survivable.
    Ticket t = r.bus.send_to_role("spawner", Message(ping(1)));
    r.bus.pump();
    CHECK(r.bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);

    // And the slot is genuinely free — a later load takes it.
    Answer again = drive(r.engine, r.manager, "zen.LoadWeave",
                         {lit("spawn_v3"), lit(ZEN_SO_WEAVE_B), lit("spawner")});
    CHECK(again.name == "zen.Result");
}

TEST_CASE("swapping a role nobody holds is a plain load — the unload's 'failure' loses no outcome") {
    Rig r;
    // The unload half is fire-and-forget precisely because its outcome is
    // subsumed by the load's. Here it fails (no holder) and the swap still does
    // exactly what was asked: make the holder of 'spawner' be this weave.
    Answer a = drive(r.engine, r.manager, "zen.SwapWeave",
                     {lit("spawner"), lit("spawn_v1"), lit(ZEN_SO_WEAVE)});
    CHECK(a.name == "zen.Result");
    CHECK(r.kernel.is_loaded("spawn_v1"));
    CHECK(r.kernel.role_of("spawn_v1") == "spawner");
}

TEST_CASE("a swap cannot destroy a weave the asker did not name: the unload is role-addressed") {
    Rig r;
    // An innocent bystander, loaded with NO role.
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("bystander"), lit(ZEN_SO_WEAVE), lit("")});
    // And a real holder of the role.
    drive(r.engine, r.manager, "zen.LoadWeave",
          {lit("holder"), lit(ZEN_SO_WEAVE_B), lit("spawner")});

    drive(r.engine, r.manager, "zen.SwapWeave",
          {lit("spawner"), lit("successor"), lit(ZEN_SO_V2)});

    CHECK(r.kernel.is_loaded("bystander"));      // untouched
    CHECK_FALSE(r.kernel.is_loaded("holder"));   // the role's holder, replaced
    CHECK(r.kernel.is_loaded("successor"));
}

TEST_CASE("an empty role unloads nothing: 'no role' is not a role") {
    Rig r;
    // Two role-less libraries. If UnloadRole treated "" as a matchable role it
    // would match the first role-less entry it walked and unload an arbitrary
    // bystander — a fail-open in the destructive direction, and exactly the kind
    // of thing a marker-check that sniffs presence instead of value gets wrong.
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("a"), lit(ZEN_SO_WEAVE), lit("")});
    drive(r.engine, r.manager, "zen.LoadWeave", {lit("b"), lit(ZEN_SO_WEAVE_B), lit("")});

    Answer a = drive(r.engine, r.manager, "zen.SwapWeave",
                     {lit(""), lit("c"), lit(ZEN_SO_V2)});
    CHECK(a.name == "zen.Result"); // degrades to a plain, role-less load

    CHECK(r.kernel.is_loaded("a")); // both bystanders untouched
    CHECK(r.kernel.is_loaded("b"));
    CHECK(r.kernel.is_loaded("c"));
}

TEST_CASE("a kernel load cannot steal a role a native weave already holds") {
    Rig r;
    // A role held by an ordinary in-process participant — nothing to do with the
    // kernel. A load asking for it must fail cleanly and leave the holder alone.
    auto native = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        pong_schema()});
    ProbeWeave* raw = native.get();
    const WeaveId holder = r.bus.register_weave(std::move(native), Grant{}.allow_any(), "spawner");

    Answer a = drive(r.engine, r.manager, "zen.LoadWeave",
                     {lit("usurper"), lit(ZEN_SO_WEAVE), lit("spawner")});
    CHECK(a.name == "zen.Refused");
    CHECK(text_field(a.value, "reason").find("already held") != std::string::npos);
    CHECK_FALSE(r.kernel.is_loaded("usurper")); // no half-loaded wreckage left behind

    // The incumbent keeps its role AND its life: the slot still resolves to it.
    r.bus.send_to_role("spawner", Message(pong(1)));
    r.bus.pump();
    CHECK(raw->handled_names.size() == 1);
    CHECK(r.bus.alive(holder));
}

// ---- the hostile frame ------------------------------------------------------

TEST_CASE("a forged answer from a third party is dropped, and does not poison the real one") {
    Rig r;
    // An impostor granted the standard reply shapes — the vocabulary is
    // universal, so emitting one is fully sayable through the honest API. What it
    // CANNOT do is speak as the kernel door: the sender is bus-stamped.
    Grant ig;
    ig.allow_to_any(Result::zen_name, Result::zen_version);
    Registered impostor = register_probe(r.bus, {schema_of<Go>()}, 2, true, ig);
    const WeaveId mgr = r.manager;
    impostor.weave->on_handle = [mgr](const Message&, Bus& b, ProbeWeave&) {
        // correlation 1 is the Manager's FIRST relay sequence — a correct guess.
        b.send(mgr, Message(to_value(Result{"66613371"}), WeaveId{}, WeaveId{}, 1));
    };

    std::vector<TapRecord> tap;
    r.bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    // Queue the operator's load and the impostor's trigger together, so the
    // forged answer is delivered to the Manager BEFORE the door's real one.
    Composed c = r.engine.compose(r.manager, "zen.LoadWeave",
                                  1, {lit("real"), lit(ZEN_SO_WEAVE), lit("")});
    REQUIRE(c.status == Composed::Status::Ready);
    r.bus.send(impostor.id, Message(to_value(Go{1})));
    r.bus.pump();

    // FIRST: prove the hostile frame actually ARRIVED. Without this the case
    // could pass vacuously — a forged reply that never reached the Manager (a
    // denied grant, a wrong target) would look exactly like one the relay
    // correctly dropped. The pin is "delivered AND ignored", never just "absent".
    CHECK(delivered_count(tap, "zen.Result", r.manager) == 2); // the forgery, then the door's

    // Exactly one answer reached the operator, and it is the door's.
    REQUIRE(r.engine.buffer_size() == 1);
    const auto entry = r.engine.buffer_at(1);
    REQUIRE(entry.has_value());
    CHECK(entry->name == "zen.Result");
    CHECK(text_field(entry->value, "value") != "66613371");
    CHECK(text_field(entry->value, "value") == std::to_string(r.kernel.weave_id("real").value));
}

TEST_CASE("an unsolicited standard reply to the Manager reaches no asker") {
    Rig r;
    Grant ig;
    ig.allow_to_any(Ack::zen_name, Ack::zen_version);
    Registered impostor = register_probe(r.bus, {schema_of<Go>()}, 2, true, ig);
    const WeaveId mgr = r.manager;
    impostor.weave->on_handle = [mgr](const Message&, Bus& b, ProbeWeave&) {
        b.send(mgr, Message(to_value(Ack{}), WeaveId{}, WeaveId{}, 7));
    };

    std::vector<TapRecord> tap;
    r.bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    r.bus.send(impostor.id, Message(to_value(Go{1})));
    r.bus.pump();

    // Again: delivered AND ignored. The Ack reached the Manager's door and the
    // Manager simply had nothing outstanding to match it against.
    CHECK(delivered_count(tap, "zen.Ack", r.manager) == 1);
    CHECK(r.engine.buffer_size() == 0); // nothing outstanding, so nothing relayed
}

// ---- no privilege -----------------------------------------------------------

TEST_CASE("the Manager holds no privilege: a second granted participant drives the door alone") {
    Rig r;
    // Exactly the Manager's kernel authority — load_capability, nothing more —
    // handed to an ordinary weave. No Manager is in this path at all.
    struct Driver : WeaveBase<Driver, GoState, Accept<Go>, Emit<LoadLibrary>> {
        WeaveId control{};
        void on(const Go&, Mail& mail) {
            mail.send(control, LoadLibrary{"direct", ZEN_SO_WEAVE, "spawner"});
        }
    };
    WeaveId driver = mount_granted<Driver>(r.bus, load_capability(r.control));
    static_cast<Driver*>(r.bus.weave(driver))->control = r.control;

    r.bus.send(driver, Message(to_value(Go{1})));
    r.bus.pump();

    CHECK(r.kernel.is_loaded("direct"));
    CHECK(r.kernel.role_of("direct") == "spawner");

    // And the same door refuses a participant that was granted nothing — the
    // authority is the grant, never the identity of who is asking.
    struct Sneak : WeaveBase<Sneak, GoState, Accept<Go>, Emit<LoadLibrary>> {
        WeaveId control{};
        void on(const Go&, Mail& mail) {
            mail.send(control, LoadLibrary{"sneaked", ZEN_SO_WEAVE_B, ""});
        }
    };
    WeaveId sneak = mount_granted<Sneak>(r.bus, Grant::nothing());
    static_cast<Sneak*>(r.bus.weave(sneak))->control = r.control;
    r.bus.send(sneak, Message(to_value(Go{1})));
    r.bus.pump();
    CHECK_FALSE(r.kernel.is_loaded("sneaked"));
}

// ---- the steward is itself inspectable --------------------------------------

TEST_CASE("the Manager is poke-inspectable like any weave: its bookkeeping is not secret") {
    Rig r;
    // The console's wildcard-accept admits only REGISTERED shapes; zen.PokeStructure
    // is registered by whoever ACCEPTS it, and the Manager (unlike the Poke weave)
    // does not. A listening probe stands in — the same convention the poke and
    // console suites use for a direct poke.
    register_probe(r.bus, {schema_of<PokeStructure>()});
    Answer a = drive(r.engine, r.manager, "zen.PokeDescribe", {});
    CHECK(a.name == "zen.PokeStructure");
    const auto& fields = a.value.get("fields")->as_list();
    REQUIRE(fields.size() == 2);
    CHECK(text_field(*fields[0].as_message(), "name") == "next_seq");
    CHECK(text_field(*fields[1].as_message(), "name") == "pending");
    // Default access: readable, not writable — the steward's state is visible
    // and not manipulable, which is exactly the floor every weave gets.
    CHECK_FALSE(fields[0].as_message()->get("hidden")->as_bool());
    CHECK_FALSE(fields[0].as_message()->get("writable")->as_bool());
}

} // TEST_SUITE
