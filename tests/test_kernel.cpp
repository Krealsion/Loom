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

// ---- R2B-2: reading the deferring steward's mind, host-side ------------------
//
// A loaded weave has exactly one window on itself — the snapshot it already had
// to provide — so that is where the fixture puts what these tests need to see. No
// back channel was invented for the test.

// Counter v4, the ZEN_WEAVE_DEFERS state contract, spelled out again here on
// purpose: the test must not share a definition with the library it is
// interrogating, or a drift in either would cancel out.
inline std::shared_ptr<const Schema> defers_state_schema() {
    static const auto s = SchemaBuilder("Counter", 4)
                              .field("count", Kind::Int)
                              .field("deferred", Kind::Int)
                              .field("spent", Kind::Int)
                              .field("token", Kind::Int)
                              .build();
    return s;
}

struct Steward {
    std::int64_t count = 0;    ///< deliveries handled (either shape)
    std::int64_t deferred = 0; ///< did the last ask yield a retained answer right?
    std::int64_t spent = 0;    ///< how many spends the BOARD accepted
    std::int64_t token = 0;    ///< the opaque number it is holding, if any
};

inline Steward steward_state(Switchboard& bus, WeaveId id) {
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, defers_state_schema());
    REQUIRE(a.ok());
    return Steward{a.value().get("count")->as_int(), a.value().get("deferred")->as_int(),
                   a.value().get("spent")->as_int(), a.value().get("token")->as_int()};
}

// A correlation the asker chooses and the steward is never told in any payload.
constexpr std::uint64_t kAskCorrelation = 0x5EEDu;

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

TEST_CASE("a rejected candidate's schemas stay admitted — today's registry monotonicity, named") {
    // THIS PINS CURRENT BEHAVIOUR; IT DOES NOT ENDORSE IT AS FINAL DOCTRINE.
    //
    // Reconstructing a candidate's manifest is what PRODUCES the schemas the
    // compatibility check then compares, and reconstruct() admits them into this
    // Kernel's dependency registry on the way — so a candidate refused for
    // accepted-contract drift has already bound its (name, version) keys. That
    // is why "refused before commit" is scoped to *incumbent replacement and its
    // published routing contract* and not to "the Loom is unchanged".
    //
    // Whether that admission is intentionally monotonic, or should join a future
    // prepared-replacement transaction, is an R2B decision. Nothing here answers
    // it. (Note the Switchboard's own registry is a DIFFERENT registry and is
    // untouched: only register_weave writes it, and a rejected candidate never
    // registers — so nothing the bus routes or resolves by is affected.)
    {
        // THE NEGATIVE CONTROL, first, or the pin below proves nothing: in a
        // fresh kernel the conflicting library loads perfectly well. Its refusal
        // further down is therefore caused by the rejected candidate's
        // admission, not by the fixture being intrinsically unloadable.
        Switchboard bus;
        Kernel kernel(bus);
        REQUIRE(kernel.load("t", ZEN_SO_ACTIVATES).ok);
        LoadResult clean = kernel.load("u", ZEN_SO_ACTIVATES_CONFLICT);
        CHECK_MESSAGE(clean.ok, clean.error);
        CHECK(kernel.is_loaded("u"));
    }

    Switchboard bus;
    Kernel kernel(bus);
    LoadResult lr = kernel.load("t", ZEN_SO_ACTIVATES);
    REQUIRE_MESSAGE(lr.ok, lr.error);

    // A reload refused for door drift. The candidate declared Greet v1 {msg}.
    ReloadResult rr = kernel.reload_from("t", ZEN_SO_ACTIVATES_DRIFT);
    REQUIRE(rr.ok);
    REQUIRE_FALSE(rr.reloaded);
    CHECK(rr.error == "accepted schema contract mismatch; reload refused");

    // The R2A-1 claim, unchanged and still true: the incumbent's published
    // routing contract never moved.
    CHECK(kernel.weave_id("t") == lr.id);
    CHECK_FALSE(kernel.accepts(lr.id, "Greet", 1));

    // AND YET the rejected candidate's Greet v1 is still bound in the kernel's
    // dependency registry — observable because a later library declaring Greet
    // v1 with DIFFERENT content now meets the agreement wall, exactly where the
    // negative control above shows it would otherwise have loaded.
    LoadResult conflicted = kernel.load("u", ZEN_SO_ACTIVATES_CONFLICT);
    CHECK_FALSE(conflicted.ok);
    CHECK(conflicted.error.find("load refused") != std::string::npos);
    CHECK(conflicted.error.find("Greet") != std::string::npos);
    CHECK_FALSE(kernel.is_loaded("u"));
    // No half-loaded wreckage, and the incumbent is still serving.
    CHECK(kernel.is_loaded("t"));
    CHECK(kernel.weave_id("t") == lr.id);
}

// ---- R2B-2: the deferring steward, proven where it has to be proven -----------
//
// EVERY CASE BELOW DRIVES A REAL .so. That is not thoroughness for its own sake:
// the whole claim is that an answer right survives the handler that earned it,
// and a native fixture holding the old Bus& would be asking a different (and
// already-answered) question. Across the C seam a loaded weave keeps NOTHING but
// an opaque integer, gets a brand-new Bus on the next delivery, and still has to
// be able to answer — or the capability is not really a capability.

TEST_CASE("R2B-2: a loaded steward answers after its handler returned, and only once") {
    Switchboard bus;
    Kernel kernel(bus);

    LoadResult lr = kernel.load("steward", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(lr.ok, lr.error);
    const WeaveId steward = lr.id;

    // The asker is native only because somebody has to ask. Note what it does NOT
    // supply: no reply_to. Nothing in the ask says where an answer should go, so
    // the answer's arrival is Loom's bookkeeping and cannot be the payload's.
    Registered asker = register_probe(bus, {pong_schema(), tick_schema()});
    int answers = 0;
    bool every_answer_attested = true;
    std::uint64_t heard_correlation = 0;
    asker.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Tick") {
            if (in.payload.get("n")->as_int() == 1) {
                b.send(steward, Message(ping(41), WeaveId{}, /*reply_to=*/WeaveId{},
                                        kAskCorrelation));
            } else {
                b.send(steward, Message(greet("now")));
            }
            return;
        }
        ++answers;
        every_answer_attested = every_answer_attested && in.provenance.answers_ask();
        heard_correlation = in.correlation;
    };

    // Round one: the ask is delivered, the steward's handler runs to completion,
    // and the pump drains. No handler is on the stack when this returns.
    bus.send(asker.id, Message(tick(1)));
    bus.pump();
    Steward s1 = steward_state(bus, steward);
    CHECK(s1.count == 1);    // it really did handle the ask...
    CHECK(s1.deferred == 1); // ...and really did take the answer right with it...
    CHECK(s1.spent == 0);
    CHECK(answers == 0); // ...and answered nothing at all.

    // Round two: an unrelated delivery, a fresh Bus, a new stack frame — and the
    // answer to the ORIGINAL request goes out.
    bus.send(asker.id, Message(tick(2)));
    bus.pump();
    REQUIRE(answers == 1);
    CHECK(asker.weave->handled_names.back() == "Pong");
    CHECK(steward_state(bus, steward).spent == 1);
    // Still an AUTHENTICATED answer: waiting costs the answer none of its
    // standing, and it still wears the original ask's label, which the steward
    // never chose and could not have known to forge.
    CHECK(every_answer_attested);
    CHECK(heard_correlation == kAskCorrelation);

    // Round three: the same completion again. Deferring did not multiply the
    // right; it moved it. One delivered request, one answer, across handlers.
    bus.send(asker.id, Message(tick(2)));
    bus.pump();
    const Steward s3 = steward_state(bus, steward);
    CHECK(s3.count == 3); // POSITIVE CONTROL: the third delivery really happened...
    CHECK(answers == 1);  // ...and still produced no second answer
    CHECK(s3.spent == 1);
}

TEST_CASE("R2B-2: a loaded successor inherits the token and is still refused — reload is a new "
          "incarnation across the seam too") {
    // THE SHARPEST FORM OF THE INHERITANCE QUESTION. The fixture persists its
    // token, so the successor rebuilds a capability from the real number and
    // genuinely believes it holds one. Nothing library-side says no. The board
    // does.
    Switchboard bus;
    Kernel kernel(bus);

    LoadResult lr = kernel.load("steward", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(lr.ok, lr.error);
    const WeaveId steward = lr.id;

    Registered asker = register_probe(bus, {pong_schema(), tick_schema()});
    int answers = 0;
    asker.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Tick") {
            if (in.payload.get("n")->as_int() == 1) {
                b.send(steward, Message(ping(7), WeaveId{}, WeaveId{}, kAskCorrelation));
            } else {
                b.send(steward, Message(greet("now")));
            }
            return;
        }
        ++answers;
    };

    bus.send(asker.id, Message(tick(1)));
    bus.pump();
    const Steward before = steward_state(bus, steward);
    REQUIRE(before.deferred == 1);
    REQUIRE(before.token != 0); // the number really is in its persisted state

    // A reload in place: its own snapshot, transplanted back — exactly what a real
    // reload does — which carries the token forward and bumps the incarnation.
    const ReviveOutcome ro = bus.swap_state(steward, bus.snapshot_bytes(steward));
    REQUIRE(ro.revived);
    const Steward after = steward_state(bus, steward);
    CHECK(after.token == before.token); // the successor holds the same number...
    CHECK(after.deferred == 1);         // ...and believes it is a live capability

    // ...and the completion buys it nothing. Same WeaveId, same role in the world,
    // same token — different incarnation.
    bus.send(asker.id, Message(tick(2)));
    bus.pump();
    const Steward tried = steward_state(bus, steward);
    CHECK(tried.count == 2); // POSITIVE CONTROL: the successor DID handle it...
    CHECK(answers == 0);     // ...and reached nobody
    CHECK(tried.spent == 0);
}

TEST_CASE("R2B-2: the requester dying strands a loaded steward's answer rather than misdelivering "
          "it") {
    Switchboard bus;
    Kernel kernel(bus);

    LoadResult lr = kernel.load("steward", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(lr.ok, lr.error);
    const WeaveId steward = lr.id;

    Registered asker = register_probe(bus, {pong_schema(), tick_schema()});
    int answers = 0;
    asker.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Tick") {
            b.send(steward, Message(ping(9), WeaveId{}, WeaveId{}, kAskCorrelation));
            return;
        }
        ++answers;
    };

    bus.send(asker.id, Message(tick(1)));
    bus.pump();
    REQUIRE(steward_state(bus, steward).deferred == 1);

    // The requester leaves, and somebody else arrives after it.
    bus.unregister_weave(asker.id);
    Registered newcomer = register_probe(bus, {pong_schema(), tick_schema()});

    bus.send(steward, Message(greet("now")));
    bus.pump();
    const Steward after = steward_state(bus, steward);
    CHECK(after.count == 2); // POSITIVE CONTROL: it really tried
    CHECK(answers == 0);
    CHECK(newcomer.weave->handled_names.empty()); // and NOT onto whoever came next
    CHECK(after.spent == 0);
}

TEST_CASE("R2B-2: a second loaded steward holding the same token cannot finish somebody else's "
          "conversation") {
    // The seam's token is a number, and a number is guessable. What stops a thief
    // is not secrecy: it is that the record names its respondent AT AN
    // INCARNATION, so a token only ever spends a right its holder already had.
    Switchboard bus;
    Kernel kernel(bus);

    LoadResult first = kernel.load("steward", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(first.ok, first.error);
    LoadResult second = kernel.load("thief", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(second.ok, second.error);

    // THE INCARNATIONS ARE EQUALIZED ON PURPOSE, and this is the difference
    // between a pin and a coincidence. Injecting a token into the thief takes a
    // reload, which advances ITS incarnation — so without this the incarnation
    // term alone would refuse the theft and the respondent term would never be
    // tested. Reloading the victim first puts both at incarnation 2, leaving
    // respondent identity as the only thing standing between them.
    REQUIRE(bus.swap_state(first.id, bus.snapshot_bytes(first.id)).revived);

    Registered asker = register_probe(bus, {pong_schema(), tick_schema()});
    int answers = 0;
    asker.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Tick") {
            b.send(first.id, Message(ping(3), WeaveId{}, WeaveId{}, kAskCorrelation));
            return;
        }
        ++answers;
    };

    bus.send(asker.id, Message(tick(1)));
    bus.pump();
    const Steward victim = steward_state(bus, first.id);
    REQUIRE(victim.token != 0);

    // Hand the thief the victim's exact token through its own persisted state —
    // the strongest position a thief could ever reach, since the number is
    // otherwise unguessable-by-luck rather than unguessable-in-principle.
    Value stolen(defers_state_schema());
    stolen.set("count", Cell::integer(0));
    stolen.set("deferred", Cell::integer(0));
    stolen.set("spent", Cell::integer(0));
    stolen.set("token", Cell::integer(victim.token));
    REQUIRE(bus.swap_state(second.id, serialize(stolen)).revived);
    REQUIRE(steward_state(bus, second.id).deferred == 1); // it believes it holds one

    bus.send(second.id, Message(greet("mine now")));
    bus.pump();
    const Steward robbed = steward_state(bus, second.id);
    CHECK(robbed.count == 1); // POSITIVE CONTROL: the thief really did try
    CHECK(answers == 0);
    CHECK(robbed.spent == 0);

    // And the rightful holder is unharmed: the conversation is still open and
    // still its own to finish.
    bus.send(first.id, Message(greet("now")));
    bus.pump();
    CHECK(answers == 1);
    CHECK(steward_state(bus, first.id).spent == 1);
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
