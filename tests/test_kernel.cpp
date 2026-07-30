#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/host/lifecycle_wiring.hpp>
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
    REQUIRE_FALSE(recorder.weave->handled_values.empty()); // guard the index below
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

// ---- R2B-3: the candidate waits outside the world -----------------------------
//
// Today's SwapWeave says its own window out loud: it issues UnloadRole then
// LoadLibrary, and between those two deliveries the role is held by nobody. The
// incumbent is already gone when the successor turns out to be broken.
//
// A prepared replacement inverts that. The candidate is loaded, constructed and
// made ready while the incumbent is still fully the incumbent — and one operation
// makes the swap visible:
//
//     The incumbent remains fully authoritative until one atomic commit makes
//     the prepared candidate live.
//
// The primitive underneath is a SEAL on the weave record. A sealed weave is real
// (it loaded from a real artifact, it has contracts, it can be revived and can
// answer) and is not a participant: publications skip it, ordinary sends cannot
// find it, it cannot address a role at all, and it may speak to exactly one weave
// — the coordinator preparing it.
//
//     The candidate may converse inside preparation before it may speak
//     inside the world.

TEST_CASE("R2B-3: a sealed candidate is loaded from a real artifact and is NOT in the world") {
    Switchboard bus;
    Kernel kernel(bus);

    // A coordinator: an ordinary weave, and the only one the candidate may talk to.
    Registered coordinator = register_probe(bus, {pong_schema(), tick_schema()});
    Registered bystander = register_probe(bus, {ping_schema(), pong_schema()});

    // The incumbent holds the role and keeps it throughout.
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE_MESSAGE(incumbent.ok, incumbent.error);

    // The candidate loads from the same real artifact — every ordinary load step,
    // then sealed.
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE_MESSAGE(candidate.ok, candidate.error);
    CHECK(bus.sealed(candidate.id));
    CHECK_FALSE(bus.sealed(incumbent.id));
    CHECK(bus.alive(candidate.id)); // real, constructed, and alive — just not present
    CHECK(kernel.role_of("cand").empty());
    CHECK(kernel.role_of("live") == "worker");

    // IT IS NOT IN THE WORLD, one routing path at a time.
    //
    // (1) A publication reaches every living participant that accepts the shape —
    //     and not the candidate.
    const std::size_t heard = bus.publish(Message(ping(1)));
    bus.pump();
    CHECK(heard == 2); // the incumbent and the bystander; NOT the candidate
    CHECK(live_count(bus, candidate.id) == 0);
    CHECK(live_count(bus, incumbent.id) == 1);

    // (2) An ordinary weave that somehow knows the id cannot reach it, and cannot
    //     tell it apart from an id that was never registered.
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send_as(bystander.id, candidate.id, Message(ping(2)));
    bus.send_as(bystander.id, WeaveId{999999}, Message(ping(3)));
    bus.pump();
    REQUIRE(tap.size() == 2);
    CHECK(tap[0].kind == EventKind::Refused);
    CHECK(tap[0].reason == RefusalReason::NoSuchTarget);
    CHECK(tap[1].reason == tap[0].reason); // indistinguishable, deliberately
    CHECK(live_count(bus, candidate.id) == 0);

    // (3) The role still points at the incumbent, and role traffic still lands there.
    bus.send_to_role("worker", Message(ping(4)));
    bus.pump();
    CHECK(live_count(bus, incumbent.id) == 2);
    CHECK(live_count(bus, candidate.id) == 0);

    // (4) ...and its coordinator CAN reach it. That is the whole point of a seal
    //     rather than a quarantine: preparation is a conversation.
    bus.send_as(coordinator.id, candidate.id, Message(ping(5)));
    bus.pump();
    CHECK(live_count(bus, candidate.id) == 1);
}

TEST_CASE("R2B-3: a sealed candidate cannot speak into the world, and every attempt is named") {
    // THE HOSTILE CANDIDATE. Its grant is the permissive one every loaded weave
    // gets — the prepared artifact is the artifact that becomes live, so its real
    // contract is not stripped — and it still cannot reach anything.
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered victim = register_probe(bus, {pong_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE_MESSAGE(candidate.ok, candidate.error);

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    // Speaking AS the candidate, by every door a weave has: a direct domain
    // message, a role-addressed message, and a publication.
    bus.send_as(candidate.id, victim.id, Message(pong(1)));
    bus.send_as_to_role(candidate.id, "worker", Message(ping(2)));
    const std::size_t published = bus.publish_as(candidate.id, Message(pong(3)));
    bus.pump();

    CHECK(published == 0);
    CHECK(victim.weave->handled_names.empty());
    CHECK(live_count(bus, incumbent.id) == 0); // the role send reached nobody
    std::size_t sealed_refusals = 0;
    for (const TapRecord& r : tap) {
        if (r.kind == EventKind::Refused && r.reason == RefusalReason::SealedSpeech) {
            ++sealed_refusals;
        }
    }
    CHECK(sealed_refusals == 3); // all three, each named as what it was

    // And the one thing it MAY do still works.
    bus.send_as(candidate.id, coordinator.id, Message(pong(4)));
    bus.pump();
    REQUIRE(coordinator.weave->handled_names.size() == 1);
    CHECK(coordinator.weave->handled_names[0] == "Pong");
}

TEST_CASE("R2B-3: commit is ONE visible change — no observer sees a gap, two holders, or a "
          "role pointing at a sealed weave") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE(candidate.ok);

    // A watcher that samples the world's topology on EVERY delivery. If commit were
    // several ordinary steps, some delivery would land between them and see one of
    // the forbidden intermediate states.
    struct Sample {
        bool role_held = false;
        bool role_is_incumbent = false;
        bool role_is_candidate = false;
        bool candidate_sealed = false;
    };
    std::vector<Sample> samples;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind != EventKind::Delivered && e.kind != EventKind::Refused) {
            return;
        }
        const WeaveId holder = bus.role_holder("worker");
        samples.push_back(Sample{holder.valid(), holder == incumbent.id,
                                 holder == candidate.id, bus.sealed(candidate.id)});
    });

    // Traffic before, the commit, traffic after — all in one drain.
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    REQUIRE(kernel.commit_candidate("live", "cand", "worker"));
    bus.send_to_role("worker", Message(ping(2)));
    bus.pump();

    // Four samples, not two: each Ping is delivered AND draws a Pong whose
    // reply_to is nobody, so each round produces a delivery and a refusal. Both
    // are ordinary deliveries and both sample the topology, which is what makes
    // this a real watcher rather than a bookend.
    REQUIRE(samples.size() == 4);
    // Every sample saw exactly one holder, and never a sealed one.
    for (const Sample& s : samples) {
        CHECK(s.role_held);
        const bool two_holders = s.role_is_incumbent && s.role_is_candidate;
        const bool unready_holder = s.role_is_candidate && s.candidate_sealed;
        CHECK_FALSE(two_holders);
        CHECK_FALSE(unready_holder);
    }
    CHECK(samples[0].role_is_incumbent); // before the commit
    CHECK(samples[1].role_is_incumbent);
    CHECK(samples[2].role_is_candidate); // after it, and nothing in between
    CHECK(samples[3].role_is_candidate);
    CHECK_FALSE(samples[3].candidate_sealed);

    // The traffic went where the topology said it would, on both sides of the line.
    CHECK(live_count(bus, incumbent.id) == 1);
    CHECK(live_count(bus, candidate.id) == 1);

    // ...and the incumbent is still alive and addressable by id — it retired from
    // the ROLE, not from existence. Retirement is a separate act.
    CHECK(bus.alive(incumbent.id));
    CHECK(kernel.role_of("live").empty());
    CHECK(kernel.role_of("cand") == "worker");
}

TEST_CASE("R2B-3: a refused commit changes NOTHING — it is observationally identical to never "
          "having tried") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE(candidate.ok);

    // Wrong role; wrong incumbent; and a candidate that was never sealed.
    CHECK_FALSE(kernel.commit_candidate("live", "cand", "not-a-role"));
    CHECK_FALSE(kernel.commit_candidate("cand", "live", "worker")); // roles reversed
    CHECK_FALSE(kernel.commit_candidate("live", "nope", "worker"));

    CHECK(bus.role_holder("worker") == incumbent.id);
    CHECK(bus.sealed(candidate.id));
    CHECK(kernel.role_of("live") == "worker");
    CHECK(kernel.role_of("cand").empty());

    // The incumbent never stopped serving.
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    CHECK(live_count(bus, incumbent.id) == 1);
    CHECK(live_count(bus, candidate.id) == 0);
}

TEST_CASE("R2B-3: an artifact that cannot load never becomes a candidate, and the incumbent "
          "does not notice") {
    // THE FAILURE SIDE IS THE POINT. Under today's SwapWeave the incumbent is
    // already unloaded when a broken successor is discovered; here the discovery
    // happens with the incumbent untouched, because nothing has been touched yet.
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);

    // A stale-ABI artifact: a real, complete refusal at the artifact level.
    LoadResult stale = kernel.load_candidate("cand", ZEN_SO_STALEABI, coordinator.id);
    CHECK_FALSE(stale.ok);
    CHECK(stale.error.find("abi_version") != std::string::npos);
    CHECK_FALSE(kernel.is_loaded("cand"));

    // A malformed-snapshot artifact: refused at the manifest/gate level.
    LoadResult bad = kernel.load_candidate("cand2", ZEN_SO_BADSNAP, coordinator.id);
    CHECK_FALSE(bad.ok);
    CHECK_FALSE(kernel.is_loaded("cand2"));

    // The incumbent is exactly where it was, still holding the role and serving.
    CHECK(bus.role_holder("worker") == incumbent.id);
    CHECK(bus.alive(incumbent.id));
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    CHECK(live_count(bus, incumbent.id) == 1);
    CHECK(bus.list_weaves().size() == 2); // coordinator + incumbent; no wreckage
}

TEST_CASE("R2B-3: abandoning a prepared candidate leaves the world as it was, and its speech "
          "cannot leak afterwards") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered victim = register_probe(bus, {pong_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE(candidate.ok);
    const WeaveId cand_id = candidate.id;

    // The candidate speaks — into the queue — and is then abandoned before the
    // pump. Its queued speech must not outlive the transaction.
    bus.send_as(cand_id, coordinator.id, Message(pong(1)));
    REQUIRE(bus.pending() == 1);
    REQUIRE(kernel.unload("cand"));
    bus.pump();

    CHECK(coordinator.weave->handled_names.empty()); // R2B-2b: its life ended
    CHECK_FALSE(bus.alive(cand_id));
    CHECK(bus.role_holder("worker") == incumbent.id);
    bus.send_to_role("worker", Message(ping(2)));
    bus.pump();
    CHECK(live_count(bus, incumbent.id) == 1);
    CHECK(victim.weave->handled_names.empty());
}

// ---- R2B-3b: the transaction's identity and the admission boundary -------------
//
// R2B-3a bound a seal to a coordinator's WeaveId, which was enough to prove
// isolation and is not enough to own a transaction. Every other authority in this
// codebase already carries three facts, and for the same reason: a coordinator
// that dies and revives, or whose code is replaced, is a different participant at
// the same address. A preparation is a conversation, so it belongs to a life.
//
// And commit has to account for the incumbent, not merely the role — otherwise a
// "replaced" service is still publicly direct-addressable, which is two live
// services with one of them pretending to be retired.

TEST_CASE("R2B-3b: a coordinator successor inherits neither the candidate nor its conversation") {
    bool by_death = false;
    SUBCASE("the coordinator dies and is revived") { by_death = true; }
    SUBCASE("the coordinator's code is replaced while alive") { by_death = false; }

    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema(), tick_schema()});
    LoadResult incumbent = kernel.load("live", ZEN_SO_WEAVE, "worker");
    REQUIRE(incumbent.ok);
    LoadResult candidate = kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id);
    REQUIRE_MESSAGE(candidate.ok, candidate.error);

    const CandidateOwner owner = bus.candidate_owner(candidate.id);
    CHECK(owner.who == coordinator.id);
    CHECK(owner.life == 1);
    CHECK(owner.incarnation == 1);

    // Before: the coordinator can converse with its candidate.
    bus.send_as(coordinator.id, candidate.id, Message(ping(1)));
    bus.pump();
    REQUIRE(live_count(bus, candidate.id) == 1);

    // The coordinator becomes a different participant at the same address.
    const std::string state = bus.snapshot_bytes(coordinator.id);
    if (by_death) {
        bus.kill(coordinator.id);
        REQUIRE(bus.reload(coordinator.id, state).revived);
    } else {
        REQUIRE(bus.swap_state(coordinator.id, state).revived);
    }

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    // The successor cannot reach the candidate — and cannot tell it from an id
    // that was never registered.
    bus.send_as(coordinator.id, candidate.id, Message(ping(2)));
    // ...and the candidate cannot reach the successor either: its one permitted
    // correspondent was a life, not an address.
    bus.send_as(candidate.id, coordinator.id, Message(pong(3)));
    bus.pump();

    CHECK(live_count(bus, candidate.id) == 1); // unchanged: nothing got through
    REQUIRE(tap.size() == 2);
    CHECK(tap[0].reason == RefusalReason::NoSuchTarget);  // inbound, no oracle
    CHECK(tap[1].reason == RefusalReason::SealedSpeech);  // outbound, named
    CHECK(coordinator.weave->handled_names.empty());

    // The incumbent never noticed any of it.
    CHECK(bus.role_holder("worker") == incumbent.id);
    bus.send_to_role("worker", Message(ping(4)));
    bus.pump();
    CHECK(live_count(bus, incumbent.id) == 1);
}

TEST_CASE("R2B-3b: at admission the candidate's FIRST live delivery is its activation, even "
          "though production was queued for the role before commit") {
    // THE ORDERING THE QUEUE RESISTED. Role resolution is a delivery-time decision,
    // so a role-addressed message enqueued before the commit resolves to whoever
    // holds the role when it is finally dispatched — the candidate. Appending the
    // activation at the tail would let ordinary production reach a weave that has
    // not yet been told it is alive.
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered incumbent = register_probe(bus, {ping_schema()});
    // The candidate is a probe here because the claim is about ORDER, and a probe
    // records the exact sequence it was handed. (That a real .so can be a sealed
    // candidate is R2B-3a's ground, pinned there against actual artifacts.)
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    const WeaveId inc_id = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    (void)incumbent;
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    // Production aimed at the ROLE, queued while the incumbent still holds it.
    bus.send_to_role("worker", Message(ping(1)));
    bus.send_to_role("worker", Message(ping(2)));
    REQUIRE(bus.pending() == 2);

    loom::Activated fact{7};
    REQUIRE(bus.admit_candidate(candidate, inc_id, "worker",
                                host_lifecycle_authority(bus),
                                Message(to_value(fact)), 7));

    bus.pump();

    // The candidate saw its activation FIRST, then the production that was already
    // waiting. Nothing was dropped to achieve that.
    REQUIRE(cand_raw->handled_names.size() == 3);
    CHECK(cand_raw->handled_names[0] == std::string(loom::Activated::zen_name));
    CHECK(cand_raw->handled_names[1] == "Ping");
    CHECK(cand_raw->handled_names[2] == "Ping");
}

TEST_CASE("R2B-3b: after admission the incumbent is sealed for retirement — no production of any "
          "kind, and the coordinator can still reach it") {
    // Moving the role alone would leave the incumbent publicly direct-addressable:
    // a second live service that merely lost its name.
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered outsider = register_probe(bus, {pong_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    ProbeWeave* inc_raw = static_cast<ProbeWeave*>(bus.weave(incumbent));
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    REQUIRE(inc_raw->handled_names.size() == 1);

    loom::Activated fact{3};
    REQUIRE(bus.admit_candidate(candidate, incumbent, "worker",
                                host_lifecycle_authority(bus),
                                Message(to_value(fact)), 3));
    CHECK(bus.sealed(incumbent));
    CHECK_FALSE(bus.sealed(candidate));

    // Production by role AND by id both miss the incumbent now.
    bus.send_to_role("worker", Message(ping(2)));
    bus.send_as(outsider.id, incumbent, Message(ping(3)));
    bus.pump();
    CHECK(inc_raw->handled_names.size() == 1); // still just the pre-commit one
    CHECK(cand_raw->handled_names.size() == 2); // activation + the role message

    // ...and the private retirement conversation still reaches it.
    bus.send_as(coordinator.id, incumbent, Message(ping(4)));
    bus.pump();
    CHECK(inc_raw->handled_names.size() == 2);
}

TEST_CASE("R2B-3b: admission refuses without Loom's own authority, and a refusal changes nothing") {
    Switchboard bus;
    Switchboard decoy; // a real board, and a real authority — issued elsewhere
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    const WeaveId candidate = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    loom::Activated fact{1};
    // Another Loom's authority has no standing here (R2B-1b), and topology is
    // exactly the kind of thing it must not move.
    CHECK_FALSE(bus.admit_candidate(candidate, incumbent, "worker",
                                    host_lifecycle_authority(decoy),
                                    Message(to_value(fact)), 1));
    // Precondition drift: the role is not held by the named incumbent.
    CHECK_FALSE(bus.admit_candidate(candidate, coordinator.id, "worker",
                                    host_lifecycle_authority(bus),
                                    Message(to_value(fact)), 1));
    // An unsealed "candidate" is not a candidate.
    CHECK_FALSE(bus.admit_candidate(coordinator.id, incumbent, "worker",
                                    host_lifecycle_authority(bus),
                                    Message(to_value(fact)), 1));

    // Nothing moved, and nothing was queued.
    CHECK(bus.role_holder("worker") == incumbent);
    CHECK(bus.sealed(candidate));
    CHECK_FALSE(bus.sealed(incumbent));
    CHECK(bus.pending() == 0);
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    CHECK(static_cast<ProbeWeave*>(bus.weave(incumbent))->handled_names.size() == 1);
}

// ---- R2B-3b-1a (errand A): admission recognizes its owner ----------------------
//
// The candidate's private conversation already checked the exact coordinator life
// and incarnation on every message. Admission did not — which left the strongest
// act in the system, moving production topology, resting on a stale fact. A
// trusted host caller holding a perfectly good lifecycle authority could admit a
// candidate whose coordinator had died and revived, been reloaded into new code,
// or been removed entirely.
//
//     A candidate may enter the world only while the exact coordinator life and
//     incarnation that sealed it still owns the preparation.

namespace {

/// The shape every errand-A case builds: a coordinator, an incumbent holding the
/// role, and a sealed candidate belonging to that coordinator.
struct Prepared {
    Registered coordinator;
    WeaveId incumbent{};
    WeaveId candidate{};
    ProbeWeave* incumbent_raw = nullptr;
    ProbeWeave* candidate_raw = nullptr;
};

Prepared prepare(Switchboard& bus) {
    Prepared p{register_probe(bus, {pong_schema(), tick_schema()})};
    auto inc = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{ping_schema()});
    p.incumbent_raw = inc.get();
    p.incumbent = bus.register_weave(std::move(inc), Grant{}.allow_any(), "worker");
    auto cand = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    p.candidate_raw = cand.get();
    p.candidate = bus.register_weave(std::move(cand), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(p.candidate, p.coordinator.id));
    return p;
}

/// Everything an ordinary observer could notice about the replacement, sampled so
/// "nothing changed" is measured rather than asserted.
struct Topology {
    WeaveId holder{};
    bool candidate_sealed = false;
    bool incumbent_sealed = false;
    std::size_t pending = 0;
    std::size_t candidate_deliveries = 0;

    static Topology of(Switchboard& bus, const Prepared& p) {
        return Topology{bus.role_holder("worker"), bus.sealed(p.candidate),
                        bus.sealed(p.incumbent), bus.pending(),
                        p.candidate_raw->handled_names.size()};
    }
    friend bool operator==(const Topology& a, const Topology& b) {
        return a.holder == b.holder && a.candidate_sealed == b.candidate_sealed &&
               a.incumbent_sealed == b.incumbent_sealed && a.pending == b.pending &&
               a.candidate_deliveries == b.candidate_deliveries;
    }
};

} // namespace

TEST_CASE("R2B-3b-1a: a coordinator successor cannot admit its predecessor's candidate, and the "
          "refusal changes nothing") {
    int route = 0;
    SUBCASE("the coordinator died and was revived") { route = 0; }
    SUBCASE("the coordinator's code was replaced while alive") { route = 1; }
    SUBCASE("the coordinator was permanently removed") { route = 2; }

    Switchboard bus;
    Prepared p = prepare(bus);
    const Topology before = Topology::of(bus, p);

    const std::string state = bus.snapshot_bytes(p.coordinator.id);
    if (route == 0) {
        bus.kill(p.coordinator.id);
        REQUIRE(bus.reload(p.coordinator.id, state).revived);
    } else if (route == 1) {
        REQUIRE(bus.swap_state(p.coordinator.id, state).revived);
    } else {
        bus.unregister_weave(p.coordinator.id);
    }

    // A trusted host caller, with this Loom's own genuine lifecycle authority.
    loom::Activated fact{9};
    const AdmitResult r = bus.admit_candidate(p.candidate, p.incumbent, "worker",
                                              host_lifecycle_authority(bus),
                                              Message(to_value(fact)), 9);
    CHECK_FALSE(r.ok);
    CHECK(r.why == AdmitRefusal::OwnerChanged); // named, not a bare false

    // NOTHING MOVED — sampled, not assumed. In particular no activation was
    // inserted, which is what `pending` catches.
    CHECK(Topology::of(bus, p) == before);
    CHECK(bus.role_holder("worker") == p.incumbent);
    CHECK(bus.sealed(p.candidate));
    CHECK_FALSE(bus.sealed(p.incumbent));

    // ...and the incumbent is still the service.
    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    CHECK(p.incumbent_raw->handled_names.size() == 1);
    CHECK(p.candidate_raw->handled_names.empty());
}

TEST_CASE("R2B-3b-1a: the unchanged exact coordinator still admits — the positive control the "
          "refusals above would otherwise be meaningless without") {
    Switchboard bus;
    Prepared p = prepare(bus);

    loom::Activated fact{4};
    const AdmitResult r = bus.admit_candidate(p.candidate, p.incumbent, "worker",
                                              host_lifecycle_authority(bus),
                                              Message(to_value(fact)), 4);
    REQUIRE(r.ok);
    CHECK(r.why == AdmitRefusal::None);
    CHECK(bus.role_holder("worker") == p.candidate);
    CHECK_FALSE(bus.sealed(p.candidate));
    CHECK(bus.sealed(p.incumbent));

    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    REQUIRE(p.candidate_raw->handled_names.size() == 2);
    CHECK(p.candidate_raw->handled_names[0] == std::string(loom::Activated::zen_name));
    CHECK(p.candidate_raw->handled_names[1] == "Ping");
}

TEST_CASE("R2B-3b-1a: sealing refuses a dead coordinator and refuses to reseal") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered second = register_probe(bus, {pong_schema()});
    const WeaveId candidate = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any());

    // A dead coordinator cannot own a preparation: it cannot converse, so the
    // candidate would be sealed to a correspondent that can never answer.
    bus.kill(coordinator.id);
    CHECK_FALSE(bus.seal_weave(candidate, coordinator.id));
    CHECK_FALSE(bus.sealed(candidate));

    REQUIRE(bus.reload(coordinator.id, bus.snapshot_bytes(coordinator.id)).revived);
    REQUIRE(bus.seal_weave(candidate, coordinator.id));
    const CandidateOwner owner = bus.candidate_owner(candidate);
    CHECK(owner.who == coordinator.id);
    CHECK(owner.life == 2); // the revived life, and the seal knows which one

    // RESEALING IS NOT A TRANSFER. Silently changing owners would hand a prepared
    // candidate to somebody else's transaction; transfer semantics are deliberately
    // not part of this errand, so the second attempt simply fails.
    CHECK_FALSE(bus.seal_weave(candidate, second.id));
    CHECK(bus.candidate_owner(candidate) == owner);

    // A candidate that holds a role was never sealable, and still is not.
    const WeaveId roled = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "taken");
    CHECK_FALSE(bus.seal_weave(roled, coordinator.id));
}

TEST_CASE("R2B-3b-1a: every other admission refusal is named, and none of them touch topology") {
    Switchboard bus;
    Switchboard decoy;
    Prepared p = prepare(bus);
    const Topology before = Topology::of(bus, p);
    loom::Activated fact{1};

    const AdmitResult foreign =
        bus.admit_candidate(p.candidate, p.incumbent, "worker",
                            host_lifecycle_authority(decoy), Message(to_value(fact)), 1);
    CHECK(foreign.why == AdmitRefusal::ForeignAuthority);

    const AdmitResult not_a_candidate =
        bus.admit_candidate(p.coordinator.id, p.incumbent, "worker",
                            host_lifecycle_authority(bus), Message(to_value(fact)), 1);
    CHECK(not_a_candidate.why == AdmitRefusal::NotACandidate);

    const AdmitResult wrong_role =
        bus.admit_candidate(p.candidate, p.incumbent, "no-such-role",
                            host_lifecycle_authority(bus), Message(to_value(fact)), 1);
    CHECK(wrong_role.why == AdmitRefusal::RoleNotHeld);

    const AdmitResult wrong_incumbent =
        bus.admit_candidate(p.candidate, p.coordinator.id, "worker",
                            host_lifecycle_authority(bus), Message(to_value(fact)), 1);
    CHECK(wrong_incumbent.why == AdmitRefusal::RoleNotHeld);

    CHECK(Topology::of(bus, p) == before);
}

// ---- R2B-3b-1a (errand B): the answer means the same on both sides -------------
//
// A native weave writes `mail.answer(reply)` and Loom enqueues an authenticated
// answer. A dynamically loaded weave wrote the same line, reached `HostApiBus`,
// and got the base class's do-nothing default: no answer, no refusal, no bus
// event. The same public word meant two different things depending on which side
// of the library seam it was spoken — and the difference was SILENT, which is the
// worst way for it to be different.
//
//     A public delivery operation must mean the same thing on both sides of the
//     dynamic-library seam, or fail loudly before user code mistakes silence for
//     success.
//
// ABI v4 pays for it with one narrowly typed door. No authority crosses: the
// library asks for the public operation, and the host decides whether this
// delivery earned an answer, who receives it, and what it is labelled.

namespace {

/// The native half of the parity fixture: the same ask, the same public call.
class NativeAnswerer final : public Weave {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {ping_schema()};
    }
    void handle(const Message& in, Bus& bus) override {
        ++n_;
        Value reply(pong_schema());
        reply.set("seq", Cell::integer(in.payload.get("seq")->as_int()));
        answered = bus.answer(Message(reply)).valid();
    }
    Value snapshot() const override {
        Value v(counter_schema());
        v.set("count", Cell::integer(n_));
        return v;
    }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(8));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value& v) override { n_ = v.get("count")->as_int(); }

    bool answered = false;

private:
    std::int64_t n_ = 0;
};

/// What an asker heard, and Loom's verdict on it.
struct Heard {
    int count = 0;
    bool all_attested = true;
    std::uint64_t correlation = 0;
    std::int64_t seq = -1;

    void arm(ProbeWeave& probe) {
        probe.on_handle = [this](const Message& in, Bus&, ProbeWeave&) {
            ++count;
            all_attested = all_attested && in.provenance.answers_ask();
            correlation = in.correlation;
            const Cell* c = in.payload.get("seq");
            seq = c == nullptr ? -1 : c->as_int();
        };
    }
};

constexpr std::uint64_t kAskCorr = 0xA5Cu;

/// The dynamic fixture's own window (Counter v5).
struct AnswerState {
    std::int64_t count = 0;
    std::int64_t answered = 0;
    std::int64_t second = 0;
};

AnswerState answer_state(Switchboard& bus, WeaveId id) {
    static const auto s = SchemaBuilder("Counter", 5)
                              .field("count", Kind::Int)
                              .field("answered", Kind::Int)
                              .field("second", Kind::Int)
                              .build();
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, s);
    REQUIRE(a.ok());
    return AnswerState{a.value().get("count")->as_int(), a.value().get("answered")->as_int(),
                       a.value().get("second")->as_int()};
}

} // namespace

TEST_CASE("R2B-3b-1a: mail.answer() means the same thing natively and dynamically") {
    // THE PARITY PROOF. One ask each, the same public call, and the two results
    // compared field by field rather than each asserted against a wish.
    Switchboard bus;
    Kernel kernel(bus);

    Registered native_asker = register_probe(bus, {pong_schema(), tick_schema()});
    Registered dynamic_asker = register_probe(bus, {pong_schema(), tick_schema()});
    Heard native_heard;
    Heard dynamic_heard;
    native_heard.arm(*native_asker.weave);
    dynamic_heard.arm(*dynamic_asker.weave);

    auto nat = std::make_unique<NativeAnswerer>();
    NativeAnswerer* nat_raw = nat.get();
    const WeaveId native_responder = bus.register_weave(std::move(nat), Grant{}.allow_any());
    LoadResult dyn = kernel.load("dyn", ZEN_SO_ANSWERS);
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    bus.send_as(native_asker.id, native_responder, Message(ping(11), native_asker.id,
                                                           WeaveId{}, kAskCorr));
    bus.send_as(dynamic_asker.id, dyn.id, Message(ping(22), dynamic_asker.id, WeaveId{},
                                                  kAskCorr));
    bus.pump();

    // Delivered: exactly one each.
    CHECK(native_heard.count == 1);
    CHECK(dynamic_heard.count == 1);
    // Authentic: Loom's word on both.
    CHECK(native_heard.all_attested);
    CHECK(dynamic_heard.all_attested);
    // The original correlation, which neither responder chose.
    CHECK(native_heard.correlation == kAskCorr);
    CHECK(dynamic_heard.correlation == kAskCorr);
    // ...and each answered its own asker with its own payload.
    CHECK(native_heard.seq == 11);
    CHECK(dynamic_heard.seq == 22);
    // Both responders were told their answer went out — the dynamic one used to be
    // told nothing at all.
    CHECK(nat_raw->answered);
    CHECK(answer_state(bus, dyn.id).answered == 1);
}

TEST_CASE("R2B-3b-1a: the dynamic answer is authentic when the ask arrives BY ROLE, and a "
          "forged ordinary reply is not") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered asker = register_probe(bus, {pong_schema()});
    Heard heard;
    heard.arm(*asker.weave);
    LoadResult dyn = kernel.load("dyn", ZEN_SO_ANSWERS, "answerer");
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    bus.send_as_to_role(asker.id, "answerer",
                        Message(ping(5), asker.id, WeaveId{}, kAskCorr));
    bus.pump();
    REQUIRE(heard.count == 1);
    CHECK(heard.all_attested);
    CHECK(heard.correlation == kAskCorr);

    // A rogue that knows the shape and the correlation sends the same bytes the
    // ordinary way. Provenance is a delivery fact, not a payload (R2B-1).
    Registered rogue = register_probe(bus, {tick_schema()});
    bus.send_as(rogue.id, asker.id, Message(pong(5), rogue.id, WeaveId{}, kAskCorr));
    bus.pump();
    REQUIRE(heard.count == 2);
    CHECK_FALSE(heard.all_attested); // the forgery is the one that is not attested
}

TEST_CASE("R2B-3b-1a: one delivery authorizes one dynamic answer, and the second is refused "
          "rather than silently dropped") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered asker = register_probe(bus, {pong_schema()});
    Heard heard;
    heard.arm(*asker.weave);
    LoadResult dyn = kernel.load("dyn", ZEN_SO_ANSWERS_TWICE);
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    bus.send_as(asker.id, dyn.id, Message(ping(1), asker.id, WeaveId{}, kAskCorr));
    bus.pump();

    CHECK(heard.count == 1); // exactly one answer exists
    const AnswerState st = answer_state(bus, dyn.id);
    CHECK(st.answered == 1); // the first was accepted...
    CHECK(st.second == 0);   // ...and the second was REFUSED, and it was told so
}

TEST_CASE("R2B-3b-1a: a dynamic immediate answer consumes no deferred-answer capacity — proven "
          "with the registry already FULL") {
    // The defer-then-spend shortcut would have been the easy implementation, and
    // this is why it was rejected: it would borrow a slot from a bounded registry
    // for a conversation that never needed one, and at the bound it would begin
    // failing as `Exhausted` for a reason the caller could do nothing about.
    //
    // AN EARLIER VERSION OF THIS CASE PROVED NOTHING: it answered far past the
    // bound but pumped after every ask, so each borrowed slot was returned before
    // the next was taken and the registry never actually filled. The mutation that
    // routes the immediate answer through defer-then-spend stayed green. The
    // instrument has to HOLD the registry full.
    Switchboard bus;
    Kernel kernel(bus);
    Registered asker = register_probe(bus, {pong_schema()});
    Heard heard;
    heard.arm(*asker.weave);
    LoadResult dyn = kernel.load("dyn", ZEN_SO_ANSWERS);
    REQUIRE(dyn.ok);

    // Fill every slot with conversations that are deferred and never answered: a
    // dynamic steward that keeps its capability and is simply never completed.
    LoadResult hoarder = kernel.load("hoard", ZEN_SO_DEFERS);
    REQUIRE_MESSAGE(hoarder.ok, hoarder.error);
    Registered nagger = register_probe(bus, {pong_schema()});
    for (std::size_t i = 0; i < Switchboard::kMaxDeferredAnswers; ++i) {
        bus.send_as(nagger.id, hoarder.id, Message(ping(1), nagger.id, WeaveId{}, 1));
        bus.pump();
    }

    // The registry is now full, and says so: one more deferral is refused.
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) {
        if (e.kind == EventKind::Refused) {
            tap.push_back(to_record(e));
        }
    });
    bus.send_as(nagger.id, hoarder.id, Message(ping(1), nagger.id, WeaveId{}, 1));
    bus.pump();
    std::size_t exhausted = 0;
    for (const TapRecord& r : tap) {
        if (r.reason == RefusalReason::Exhausted) {
            ++exhausted;
        }
    }
    REQUIRE(exhausted == 1); // the positive control: the bound is genuinely reached

    // ...and with not one slot to spare, the dynamic immediate answer works
    // exactly as a native one does. It never wanted a slot.
    tap.clear();
    bus.send_as(asker.id, dyn.id, Message(ping(7), asker.id, WeaveId{}, kAskCorr));
    bus.pump();
    CHECK(heard.count == 1);
    CHECK(heard.all_attested);
    CHECK(heard.correlation == kAskCorr);
    CHECK(heard.seq == 7);
    CHECK(tap.empty()); // no Exhausted, no refusal of any kind
}

TEST_CASE("R2B-3b-1a: a dynamic answer obeys the requester and sender lifecycle laws exactly as "
          "a native one does") {
    bool requester_changes = false;
    SUBCASE("the requester dies and revives before the answer is delivered") {
        requester_changes = true;
    }
    SUBCASE("the responder dies after queueing the answer") { requester_changes = false; }

    Switchboard bus;
    Kernel kernel(bus);
    Registered asker = register_probe(bus, {pong_schema()});
    Heard heard;
    heard.arm(*asker.weave);
    LoadResult dyn = kernel.load("dyn", ZEN_SO_ANSWERS);
    REQUIRE(dyn.ok);

    // Stop the pump the moment the ASK is delivered, so the answer the dynamic
    // weave produced is queued and undelivered.
    const ObserverId stopper = bus.add_observer([&bus](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == "Ping") {
            bus.stop();
        }
    });
    bus.send_as(asker.id, dyn.id, Message(ping(3), asker.id, WeaveId{}, kAskCorr));
    bus.pump();
    bus.remove_observer(stopper);
    REQUIRE(bus.pending() == 1);
    REQUIRE(heard.count == 0);

    if (requester_changes) {
        // R2B-2c: the answer belongs to the life that asked.
        const std::string state = bus.snapshot_bytes(asker.id);
        bus.kill(asker.id);
        REQUIRE(bus.reload(asker.id, state).revived);
    } else {
        // R2B-2b: queued speech belongs to the life that authored it.
        bus.kill(dyn.id);
    }
    bus.pump();
    CHECK(heard.count == 0); // neither law is weakened by crossing the seam
}

TEST_CASE("R2B-3b-1a: an artifact built against the previous ABI is refused, naming both "
          "versions") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult stale = kernel.load("stale", ZEN_SO_STALEABI);
    CHECK_FALSE(stale.ok);
    CHECK(stale.error.find("abi_version") != std::string::npos);
    CHECK(stale.error.find(std::to_string(ZEN_ABI_VERSION - 1u)) != std::string::npos);
    CHECK(stale.error.find(std::to_string(ZEN_ABI_VERSION)) != std::string::npos);
    CHECK_FALSE(kernel.is_loaded("stale"));
    // No silent fallback: nothing of it reached the bus.
    CHECK(bus.list_weaves().empty());
}

// ---- R2B-3b-2: the transaction remembers ---------------------------------------
//
// The door already knew how to open. What it did not have was a memory of who was
// allowed to turn the handle — so the strongest act in the system could be driven
// by a coordinator that had died, an operator that had been replaced, or against
// an incumbent that had drifted away underneath it.
//
//     A replacement transaction belongs to exact lives, advances through one
//     finite state machine, and either commits once or disappears without
//     disturbing the incumbent.
//
// The registry lives in the Switchboard for one decisive reason: `kill` announces
// Died and `swap_state` announces Revived, but `unregister_weave` announces
// NOTHING. A registry watching from outside would silently miss permanent
// removal — the most complete invalidation there is.

namespace {

/// A production service that answers a version query, so "the incumbent never
/// stopped being the service" is something a test can ASK rather than infer.
class VersionedService final : public Weave {
public:
    explicit VersionedService(std::string version) : version_(std::move(version)) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {ping_schema(), schema_of<loom::Activated>()};
    }
    void handle(const Message& in, Bus& bus) override {
        if (in.payload.schema().name() == std::string_view(loom::Activated::zen_name)) {
            ++activations;
            return;
        }
        ++served;
        Value out(greet_schema());
        out.set("msg", Cell::text(version_));
        bus.send(in.reply_to, Message(std::move(out)));
    }
    Value snapshot() const override {
        Value v(counter_schema());
        v.set("count", Cell::integer(served));
        return v;
    }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(8));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value& v) override { served = v.get("count")->as_int(); }

    std::int64_t served = 0;
    std::int64_t activations = 0;

private:
    std::string version_;
};

/// The whole cast a prepared replacement names, plus an observer that asks the
/// role "which version are you?" and records the answer.
struct Cast {
    Registered op;
    Registered coordinator;
    Registered observer;
    WeaveId incumbent{};
    WeaveId candidate{};
    VersionedService* incumbent_raw = nullptr;
    VersionedService* candidate_raw = nullptr;
    std::vector<std::string> answers;

    std::string ask(Switchboard& bus, const char* role) {
        const std::size_t before = answers.size();
        bus.send_as_to_role(observer.id, role,
                            Message(ping(1), observer.id, observer.id, 0));
        bus.pump();
        return answers.size() > before ? answers.back() : std::string("<no answer>");
    }
};

Cast cast_with_role(Switchboard& bus, const char* role) {
    Cast c{register_probe(bus, {pong_schema()}), register_probe(bus, {pong_schema()}),
           register_probe(bus, {greet_schema()}), WeaveId{}, WeaveId{}, nullptr, nullptr, {}};
    c.observer.weave->on_handle = [&c](const Message& in, Bus&, ProbeWeave&) {
        const Cell* m = in.payload.get("msg");
        c.answers.push_back(m == nullptr ? std::string("<none>") : std::string(m->as_text()));
    };
    auto inc = std::make_unique<VersionedService>("v1");
    c.incumbent_raw = inc.get();
    c.incumbent = bus.register_weave(std::move(inc), Grant{}.allow_any(), role);
    auto cand = std::make_unique<VersionedService>("v2");
    c.candidate_raw = cand.get();
    c.candidate = bus.register_weave(std::move(cand), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(c.candidate, c.coordinator.id));
    return c;
}

/// Everything a failure case must show is unchanged.
///
/// DELIBERATELY BUS-ONLY. An earlier version read `c.candidate_raw->activations`,
/// which is a use-after-free the moment an abort discards the candidate — and it
/// duly printed a garbage number rather than failing honestly. Everything here is
/// asked of the bus, which is also the only party whose answer would matter to a
/// real observer.
void incumbent_untouched(Switchboard& bus, Cast& c, const char* role) {
    CHECK(bus.alive(c.incumbent));
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(bus.role_holder(role) == c.incumbent);
    CHECK(c.ask(bus, role) == "v1");
    // The candidate never entered the world: it does not hold the role, and no
    // activation is waiting for it (an activation can only be queued BY admission,
    // which is the same act that moves the role).
    CHECK(bus.role_holder(role) != c.candidate);
    CHECK(bus.pending() == 0);
}

constexpr const char* kRole = "service";

} // namespace

TEST_CASE("R2B-3b-2: one prepared replacement, remembered from Preparing to Committed") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    REQUIRE(c.ask(bus, kRole) == "v1");

    const TxnResult begun = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                           c.incumbent, c.candidate, kRole, 4);
    REQUIRE(begun.ok);
    CHECK(bus.transaction_state(begun.id) == TxnState::Preparing);
    CHECK(bus.active_transactions() == 1);
    // Beginning changed nothing about the live world.
    CHECK(c.ask(bus, kRole) == "v1");
    CHECK_FALSE(bus.sealed(c.incumbent));

    // Preparation spends a deterministic budget — a step, not a clock.
    REQUIRE(bus.tick_preparation(begun.id).ok);
    CHECK(bus.transaction_state(begun.id) == TxnState::Preparing);
    CHECK(c.ask(bus, kRole) == "v1"); // ...and the incumbent is still the service

    REQUIRE(bus.mark_candidate_ready(begun.id, c.coordinator.id, c.candidate).ok);
    CHECK(bus.transaction_state(begun.id) == TxnState::Ready);
    CHECK(c.ask(bus, kRole) == "v1"); // still

    loom::Activated fact{1};
    const TxnResult committed = bus.commit_prepared_replacement(
        begun.id, host_lifecycle_authority(bus), Message(to_value(fact)), 1);
    REQUIRE(committed.ok);

    // One terminal result, for the exact operator, consumed once.
    TxnOutcome out{};
    REQUIRE(bus.take_outcome(c.op.id, out));
    CHECK(out.state == TxnState::Committed);
    CHECK(out.reason == TxnReason::None);
    CHECK_FALSE(bus.take_outcome(c.op.id, out)); // consumed

    // The topology moved exactly as R2B-3b-1 proved it does.
    CHECK(bus.sealed(c.incumbent));
    CHECK_FALSE(bus.sealed(c.candidate));
    CHECK(bus.role_holder(kRole) == c.candidate);
    // The activation is QUEUED by admission, so it is handled on the next drain —
    // and it is handled before any production, which R2B-3b-1 pins against traffic
    // that was already waiting.
    bus.pump();
    CHECK(c.candidate_raw->activations == 1);
    CHECK(c.ask(bus, kRole) == "v2");
    CHECK(c.incumbent_raw->served == 4); // it answered every pre-commit query

    // A second commit refuses, and the slot is already back.
    CHECK_FALSE(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                                Message(to_value(fact)), 2).ok);
    CHECK(bus.active_transactions() == 0);
}

TEST_CASE("R2B-3b-2: the registry is bounded, one transaction per incumbent, and every ending "
          "returns the slot") {
    Switchboard bus;
    std::vector<Cast> casts;
    std::vector<TxnId> ids;
    const std::size_t bound = Switchboard::kMaxPreparedReplacements;

    // Fill it exactly, each against its own incumbent.
    for (std::size_t i = 0; i < bound; ++i) {
        const std::string role = "svc" + std::to_string(i);
        casts.push_back(cast_with_role(bus, role.c_str()));
        const TxnResult r = bus.begin_prepared_replacement(
            casts[i].op.id, casts[i].coordinator.id, casts[i].incumbent, casts[i].candidate,
            role, 4);
        REQUIRE(r.ok); // including the LAST slot: the positive control
        ids.push_back(r.id);
    }
    CHECK(bus.active_transactions() == bound);

    // One beyond capacity refuses — and refuses BEFORE touching anything.
    Cast extra = cast_with_role(bus, "one-too-many");
    const TxnResult over = bus.begin_prepared_replacement(
        extra.op.id, extra.coordinator.id, extra.incumbent, extra.candidate, "one-too-many", 4);
    CHECK_FALSE(over.ok);
    CHECK(over.why == TxnReason::CapacityExhausted);
    incumbent_untouched(bus, extra, "one-too-many");
    CHECK(bus.sealed(extra.candidate)); // still sealed, still nobody's

    // A second transaction against an incumbent that already has one refuses, and
    // the first is not disturbed by having been asked.
    REQUIRE(bus.abort_prepared_replacement(ids[0], casts[0].op.id).ok);
    CHECK(bus.active_transactions() == bound - 1); // abort returned the slot
    const TxnResult dup = bus.begin_prepared_replacement(
        casts[1].op.id, casts[1].coordinator.id, casts[1].incumbent, casts[1].candidate,
        "svc1", 4);
    CHECK_FALSE(dup.ok);
    CHECK(bus.transaction_state(ids[1]) == TxnState::Preparing);
}

TEST_CASE("R2B-3b-2: the state machine has no back doors") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    loom::Activated fact{1};

    SUBCASE("commit from Preparing is refused, and nothing moves") {
        const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                          c.incumbent, c.candidate, kRole, 4);
        REQUIRE(t.ok);
        const TxnResult early = bus.commit_prepared_replacement(
            t.id, host_lifecycle_authority(bus), Message(to_value(fact)), 1);
        CHECK_FALSE(early.ok);
        CHECK(early.why == TxnReason::WrongState);
        CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
        incumbent_untouched(bus, c, kRole);
    }

    SUBCASE("a second abort, and a commit after abort, both refuse") {
        const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                          c.incumbent, c.candidate, kRole, 4);
        REQUIRE(t.ok);
        REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
        CHECK_FALSE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
        CHECK_FALSE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                                    Message(to_value(fact)), 1).ok);
        CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
        incumbent_untouched(bus, c, kRole);
    }

    SUBCASE("abort is safe from Ready as well as Preparing") {
        const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                          c.incumbent, c.candidate, kRole, 4);
        REQUIRE(t.ok);
        REQUIRE(bus.mark_candidate_ready(t.id, c.coordinator.id, c.candidate).ok);
        REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
        CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
        CHECK(bus.active_transactions() == 0);
        incumbent_untouched(bus, c, kRole);
    }
}

TEST_CASE("R2B-3b-2: the preparation budget is deterministic, and exhausting it aborts") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 3);
    REQUIRE(t.ok);

    // A step, not a clock: no sleeping, no polling, and no dependence on how much
    // unrelated traffic the bus happened to carry.
    REQUIRE(bus.tick_preparation(t.id).ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    REQUIRE(bus.tick_preparation(t.id).ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    const TxnResult last = bus.tick_preparation(t.id);
    CHECK_FALSE(last.ok);
    CHECK(last.why == TxnReason::PreparationExhausted);

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0); // capacity reclaimed
    CHECK_FALSE(bus.alive(c.candidate));   // candidate discarded
    incumbent_untouched(bus, c, kRole);

    TxnOutcome out{};
    REQUIRE(bus.take_outcome(c.op.id, out));
    CHECK(out.reason == TxnReason::PreparationExhausted);
}

TEST_CASE("R2B-3b-2: a participant that changes aborts its own transaction and only its own") {
    // The failure ladder, one rung per subcase, each proving the same seven things
    // about the incumbent and one thing about the transaction.
    int who = 0;
    SUBCASE("the operator dies") { who = 0; }
    SUBCASE("the operator's code is replaced") { who = 1; }
    SUBCASE("the coordinator dies") { who = 2; }
    SUBCASE("the coordinator's code is replaced") { who = 3; }
    SUBCASE("the candidate dies") { who = 4; }
    SUBCASE("the candidate is permanently removed") { who = 5; }

    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    // A SECOND, UNRELATED transaction that must survive all of it.
    Cast other = cast_with_role(bus, "other");
    const TxnResult mine = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                         c.incumbent, c.candidate, kRole, 8);
    REQUIRE(mine.ok);
    const TxnResult theirs = bus.begin_prepared_replacement(
        other.op.id, other.coordinator.id, other.incumbent, other.candidate, "other", 8);
    REQUIRE(theirs.ok);
    CHECK(bus.active_transactions() == 2);

    const auto churn = [&](WeaveId id, bool by_death, bool remove) {
        const std::string state = bus.snapshot_bytes(id);
        if (remove) {
            bus.unregister_weave(id);
        } else if (by_death) {
            bus.kill(id);
            (void)bus.reload(id, state);
        } else {
            (void)bus.swap_state(id, state);
        }
    };
    TxnReason expected = TxnReason::None;
    switch (who) {
    case 0: churn(c.op.id, true, false);          expected = TxnReason::OperatorChanged; break;
    case 1: churn(c.op.id, false, false);         expected = TxnReason::OperatorChanged; break;
    case 2: churn(c.coordinator.id, true, false); expected = TxnReason::CoordinatorChanged; break;
    case 3: churn(c.coordinator.id, false, false);expected = TxnReason::CoordinatorChanged; break;
    case 4: churn(c.candidate, true, false);      expected = TxnReason::CandidateChanged; break;
    default: churn(c.candidate, false, true);     expected = TxnReason::CandidateChanged; break;
    }

    CHECK(bus.transaction_state(mine.id) == TxnState::Aborted);
    CHECK_FALSE(bus.transaction_active(mine.id));
    incumbent_untouched(bus, c, kRole);

    // ...and the unrelated transaction is untouched. One weave's trouble is not
    // everybody's.
    CHECK(bus.transaction_active(theirs.id));
    CHECK(bus.transaction_state(theirs.id) == TxnState::Preparing);
    CHECK(bus.active_transactions() == 1); // exactly one slot came back

    TxnOutcome out{};
    if (who <= 1) {
        // A successor operator inherits nothing — not even the news.
        CHECK_FALSE(bus.take_outcome(c.op.id, out));
    } else {
        REQUIRE(bus.take_outcome(c.op.id, out));
        CHECK(out.reason == expected);
    }
}

TEST_CASE("R2B-3b-2: the incumbent drifting aborts the transaction rather than retargeting it") {
    int how = 0;
    SUBCASE("the incumbent dies") { how = 0; }
    SUBCASE("the incumbent's code is replaced") { how = 1; }
    SUBCASE("the role moves to somebody else") { how = 2; }

    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);

    if (how == 0) {
        const std::string state = bus.snapshot_bytes(c.incumbent);
        bus.kill(c.incumbent);
        REQUIRE(bus.reload(c.incumbent, state).revived);
    } else if (how == 1) {
        REQUIRE(bus.swap_state(c.incumbent, bus.snapshot_bytes(c.incumbent)).revived);
    } else {
        // The role is taken away entirely: prepared replacement is not recovery,
        // and it does not follow a slot that moved on.
        bus.unregister_weave(c.incumbent);
    }

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    CHECK_FALSE(bus.alive(c.candidate)); // the candidate never becomes the successor
    CHECK(bus.role_holder(kRole) != c.candidate);
}

TEST_CASE("R2B-3b-2: commit revalidates, and a precondition that drifts after Ready refuses "
          "without moving anything") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);
    REQUIRE(bus.mark_candidate_ready(t.id, c.coordinator.id, c.candidate).ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Ready);

    // Ready, and then the coordinator's code is replaced underneath it. The
    // transaction becomes terminal PROMPTLY — it does not wait for commit to
    // discover the problem.
    REQUIRE(bus.swap_state(c.coordinator.id, bus.snapshot_bytes(c.coordinator.id)).revived);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);

    loom::Activated fact{1};
    const TxnResult late = bus.commit_prepared_replacement(
        t.id, host_lifecycle_authority(bus), Message(to_value(fact)), 1);
    CHECK_FALSE(late.ok);
    incumbent_untouched(bus, c, kRole);
    CHECK(bus.pending() == 0); // no activation was queued
}

TEST_CASE("R2B-3b-2: a Ready transaction whose ADMISSION refuses aborts terminally rather than "
          "claiming success") {
    // THE BRANCH NO OTHER CASE REACHED. Everywhere else a doomed commit is caught
    // by the transaction's own revalidation, so `admit_candidate` is never asked
    // and never says no. The mutation that made a failed admission report
    // `Committed` therefore stayed GREEN — not because the guard was redundant,
    // but because nothing exercised it.
    //
    // A foreign lifecycle authority is the cleanest way in: every transaction
    // precondition holds, and admission refuses on its own terms.
    Switchboard bus;
    Switchboard decoy; // a real board, and a real authority — issued elsewhere
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);
    REQUIRE(bus.mark_candidate_ready(t.id, c.coordinator.id, c.candidate).ok);
    REQUIRE(bus.transaction_state(t.id) == TxnState::Ready);

    loom::Activated fact{1};
    const TxnResult refused = bus.commit_prepared_replacement(
        t.id, host_lifecycle_authority(decoy), Message(to_value(fact)), 1);
    CHECK_FALSE(refused.ok);
    CHECK(refused.why == TxnReason::AdmissionRefused);

    // Terminal, not retryable — and the world is exactly as it was.
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    incumbent_untouched(bus, c, kRole);

    TxnOutcome out{};
    REQUIRE(bus.take_outcome(c.op.id, out));
    CHECK(out.state == TxnState::Aborted);
    CHECK(out.reason == TxnReason::AdmissionRefused);
}

TEST_CASE("R2B-3b-2: readiness needs the exact coordinator and the exact candidate") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    Cast other = cast_with_role(bus, "other");
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);

    // Another coordinator cannot ready somebody else's transaction...
    CHECK_FALSE(bus.mark_candidate_ready(t.id, other.coordinator.id, c.candidate).ok);
    // ...and a valid coordinator cannot ready a different candidate.
    CHECK_FALSE(bus.mark_candidate_ready(t.id, c.coordinator.id, other.candidate).ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);

    // Nor can a stranger abort it.
    CHECK_FALSE(bus.abort_prepared_replacement(t.id, other.op.id).ok);
    CHECK(bus.transaction_active(t.id));

    // The exact pair still works.
    CHECK(bus.mark_candidate_ready(t.id, c.coordinator.id, c.candidate).ok);
}

TEST_CASE("R2B-3b-2: an aborted candidate cannot be admitted afterwards, and its queued speech "
          "does not escape") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);
    REQUIRE(bus.mark_candidate_ready(t.id, c.coordinator.id, c.candidate).ok);

    // The candidate says something to its coordinator, and the transaction is
    // abandoned before the pump.
    bus.send_as(c.candidate, c.coordinator.id, Message(pong(1)));
    REQUIRE(bus.pending() == 1);
    REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
    bus.pump();
    CHECK(c.coordinator.weave->handled_names.empty()); // R2B-2b: its life ended

    // And the admission primitive itself will not take it: it is gone.
    loom::Activated fact{1};
    const AdmitResult admitted = bus.admit_candidate(c.candidate, c.incumbent, kRole,
                                                     host_lifecycle_authority(bus),
                                                     Message(to_value(fact)), 1);
    CHECK_FALSE(admitted.ok);
    incumbent_untouched(bus, c, kRole);
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
