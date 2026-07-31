#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "weavelib/prepared_replacement_protocol.hpp"

#include <zen/gate.hpp>
#include <zen/host/lifecycle_wiring.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/weave/shape.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
///
/// As a CANDIDATE it also holds up its end of the preparation conversation —
/// deferring or answering immediately as the ask's plan directs. The dynamic
/// `versioned.service` pair below is the same shape as a real artifact; this one
/// keeps the native transaction cases readable without a `.so` per case.
class VersionedService final : public Weave {
public:
    explicit VersionedService(std::string version, bool candidate = false)
        : version_(std::move(version)), candidate_(candidate) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        if (!candidate_) {
            return {ping_schema(), schema_of<loom::Activated>()};
        }
        return {ping_schema(), schema_of<loom::Activated>(),
                schema_of<versioned::PrepareReplacement>(),
                schema_of<versioned::ContinuePreparation>()};
    }
    void handle(const Message& in, Bus& bus) override {
        const std::string_view shape = in.payload.schema().name();
        if (shape == std::string_view(loom::Activated::zen_name)) {
            ++activations;
            return;
        }
        if (shape == std::string_view(versioned::PrepareReplacement::zen_name)) {
            const versioned::PrepareReplacement ask =
                from_value<versioned::PrepareReplacement>(in.payload);
            ++prepares;
            transaction = ask.transaction;
            plan = ask.plan;
            if (plan == "defer" || plan == "defer-refuse") {
                pending = bus.make_deferred_answer(); // return without answering
                return;
            }
            if (plan == "ready") {
                versioned::CandidateReady yes{transaction};
                answered = bus.answer(Message(to_value(yes))).valid();
                return;
            }
            // "refuse", and anything it does not recognise: validating the ask is
            // the candidate's own business, and refusing is a REAL answer that
            // spends the same one right a readiness would have spent.
            versioned::CandidateRefused no{transaction,
                                           plan == "refuse" ? "declined" : "unknown plan"};
            answered = bus.answer(Message(to_value(no))).valid();
            return;
        }
        if (shape == std::string_view(versioned::ContinuePreparation::zen_name)) {
            ++continues;
            if (plan == "defer-refuse") {
                versioned::CandidateRefused no{transaction, "changed my mind"};
                answered = bus.spend_deferred(pending, Message(to_value(no))).valid();
                return;
            }
            versioned::CandidateReady yes{transaction};
            // Spend the retained right if it has one; otherwise answer THIS
            // delivery. The second branch is what lets a coordinator's own
            // answer-to-an-answer be echoed back, which is how the
            // inherited-ask-identity case gets a third hop to test.
            answered = pending.valid()
                           ? bus.spend_deferred(pending, Message(to_value(yes))).valid()
                           : bus.answer(Message(to_value(yes))).valid();
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
    std::int64_t prepares = 0;
    std::int64_t continues = 0;
    std::int64_t transaction = 0;
    bool answered = false;
    std::string plan;
    loom::DeferredAnswer pending{};

private:
    std::string version_;
    bool candidate_ = false;
};

/// The mutable half of the cast, on the heap.
///
/// HELD BY shared_ptr SO THE HOOKS SURVIVE THE `Cast` BEING RETURNED. The hooks a
/// cast installs run for the life of the bus and must reach durable storage; an
/// earlier version captured the factory's local by reference and was correct only
/// while NRVO elided the copy — true today, guaranteed by nothing.
struct CastLog {
    std::vector<std::string> answers;      ///< what the role said when asked
    std::vector<TxnResult> readiness;      ///< every verdict the bus gave the coordinator
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
    std::shared_ptr<CastLog> log = std::make_shared<CastLog>();
    /// The transaction the coordinator is currently preparing, if any. Set by the
    /// test; read by the coordinator's hook.
    std::shared_ptr<TxnId> live_txn = std::make_shared<TxnId>();

    std::string ask(Switchboard& bus, const char* role) {
        const std::size_t before = log->answers.size();
        bus.send_as_to_role(observer.id, role,
                            Message(ping(1), observer.id, observer.id, 0));
        bus.pump();
        return log->answers.size() > before ? log->answers.back()
                                            : std::string("<no answer>");
    }

    /// The last verdict the bus gave this coordinator, or a "nothing happened".
    TxnResult last_readiness() const {
        return log->readiness.empty() ? TxnResult{} : log->readiness.back();
    }
};

/// THE SMALLEST HONEST COORDINATOR — and deliberately a CREDULOUS one.
///
/// It owns lifecycle conversation and nothing else: it does not route domain
/// traffic, does not inspect who spoke, does not check provenance, and does not
/// compare correlations. Every delivery it receives while a transaction is live,
/// it offers to the bus as that transaction's readiness — reading only the
/// transaction id out of the payload, exactly as the phase says a payload may be
/// read: to NAME the record, never to authorize the transition.
///
/// That credulity is the point. If the coordinator were careful, a green suite
/// would prove the coordinator careful rather than the mechanism sound. Here every
/// forgery in the phase is offered to the bus by a party that believes it, and the
/// bus is the only thing saying no.
void wire_coordinator(Switchboard& bus, Cast& c) {
    std::shared_ptr<CastLog> log = c.log;
    std::shared_ptr<TxnId> live = c.live_txn;
    c.coordinator.weave->on_handle = [&bus, log, live](const Message& in, Bus&, ProbeWeave&) {
        if (!live->valid()) {
            return;
        }
        // The payload names a record — whatever it claims, including another
        // transaction's id or none at all.
        const Cell* claimed = in.payload.get("transaction");
        const TxnId named = claimed == nullptr
                                ? *live
                                : TxnId{static_cast<std::uint64_t>(claimed->as_int())};
        const bool refusal =
            in.payload.schema().name() == std::string_view(versioned::CandidateRefused::zen_name);
        log->readiness.push_back(bus.accept_preparation_answer(
            named, refusal ? PreparationAnswer::Refused : PreparationAnswer::Ready));
    };
}

Cast cast_with_role(Switchboard& bus, const char* role) {
    Cast c{register_probe(bus, {pong_schema()}),
           register_probe(bus, {schema_of<versioned::CandidateReady>(),
                                schema_of<versioned::CandidateRefused>(), pong_schema()}),
           register_probe(bus, {greet_schema()})};
    std::shared_ptr<CastLog> log = c.log;
    c.observer.weave->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        const Cell* m = in.payload.get("msg");
        log->answers.push_back(m == nullptr ? std::string("<none>") : std::string(m->as_text()));
    };
    wire_coordinator(bus, c);
    auto inc = std::make_unique<VersionedService>("v1");
    c.incumbent_raw = inc.get();
    c.incumbent = bus.register_weave(std::move(inc), Grant{}.allow_any(), role);
    auto cand = std::make_unique<VersionedService>("v2", /*candidate=*/true);
    c.candidate_raw = cand.get();
    c.candidate = bus.register_weave(std::move(cand), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(c.candidate, c.coordinator.id));
    return c;
}

/// Ask the candidate to prepare, and let the conversation run to its end.
///
/// This is what replaced `mark_candidate_ready` at every call site: not a shorter
/// way to declare a transaction ready, but the real conversation — an ask through
/// the sealed door, the candidate's own answer, and the bus deciding whether to
/// believe it. `plan` is what the candidate is asked to do ("ready", "defer",
/// "refuse", "defer-refuse"); a deferred plan needs the continuation below.
void ask_to_prepare(Switchboard& bus, Cast& c, TxnId id, const char* plan = "ready") {
    *c.live_txn = id;
    versioned::PrepareReplacement ask;
    ask.transaction = static_cast<std::int64_t>(id.value);
    ask.plan = plan;
    REQUIRE(bus.ask_candidate_to_prepare(id, Message(to_value(ask))).ok);
    bus.pump();
}

/// A coordinator that offers every delivery to the bus TWICE, so "one ask, one
/// answer" is exercised at the moment it matters instead of across a pump.
void offer_readiness_twice(Switchboard& bus, Cast& c) {
    std::shared_ptr<CastLog> log = c.log;
    std::shared_ptr<TxnId> live = c.live_txn;
    c.coordinator.weave->on_handle = [&bus, log, live](const Message&, Bus&, ProbeWeave&) {
        log->readiness.push_back(bus.accept_preparation_answer(*live, PreparationAnswer::Ready));
        log->readiness.push_back(bus.accept_preparation_answer(*live, PreparationAnswer::Ready));
    };
}

/// The later delivery a deferred preparation finishes on.
void continue_preparation(Switchboard& bus, Cast& c, TxnId id) {
    versioned::ContinuePreparation more{static_cast<std::int64_t>(id.value)};
    bus.send_as(c.coordinator.id, c.candidate, Message(to_value(more)));
    bus.pump();
}

/// The whole conversation, for the cases whose subject is something else.
void make_ready(Switchboard& bus, Cast& c, TxnId id) {
    ask_to_prepare(bus, c, id, "ready");
    REQUIRE(bus.transaction_state(id) == TxnState::Ready);
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

    make_ready(bus, c, begun.id);
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
        make_ready(bus, c, t.id);
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
    make_ready(bus, c, t.id);
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
    make_ready(bus, c, t.id);
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

TEST_CASE("R2B-3b-3: readiness needs the exact coordinator and the exact candidate") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    Cast other = cast_with_role(bus, "other");
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);
    const TxnResult u = bus.begin_prepared_replacement(other.op.id, other.coordinator.id,
                                                       other.incumbent, other.candidate,
                                                       "other", 8);
    REQUIRE(u.ok);

    // ANOTHER CANDIDATE'S AUTHENTIC ANSWER. Everything about it is real — a live
    // preparation ask, a genuine deferred right, the bus's own answer provenance —
    // and it belongs to somebody else's transaction. `other`'s coordinator is
    // pointed at OUR transaction id, so the answer arrives naming `t`.
    *other.live_txn = t.id;
    versioned::PrepareReplacement ask;
    ask.transaction = static_cast<std::int64_t>(t.id.value); // the id it will name
    ask.plan = "ready";
    REQUIRE(bus.ask_candidate_to_prepare(u.id, Message(to_value(ask))).ok);
    bus.pump();

    CHECK_FALSE(other.last_readiness().ok);
    CHECK(other.last_readiness().why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing); // ours is untouched...
    CHECK(bus.transaction_state(u.id) == TxnState::Preparing); // ...and so is theirs

    // Nor can a stranger abort it.
    CHECK_FALSE(bus.abort_prepared_replacement(t.id, other.op.id).ok);
    CHECK(bus.transaction_active(t.id));

    // Our own candidate, answering our own ask, still works — the positive control
    // that keeps the checks above from passing for the wrong reason.
    make_ready(bus, c, t.id);
}

TEST_CASE("R2B-3b-2: an aborted candidate cannot be admitted afterwards, and its queued speech "
          "does not escape") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 8);
    REQUIRE(t.ok);
    make_ready(bus, c, t.id);

    // The candidate says something to its coordinator, and the transaction is
    // abandoned before the pump. (The coordinator has already heard the readiness
    // answer by now, so the question is whether anything MORE reaches it.)
    const std::size_t heard = c.coordinator.weave->handled_names.size();
    bus.send_as(c.candidate, c.coordinator.id, Message(pong(1)));
    REQUIRE(bus.pending() == 1);
    REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
    bus.pump();
    CHECK(c.coordinator.weave->handled_names.size() == heard); // R2B-2b: its life ended

    // And the admission primitive itself will not take it: it is gone.
    loom::Activated fact{1};
    const AdmitResult admitted = bus.admit_candidate(c.candidate, c.incumbent, kRole,
                                                     host_lifecycle_authority(bus),
                                                     Message(to_value(fact)), 1);
    CHECK_FALSE(admitted.ok);
    incumbent_untouched(bus, c, kRole);
}

// ---- R2B-3b-2a: the transaction ends once --------------------------------------
//
// Terminalizing a transaction discards its candidate, and discarding a candidate
// is a lifecycle change, which re-enters the invalidation hook. If the ending
// transaction is still in the active registry at that moment, the hook rediscovers
// it and ends it AGAIN:
//
//     outer finish -> record outcome #1 -> unregister candidate
//                     -> invalidate -> finds the same transaction
//                        -> nested finish -> record outcome #2 -> erase
//                  -> outer erase finds nothing
//
// Two terminal truths for one promise, and two slots consumed in a bounded store
// that then evicts somebody else's result early.
//
//     One candidate belongs to one active replacement, and one replacement
//     produces one terminal truth.
//
// The repair is ORDERING, not a guard: remove the record from the active registry
// BEFORE any cleanup can run, so the hook has nothing to rediscover. Structural
// non-reentrancy beats a "currently finishing" flag, which would have to be
// correct at every future call site instead of at one.

namespace {

/// One terminal truth, and nothing behind it.
void exactly_one_outcome(Switchboard& bus, WeaveId op, TxnId id, TxnState state,
                         TxnReason reason) {
    TxnOutcome first{};
    REQUIRE(bus.take_outcome(op, first));
    CHECK(first.id == id);
    CHECK(first.state == state);
    CHECK(first.reason == reason);
    TxnOutcome second{};
    CHECK_FALSE(bus.take_outcome(op, second)); // ...and there is no second copy
}

} // namespace

TEST_CASE("R2B-3b-2a: every abort route produces exactly one terminal outcome") {
    // Each of these ends by discarding the candidate, so each re-enters the
    // invalidation hook on its way out. The hook must find nothing.
    int route = 0;
    SUBCASE("explicit abort from Preparing") { route = 0; }
    SUBCASE("explicit abort from Ready") { route = 1; }
    SUBCASE("preparation exhaustion") { route = 2; }
    SUBCASE("candidate death, which BEGINS inside kill()") { route = 3; }
    SUBCASE("coordinator death") { route = 4; }
    SUBCASE("incumbent role loss") { route = 5; }
    SUBCASE("admission refused from Ready") { route = 6; }

    Switchboard bus;
    Switchboard decoy;
    Cast c = cast_with_role(bus, kRole);
    const std::uint32_t budget = route == 2 ? 1u : 8u;
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, budget);
    REQUIRE(t.ok);
    TxnReason expected = TxnReason::ExplicitAbort;

    switch (route) {
    case 0:
        REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
        break;
    case 1:
        make_ready(bus, c, t.id);
        REQUIRE(bus.abort_prepared_replacement(t.id, c.op.id).ok);
        break;
    case 2:
        CHECK_FALSE(bus.tick_preparation(t.id).ok);
        expected = TxnReason::PreparationExhausted;
        break;
    case 3:
        bus.kill(c.candidate); // the whole ending happens inside this call
        expected = TxnReason::CandidateChanged;
        break;
    case 4:
        bus.kill(c.coordinator.id);
        expected = TxnReason::CoordinatorChanged;
        break;
    case 5:
        bus.unregister_weave(c.incumbent);
        expected = TxnReason::IncumbentChanged;
        break;
    default: {
        make_ready(bus, c, t.id);
        loom::Activated fact{1};
        CHECK_FALSE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(decoy),
                                                    Message(to_value(fact)), 1).ok);
        expected = TxnReason::AdmissionRefused;
        break;
    }
    }

    CHECK_FALSE(bus.transaction_active(t.id));
    CHECK(bus.active_transactions() == 0); // the slot came back exactly once
    exactly_one_outcome(bus, c.op.id, t.id, TxnState::Aborted, expected);
}

TEST_CASE("R2B-3b-2a: a duplicate terminalization cannot consume two slots of the bounded "
          "terminal store") {
    // The bound is the instrument. Fill the store exactly, then run an abort whose
    // candidate cleanup re-enters the hook: a second insertion would evict one MORE
    // of the older results than it should, and the count of survivors says so.
    Switchboard bus;
    std::vector<Cast> casts;
    std::vector<TxnId> ids;
    const std::size_t store = Switchboard::kMaxTerminalOutcomes;

    // Each operator gets exactly one terminal result, so survivors are countable.
    for (std::size_t i = 0; i < store; ++i) {
        const std::string role = "fill" + std::to_string(i);
        casts.push_back(cast_with_role(bus, role.c_str()));
        const TxnResult r = bus.begin_prepared_replacement(
            casts[i].op.id, casts[i].coordinator.id, casts[i].incumbent, casts[i].candidate,
            role, 4);
        REQUIRE(r.ok);
        ids.push_back(r.id);
        REQUIRE(bus.abort_prepared_replacement(r.id, casts[i].op.id).ok);
    }
    CHECK(bus.active_transactions() == 0);

    // One more abort, whose cleanup re-enters. It should evict EXACTLY the oldest.
    Cast last = cast_with_role(bus, "last");
    const TxnResult t = bus.begin_prepared_replacement(last.op.id, last.coordinator.id,
                                                      last.incumbent, last.candidate,
                                                      "last", 4);
    REQUIRE(t.ok);
    REQUIRE(bus.abort_prepared_replacement(t.id, last.op.id).ok);

    // The newest is there, exactly once.
    exactly_one_outcome(bus, last.op.id, t.id, TxnState::Aborted, TxnReason::ExplicitAbort);

    // Count how many of the original results survived. One insertion evicts one;
    // a duplicate would have evicted two.
    std::size_t survivors = 0;
    for (std::size_t i = 0; i < store; ++i) {
        TxnOutcome out{};
        if (bus.take_outcome(casts[i].op.id, out)) {
            ++survivors;
        }
    }
    CHECK(survivors == store - 1);
}

TEST_CASE("R2B-3b-2a: one sealed candidate belongs to at most one active transaction") {
    Switchboard bus;
    // Two incumbents in two roles, one coordinator, and ONE candidate.
    Cast a = cast_with_role(bus, "role-a");
    Cast b = cast_with_role(bus, "role-b");

    const TxnResult first = bus.begin_prepared_replacement(a.op.id, a.coordinator.id,
                                                           a.incumbent, a.candidate,
                                                           "role-a", 8);
    REQUIRE(first.ok);

    // A different incumbent, a different operator — and the SAME candidate. Every
    // other precondition holds: it is sealed by its coordinator, holds no role, and
    // is alive. Only exclusivity stands in the way.
    const TxnResult second = bus.begin_prepared_replacement(b.op.id, a.coordinator.id,
                                                            b.incumbent, a.candidate,
                                                            "role-b", 8);
    CHECK_FALSE(second.ok);
    CHECK(second.why == TxnReason::CandidateBusy);

    // The first is untouched, the slot was never consumed, and neither incumbent
    // moved. Two incumbents were never promised the same successor.
    CHECK(bus.transaction_active(first.id));
    CHECK(bus.transaction_state(first.id) == TxnState::Preparing);
    CHECK(bus.active_transactions() == 1);
    CHECK(bus.candidate_owner(a.candidate).who == a.coordinator.id);
    incumbent_untouched(bus, a, "role-a");
    incumbent_untouched(bus, b, "role-b");

    // POSITIVE CONTROL: exclusivity is about the CANDIDATE, not about there being
    // one transaction in the world. Distinct candidates coexist happily.
    const TxnResult distinct = bus.begin_prepared_replacement(b.op.id, b.coordinator.id,
                                                              b.incumbent, b.candidate,
                                                              "role-b", 8);
    REQUIRE(distinct.ok);
    CHECK(bus.active_transactions() == 2);
    CHECK(bus.transaction_state(first.id) == TxnState::Preparing);
}

TEST_CASE("R2B-3b-2a: a committed candidate is public, so it cannot be named as a sealed "
          "candidate again") {
    Switchboard bus;
    Cast a = cast_with_role(bus, "role-a");
    Cast b = cast_with_role(bus, "role-b");

    const TxnResult t = bus.begin_prepared_replacement(a.op.id, a.coordinator.id, a.incumbent,
                                                       a.candidate, "role-a", 8);
    REQUIRE(t.ok);
    make_ready(bus, a, t.id);
    loom::Activated fact{1};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 1).ok);
    CHECK(bus.active_transactions() == 0);
    exactly_one_outcome(bus, a.op.id, t.id, TxnState::Committed, TxnReason::None);

    // It is a live public service now, holding a role. The existing begin
    // preconditions refuse it — not as "busy", but as "not a sealed candidate".
    const TxnResult again = bus.begin_prepared_replacement(b.op.id, b.coordinator.id,
                                                           b.incumbent, a.candidate,
                                                           "role-b", 8);
    CHECK_FALSE(again.ok);
    CHECK(again.why == TxnReason::PreconditionFailed);
    CHECK(bus.active_transactions() == 0);
    incumbent_untouched(bus, b, "role-b");
}

TEST_CASE("R2B-3b-2a: aborting one transaction does not duplicate its result, disturb another, "
          "or consume the other's terminal capacity") {
    Switchboard bus;
    Cast a = cast_with_role(bus, "role-a");
    Cast b = cast_with_role(bus, "role-b");
    const TxnResult ta = bus.begin_prepared_replacement(a.op.id, a.coordinator.id, a.incumbent,
                                                        a.candidate, "role-a", 8);
    const TxnResult tb = bus.begin_prepared_replacement(b.op.id, b.coordinator.id, b.incumbent,
                                                        b.candidate, "role-b", 8);
    REQUIRE(ta.ok);
    REQUIRE(tb.ok);
    make_ready(bus, b, tb.id);

    REQUIRE(bus.abort_prepared_replacement(ta.id, a.op.id).ok);

    exactly_one_outcome(bus, a.op.id, ta.id, TxnState::Aborted, TxnReason::ExplicitAbort);
    // B is exactly where it was — still Ready, still active, still owning its
    // candidate, and its own terminal slot is still unused.
    CHECK(bus.transaction_active(tb.id));
    CHECK(bus.transaction_state(tb.id) == TxnState::Ready);
    CHECK(bus.active_transactions() == 1);
    TxnOutcome none{};
    CHECK_FALSE(bus.take_outcome(b.op.id, none));
    incumbent_untouched(bus, b, "role-b");
}

// ---- R2B-3b-3: the candidate answers ------------------------------------------
//
// Until now a transaction became Ready because a trusted host said so. The state
// machine was real and the readiness was scaffolding, named as such.
//
//     A transaction becomes ready only when the exact sealed candidate
//     authentically answers the exact preparation request that belongs to that
//     transaction.
//
// The coordinator in these cases is deliberately CREDULOUS (see wire_coordinator):
// it offers every delivery it receives to the bus as readiness, reading only the
// transaction id from the payload. So a green here is never "the coordinator was
// careful" — it is always "the bus refused".

TEST_CASE("R2B-3b-3: an immediate answer and one deferred across deliveries are the same "
          "readiness") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);

    SUBCASE("immediate: the candidate completes inside the preparation handler") {
        ask_to_prepare(bus, c, t.id, "ready");
        CHECK(c.candidate_raw->prepares == 1);
        CHECK(c.candidate_raw->continues == 0);
        CHECK(c.candidate_raw->answered); // the board took its answer
        CHECK(c.last_readiness().ok);
        CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    }

    SUBCASE("deferred: it takes the answer away and finishes on a later delivery") {
        ask_to_prepare(bus, c, t.id, "defer");
        CHECK(c.candidate_raw->prepares == 1);
        CHECK_FALSE(c.candidate_raw->answered);
        CHECK(c.log->readiness.empty());                          // nothing was offered
        CHECK(bus.transaction_state(t.id) == TxnState::Preparing); // ...so nothing moved
        CHECK(c.ask(bus, kRole) == "v1"); // an unrelated delivery, and the incumbent serves it

        continue_preparation(bus, c, t.id);
        CHECK(c.candidate_raw->continues == 1);
        CHECK(c.candidate_raw->answered);
        CHECK(c.last_readiness().ok);
        CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    }

    // Whichever road it came by: the incumbent is untouched and still the service.
    CHECK(bus.role_holder(kRole) == c.incumbent);
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(c.ask(bus, kRole) == "v1");
}

TEST_CASE("R2B-3b-3: an immediate readiness answer consumes no deferred-answer capacity") {
    // THE BOUND IS ONLY AN INSTRUMENT WHILE IT IS HELD SATURATED (R2B-3b-1a's
    // lesson, paid for once already): a test that defers and pumps between asks
    // returns each slot before taking the next and never fills anything.
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);

    // 64 genuinely OUTSTANDING deferrals, held open for the rest of the case.
    std::vector<Registered> hoarders;
    Registered asker = register_probe(bus, {greet_schema()});
    for (std::size_t i = 0; i < Switchboard::kMaxDeferredAnswers; ++i) {
        Registered h = register_probe(bus, {pong_schema()});
        h.weave->on_handle = [](const Message&, Bus& b, ProbeWeave& self) {
            self.pending = b.make_deferred_answer(); // taken, and never spent
        };
        hoarders.push_back(h);
        bus.send_as(asker.id, h.id, Message(pong(1), asker.id, asker.id, 0));
    }
    bus.pump();
    for (Registered& h : hoarders) {
        REQUIRE(h.weave->pending.valid()); // every slot really is taken
    }

    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);

    SUBCASE("the immediate path is unaffected by a full deferred registry") {
        ask_to_prepare(bus, c, t.id, "ready");
        CHECK(c.candidate_raw->answered);
        CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    }

    SUBCASE("the deferred path is not — which is what proves the registry is full") {
        // The POSITIVE CONTROL. Without it, "immediate still works" could mean the
        // registry was never saturated at all.
        ask_to_prepare(bus, c, t.id, "defer");
        CHECK_FALSE(c.candidate_raw->pending.valid()); // Exhausted: nothing to retain
        CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    }
}

TEST_CASE("R2B-3b-3: a forged readiness has the right shape and is not an answer") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    // A rogue that can reach the coordinator and knows the whole protocol.
    Registered rogue = register_probe(bus, {pong_schema()});
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    *c.live_txn = t.id;
    // Open the real conversation so the transaction is genuinely WAITING for an
    // answer — a forgery refused by a transaction that expected nothing would
    // prove nothing.
    ask_to_prepare(bus, c, t.id, "defer");
    REQUIRE(bus.transaction_state(t.id) == TxnState::Preparing);

    versioned::CandidateReady yes{static_cast<std::int64_t>(t.id.value)};
    versioned::CandidateRefused no{static_cast<std::int64_t>(t.id.value), "not really"};

    SUBCASE("an authentic-looking answer offered before anybody was asked") {
        // A transaction whose conversation was never opened has no readiness to
        // be late for. Proven on a SECOND transaction, so the one above stays
        // genuinely mid-preparation.
        Cast quiet = cast_with_role(bus, "quiet");
        const TxnResult q = bus.begin_prepared_replacement(quiet.op.id, quiet.coordinator.id,
                                                           quiet.incumbent, quiet.candidate,
                                                           "quiet", 16);
        REQUIRE(q.ok);
        *quiet.live_txn = q.id;
        versioned::CandidateReady early{static_cast<std::int64_t>(q.id.value)};
        bus.send_as(quiet.candidate, quiet.coordinator.id, Message(to_value(early)));
        bus.pump();
        REQUIRE(quiet.log->readiness.size() == 1);
        CHECK(quiet.last_readiness().why == TxnReason::InvalidReadiness);
        CHECK(bus.transaction_state(q.id) == TxnState::Preparing);
    }

    SUBCASE("an ordinary send of the right shape, with the right correlation") {
        // The correlations Loom mints for preparation asks start at 1 and advance
        // by one, so a forger that has read this file knows exactly which number
        // to use. Knowing it is worth nothing, which is the point — try them all.
        for (std::uint64_t correlation = 0; correlation < 4; ++correlation) {
            bus.send_as(rogue.id, c.coordinator.id,
                        Message(to_value(yes), rogue.id, rogue.id, correlation));
            bus.send_as(rogue.id, c.coordinator.id,
                        Message(to_value(no), rogue.id, rogue.id, correlation));
        }
        bus.pump();
        CHECK(c.log->readiness.size() == 8); // the coordinator offered every one
        for (const TxnResult& r : c.log->readiness) {
            CHECK_FALSE(r.ok);
            CHECK(r.why == TxnReason::InvalidReadiness);
        }
    }

    SUBCASE("a hand-built answer provenance, which no enqueue path lets out") {
        // The honest API CAN express this attack: `Provenance::attested` is public
        // and safe precisely because every ordinary enqueue overwrites it. So the
        // test forges the frame rather than asserting the attack is unsayable.
        Message frame(to_value(yes), rogue.id, rogue.id, 1);
        frame.provenance = Provenance::attested(Provenance::Kind::Answer, 0);
        bus.send_as(rogue.id, c.coordinator.id, std::move(frame));
        bus.pump();
        REQUIRE(c.log->readiness.size() == 1);
        CHECK(c.last_readiness().why == TxnReason::InvalidReadiness);
    }

    SUBCASE("the GENUINE candidate, speaking ordinarily to its own coordinator") {
        // The sharpest one in the suite: the right speaker, the right listener,
        // the right shape, the right transaction — and nobody asked.
        bus.send_as(c.candidate, c.coordinator.id, Message(to_value(yes)));
        bus.pump();
        REQUIRE(c.log->readiness.size() == 1);
        CHECK(c.last_readiness().why == TxnReason::InvalidReadiness);
    }

    // Nothing above moved the transaction, and none of it ENDED the transaction
    // either: hostile traffic does not get to abort somebody else's promise.
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    CHECK(bus.transaction_active(t.id));
    incumbent_untouched(bus, c, kRole);

    // ...and the real answer still lands, which is what keeps every refusal above
    // from having passed for the wrong reason.
    continue_preparation(bus, c, t.id);
    CHECK(c.last_readiness().ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Ready);
}

TEST_CASE("R2B-3b-3: an authentic answer to a DIFFERENT ask, with the right correlation, is not "
          "the readiness answer") {
    // THE CASE THE WHOLE `Envelope::preparation` FIELD EXISTS FOR, and the only
    // one that can decide it. Everywhere else the answer's speaker, its recipient
    // and its correlation are all already right — because they come from the real
    // ask — so those terms cannot tell a true readiness from a false one.
    //
    // Here the coordinator asks its candidate the SAME QUESTION twice: once
    // through `ask_candidate_to_prepare`, which opens the transaction's one
    // conversation, and once as an ordinary send carrying the same correlation by
    // hand. The candidate answers the second one, authentically, to the right
    // party, with the right number. Only "which ask is this?" separates them.
    //
    // Loom mints preparation correlations from 1 on a fresh board, so the forging
    // coordinator here knows exactly which number to write — as any real one
    // could, since a correlation travels on the wire and is nobody's secret.
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    ask_to_prepare(bus, c, t.id, "defer"); // the real conversation: open, unanswered
    REQUIRE(bus.transaction_state(t.id) == TxnState::Preparing);
    REQUIRE(c.log->readiness.empty());

    versioned::PrepareReplacement lookalike{static_cast<std::int64_t>(t.id.value), "ready", 0};
    bus.send_as(c.coordinator.id, c.candidate,
                Message(to_value(lookalike), c.coordinator.id, WeaveId{}, /*correlation=*/1));
    bus.pump();

    REQUIRE(c.log->readiness.size() == 1); // the candidate answered, and was offered
    CHECK_FALSE(c.last_readiness().ok);
    CHECK(c.last_readiness().why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    incumbent_untouched(bus, c, kRole);

    // ...and the real conversation is still open, still answerable, still the only
    // one that counts.
    continue_preparation(bus, c, t.id);
    CHECK(c.last_readiness().ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Ready);
}

TEST_CASE("R2B-3b-3: answering the readiness instead of consuming it does not make the next "
          "exchange the readiness") {
    // FOUND BY READING, NOT BY A FAILING TEST — and it is the subtlest thing in
    // the phase. `enqueue_answer` copies the correlation forward at every hop, so
    // if the ask's identity were inherited the same way, a coordinator that
    // ANSWERED the readiness rather than consuming it would find a later,
    // unrelated exchange satisfying every term:
    //
    //   coordinator -> candidate   the ask                 (preparation = T)
    //   candidate   -> coordinator the real readiness      (T)
    //   coordinator -> candidate   an answer to THAT       (T, if inherited)
    //   candidate   -> coordinator an answer to that       (T, and indistinguishable)
    //
    // An ask seeds an answerable conversation; its answer does not seed another.
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);

    // A coordinator that answers the first thing it hears, and only offers the
    // SECOND one as readiness.
    std::shared_ptr<CastLog> log = c.log;
    std::shared_ptr<TxnId> live = c.live_txn;
    int seen = 0;
    c.coordinator.weave->on_handle = [&bus, &seen, log, live](const Message&, Bus& wb,
                                                              ProbeWeave&) {
        if (++seen == 1) {
            // Answer the readiness back at the candidate instead of consuming it.
            wb.answer(Message(to_value(versioned::ContinuePreparation{
                static_cast<std::int64_t>(live->value)})));
            return;
        }
        log->readiness.push_back(bus.accept_preparation_answer(*live, PreparationAnswer::Ready));
    };

    ask_to_prepare(bus, c, t.id, "ready");
    CHECK(seen == 2); // the echo really did come back round
    REQUIRE(c.log->readiness.size() == 1);
    CHECK_FALSE(c.last_readiness().ok);
    CHECK(c.last_readiness().why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    incumbent_untouched(bus, c, kRole);
}

TEST_CASE("R2B-3b-3: a delivery live on one Loom confers nothing on another") {
    // A transaction id is a number, and a number belongs to no world. The only
    // thing that could make one mean something is a live delivery — and a delivery
    // is live on exactly one board. So the decoy has nothing to offer, even at the
    // precise instant the real board is dispatching a genuine readiness answer.
    Switchboard bus;
    Switchboard decoy;
    Cast c = cast_with_role(bus, kRole);
    Cast shadow = cast_with_role(decoy, kRole); // a real transaction, elsewhere
    const TxnResult here = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                          c.candidate, kRole, 16);
    const TxnResult there = decoy.begin_prepared_replacement(
        shadow.op.id, shadow.coordinator.id, shadow.incumbent, shadow.candidate, kRole, 16);
    REQUIRE(here.ok);
    REQUIRE(there.ok);
    CHECK(here.id == there.id); // the same NUMBER, in two worlds

    std::vector<TxnResult> foreign;
    std::shared_ptr<TxnId> live = c.live_txn;
    std::shared_ptr<CastLog> log = c.log;
    c.coordinator.weave->on_handle = [&bus, &decoy, &foreign, log, live](const Message&, Bus&,
                                                                        ProbeWeave&) {
        // Mid-delivery on `bus`, offer the very same id to the other board.
        foreign.push_back(decoy.accept_preparation_answer(*live, PreparationAnswer::Ready));
        log->readiness.push_back(bus.accept_preparation_answer(*live, PreparationAnswer::Ready));
    };
    ask_to_prepare(bus, c, here.id, "ready");

    REQUIRE(foreign.size() == 1);
    CHECK_FALSE(foreign[0].ok);
    CHECK(foreign[0].why == TxnReason::InvalidReadiness); // no delivery is live there
    CHECK(decoy.transaction_state(there.id) == TxnState::Preparing);
    CHECK(c.last_readiness().ok); // ...while the real one, on its own board, lands
    CHECK(bus.transaction_state(here.id) == TxnState::Ready);
}

TEST_CASE("R2B-3b-3: an authentic answer that names another transaction satisfies neither") {
    Switchboard bus;
    Cast a = cast_with_role(bus, "role-a");
    Cast b = cast_with_role(bus, "role-b");
    const TxnResult ta = bus.begin_prepared_replacement(a.op.id, a.coordinator.id, a.incumbent,
                                                        a.candidate, "role-a", 16);
    const TxnResult tb = bus.begin_prepared_replacement(b.op.id, b.coordinator.id, b.incumbent,
                                                        b.candidate, "role-b", 16);
    REQUIRE(ta.ok);
    REQUIRE(tb.ok);

    // A's candidate is asked, authentically, for A's transaction — but the ask
    // tells it to write B's id in its answer, and A's coordinator dutifully offers
    // the answer against B.
    *a.live_txn = ta.id;
    versioned::PrepareReplacement ask;
    ask.transaction = static_cast<std::int64_t>(tb.id.value); // the lie
    ask.plan = "ready";
    REQUIRE(bus.ask_candidate_to_prepare(ta.id, Message(to_value(ask))).ok);
    bus.pump();

    CHECK_FALSE(a.last_readiness().ok);
    CHECK(a.last_readiness().why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_state(ta.id) == TxnState::Preparing); // not the one it answered
    CHECK(bus.transaction_state(tb.id) == TxnState::Preparing); // not the one it named
    incumbent_untouched(bus, a, "role-a");
    incumbent_untouched(bus, b, "role-b");
}

TEST_CASE("R2B-3b-3: one ask, one answer — a replay and a second answer both refuse") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);

    SUBCASE("the coordinator offers the same delivery twice") {
        // Two independent walls stop a replay, and they answer in a definite
        // order. The state machine speaks first: an accepted readiness has already
        // moved the transaction to Ready, so the second offer is refused as
        // WrongState — truthfully, and before the conversation is consulted at
        // all. (The conversation's own consumed-check is what catches a replay
        // after a REFUSED validation, where the state has not moved; the
        // role-drift case pins that one.)
        offer_readiness_twice(bus, c);
        ask_to_prepare(bus, c, t.id, "ready");
        REQUIRE(c.log->readiness.size() == 2);
        CHECK(c.log->readiness[0].ok);
        CHECK_FALSE(c.log->readiness[1].ok);
        CHECK(c.log->readiness[1].why == TxnReason::WrongState);
        CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    }

    SUBCASE("the candidate tries to answer a second time") {
        // The other wall, and it is the bus's answer registry rather than the
        // transaction's: one delivered ask authorizes one answer, so the second
        // continuation finds nothing left to spend.
        ask_to_prepare(bus, c, t.id, "defer");
        continue_preparation(bus, c, t.id);
        REQUIRE(bus.transaction_state(t.id) == TxnState::Ready);
        REQUIRE(c.log->readiness.size() == 1);

        continue_preparation(bus, c, t.id); // ...and again
        CHECK(c.candidate_raw->continues == 2);
        CHECK_FALSE(c.candidate_raw->answered); // the spend was refused
        CHECK(c.log->readiness.size() == 1);    // nothing reached the coordinator
        CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    }

    SUBCASE("a second preparation ask is refused before anything is sent") {
        ask_to_prepare(bus, c, t.id, "defer");
        versioned::PrepareReplacement again{static_cast<std::int64_t>(t.id.value), "ready", 0};
        const TxnResult second = bus.ask_candidate_to_prepare(t.id, Message(to_value(again)));
        CHECK_FALSE(second.ok);
        CHECK(second.why == TxnReason::PreparationAlreadyAsked);
        CHECK(bus.pending() == 0); // nothing was queued
        CHECK(c.candidate_raw->prepares == 1);
    }
}

TEST_CASE("R2B-3b-3: the candidate's own refusal ends the transaction once, and the incumbent "
          "never learns of it") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const char* plan = "refuse";
    SUBCASE("refused immediately") { plan = "refuse"; }
    SUBCASE("refused after deferring") { plan = "defer-refuse"; }
    SUBCASE("refused because the plan made no sense to it") { plan = "do-a-barrel-roll"; }

    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    ask_to_prepare(bus, c, t.id, plan);
    if (std::string(plan) == "defer-refuse") {
        CHECK(bus.transaction_active(t.id)); // still preparing, still nobody's problem
        continue_preparation(bus, c, t.id);
    }

    CHECK(c.last_readiness().ok); // the answer was accepted — it simply said no
    CHECK(c.last_readiness().why == TxnReason::CandidateRefused);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0); // capacity reclaimed
    CHECK_FALSE(bus.alive(c.candidate));   // discarded, per the existing cleanup law
    exactly_one_outcome(bus, c.op.id, t.id, TxnState::Aborted, TxnReason::CandidateRefused);
    incumbent_untouched(bus, c, kRole);
}

TEST_CASE("R2B-3b-3: a forged refusal cannot abort a legitimate transaction") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    Registered rogue = register_probe(bus, {pong_schema()});
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    ask_to_prepare(bus, c, t.id, "defer");

    versioned::CandidateRefused no{static_cast<std::int64_t>(t.id.value), "give up"};
    bus.send_as(rogue.id, c.coordinator.id, Message(to_value(no), rogue.id, rogue.id, 1));
    bus.pump();

    CHECK(c.last_readiness().why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_active(t.id));
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    TxnOutcome none{};
    CHECK_FALSE(bus.take_outcome(c.op.id, none)); // no terminal result was manufactured
    CHECK(bus.alive(c.candidate));                // and the candidate was not discarded
    incumbent_untouched(bus, c, kRole);
}

TEST_CASE("R2B-3b-3: an authentic answer offered against a transaction that has ended is late, "
          "and revives nothing") {
    Switchboard bus;
    Cast a = cast_with_role(bus, "role-a");
    Cast b = cast_with_role(bus, "role-b");
    const TxnResult ended = bus.begin_prepared_replacement(b.op.id, b.coordinator.id,
                                                           b.incumbent, b.candidate,
                                                           "role-b", 16);
    REQUIRE(ended.ok);
    REQUIRE(bus.abort_prepared_replacement(ended.id, b.op.id).ok);
    TxnOutcome collected{};
    REQUIRE(bus.take_outcome(b.op.id, collected)); // its ONE result, already taken

    const TxnResult live = bus.begin_prepared_replacement(a.op.id, a.coordinator.id, a.incumbent,
                                                          a.candidate, "role-a", 16);
    REQUIRE(live.ok);
    *a.live_txn = live.id;
    versioned::PrepareReplacement ask;
    ask.transaction = static_cast<std::int64_t>(ended.id.value); // names the dead one
    ask.plan = "ready";
    REQUIRE(bus.ask_candidate_to_prepare(live.id, Message(to_value(ask))).ok);
    bus.pump();

    CHECK_FALSE(a.last_readiness().ok);
    CHECK(a.last_readiness().why == TxnReason::LateReadiness);
    // An id this Loom never minted is a different truth, and says so.
    CHECK(bus.accept_preparation_answer(TxnId{9999}, PreparationAnswer::Ready).why ==
          TxnReason::NoSuchTransaction);

    // No second outcome appeared for the dead transaction, and the live one did
    // not advance on somebody else's answer.
    TxnOutcome second{};
    CHECK_FALSE(bus.take_outcome(b.op.id, second));
    CHECK(bus.transaction_state(live.id) == TxnState::Preparing);
    CHECK(bus.transaction_state(ended.id) == TxnState::Aborted);
    incumbent_untouched(bus, a, "role-a");
    incumbent_untouched(bus, b, "role-b");
}

TEST_CASE("R2B-3b-3: the budget keeps running through a deferred preparation, and exhausting it "
          "ends the transaction once") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 3);
    REQUIRE(t.ok);
    ask_to_prepare(bus, c, t.id, "defer"); // the candidate holds the answer and stalls
    REQUIRE(c.candidate_raw->pending.valid());

    // A conversation in progress does not stop the accounting. No wall clock is
    // involved: these are steps the operator chose to spend.
    REQUIRE(bus.tick_preparation(t.id).ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    REQUIRE(bus.tick_preparation(t.id).ok);
    const TxnResult last = bus.tick_preparation(t.id);
    CHECK_FALSE(last.ok);
    CHECK(last.why == TxnReason::PreparationExhausted);

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    CHECK_FALSE(bus.alive(c.candidate));
    exactly_one_outcome(bus, c.op.id, t.id, TxnState::Aborted, TxnReason::PreparationExhausted);
    incumbent_untouched(bus, c, kRole);

    // AND THE STALLED ANSWER CANNOT ARRIVE LATE. Aborting discards the candidate,
    // so the author of any answer still owed is gone — its queued speech is
    // refused as SenderLifeEnded before it reaches anyone, and there is no
    // transaction left to name in any case. That is stronger than "the late answer
    // is refused": there is no late answer.
    const std::size_t offered = c.log->readiness.size();
    bus.pump();
    CHECK(c.log->readiness.size() == offered);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
}

TEST_CASE("R2B-3b-3: a lifecycle change during preparation aborts before any answer can land") {
    int how = 0;
    SUBCASE("the candidate dies") { how = 0; }
    SUBCASE("the candidate's code is replaced") { how = 1; }
    SUBCASE("the coordinator dies") { how = 2; }
    SUBCASE("the coordinator's code is replaced") { how = 3; }
    SUBCASE("the operator dies") { how = 4; }

    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    ask_to_prepare(bus, c, t.id, "defer");
    REQUIRE(bus.transaction_state(t.id) == TxnState::Preparing);
    REQUIRE(c.candidate_raw->pending.valid()); // it really is holding the answer

    const std::string op_state = bus.snapshot_bytes(c.op.id);
    TxnReason expected = TxnReason::CandidateChanged;
    if (how == 0) {
        bus.kill(c.candidate);
    } else if (how == 1) {
        const std::string state = bus.snapshot_bytes(c.candidate);
        REQUIRE(bus.swap_state(c.candidate, state).revived);
    } else if (how == 2) {
        bus.kill(c.coordinator.id);
        expected = TxnReason::CoordinatorChanged;
    } else if (how == 3) {
        REQUIRE(bus.swap_state(c.coordinator.id, bus.snapshot_bytes(c.coordinator.id)).revived);
        expected = TxnReason::CoordinatorChanged;
    } else {
        bus.kill(c.op.id);
        expected = TxnReason::OperatorChanged;
    }

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    if (how != 4) {
        exactly_one_outcome(bus, c.op.id, t.id, TxnState::Aborted, expected);
    } else {
        // THE OPERATOR SUCCESSOR INHERITS NOTHING. The result is kept for the
        // exact life and incarnation that began the transaction, and that life is
        // over — so bringing the same id back produces a participant with no
        // standing to collect, exactly as it has no standing to converse.
        REQUIRE(bus.reload(c.op.id, op_state).revived);
        REQUIRE(bus.alive(c.op.id));
        TxnOutcome inherited{};
        CHECK_FALSE(bus.take_outcome(c.op.id, inherited));
    }

    // Whatever the candidate still owes, it cannot be readiness: there is no
    // transaction to name, and the continuation reaches nothing that matters.
    const std::size_t offered = c.log->readiness.size();
    versioned::ContinuePreparation more{static_cast<std::int64_t>(t.id.value)};
    bus.send_as(c.coordinator.id, c.candidate, Message(to_value(more)));
    bus.pump();
    CHECK(c.log->readiness.size() == offered);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.role_holder(kRole) == c.incumbent);
    CHECK(bus.alive(c.incumbent));
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(bus.role_holder(kRole) != c.candidate);
}

TEST_CASE("R2B-3b-3: the role drifting under a live preparation refuses the readiness") {
    // THE TERM NO LIFECYCLE CASE CAN REACH. Every death, revival and reload aborts
    // the transaction outright, so readiness validation never sees a drifted role
    // by those roads — which is exactly why R2B-3b-2's role-drift mutation stayed
    // GREEN, and why it needs a road of its own.
    //
    // There is one: `admit_candidate` is the sole admission mutation and a trusted
    // host may call it directly, outside any transaction. Doing so moves the role
    // away from our incumbent and seals it — while leaving its life and its code
    // untouched, so nothing announces anything and our transaction survives to
    // meet the drift at validation time.
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult t = bus.begin_prepared_replacement(c.op.id, c.coordinator.id, c.incumbent,
                                                       c.candidate, kRole, 16);
    REQUIRE(t.ok);
    offer_readiness_twice(bus, c); // the second offer is what pins the consumption
    ask_to_prepare(bus, c, t.id, "defer");

    // A second sealed candidate, admitted against OUR incumbent by a direct host
    // call. Nobody's life changed; the slot simply moved on.
    auto usurper = std::make_unique<VersionedService>("v9");
    const WeaveId usurper_id = bus.register_weave(std::move(usurper), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(usurper_id, c.coordinator.id));
    loom::Activated fact{7};
    REQUIRE(bus.admit_candidate(usurper_id, c.incumbent, kRole, host_lifecycle_authority(bus),
                                Message(to_value(fact)), 7));
    REQUIRE(bus.role_holder(kRole) == usurper_id);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing); // nothing announced it

    continue_preparation(bus, c, t.id);
    REQUIRE(c.log->readiness.size() == 2);
    CHECK_FALSE(c.log->readiness[0].ok);
    CHECK(c.log->readiness[0].why == TxnReason::RoleChanged);
    // AND THE ANSWER IS SPENT ANYWAY. It was authentic and it was heard; a
    // validation that refuses for a reason about the WORLD does not hand the
    // conversation back. This is the only road to the consumed-conversation term:
    // everywhere else an accepted readiness has already moved the state machine,
    // so `WrongState` answers first.
    CHECK_FALSE(c.log->readiness[1].ok);
    CHECK(c.log->readiness[1].why == TxnReason::InvalidReadiness);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing); // refused, never ended
    CHECK(bus.role_holder(kRole) == usurper_id);               // and nothing moved back
}

// ---- R2B-3b-3: the dynamic `versioned.service` proof ---------------------------
//
// Everything above is native. This is the phase's real subject: two loaded
// artifacts, a service that never stops answering "v1", and a successor that
// receives a private preparation ask, keeps the answer across other deliveries,
// and answers for itself before it is allowed anywhere near the world.

namespace {

constexpr const char* kService = "versioned.service";

/// The candidate's state contract, spelled out here on purpose: the test must not
/// share a definition with the artifact it is interrogating, or a drift in either
/// would cancel out. (Same discipline as the R2B-2 steward's Counter v4.)
std::shared_ptr<const Schema> versioned_state_schema() {
    static const auto s = SchemaBuilder("VersionedState", 1)
                              .field("served", Kind::Int)
                              .field("prepares", Kind::Int)
                              .field("continues", Kind::Int)
                              .field("deferred", Kind::Int)
                              .field("answered", Kind::Int)
                              .field("activations", Kind::Int)
                              .field("last_activation", Kind::Int)
                              .field("escapes", Kind::Int)
                              .field("retired", Kind::Int)
                              .field("token", Kind::Int)
                              .field("plan", Kind::Text)
                              .build();
    return s;
}

Value versioned_state(Switchboard& bus, WeaveId id) {
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, versioned_state_schema());
    REQUIRE(a.ok());
    return std::move(a).value();
}

std::int64_t state_field(Switchboard& bus, WeaveId id, const char* field) {
    return versioned_state(bus, id).get(field)->as_int();
}

/// The whole dynamic cast: real artifacts for both services, native probes for the
/// three roles the substrate does not care about (operator, coordinator, observer).
struct DynCast {
    Registered op;
    Registered coordinator;
    Registered observer;
    WeaveId incumbent{};
    WeaveId candidate{};
    std::shared_ptr<CastLog> log = std::make_shared<CastLog>();
    std::shared_ptr<TxnId> live_txn = std::make_shared<TxnId>();

    std::string ask(Switchboard& bus) {
        const std::size_t before = log->answers.size();
        bus.send_as_to_role(observer.id, kService,
                            Message(to_value(versioned::QueryVersion{1}), observer.id,
                                    observer.id, 0));
        bus.pump();
        return log->answers.size() > before ? log->answers.back()
                                            : std::string("<no answer>");
    }
    TxnResult last_readiness() const {
        return log->readiness.empty() ? TxnResult{} : log->readiness.back();
    }
};

/// Load both artifacts and wire the same credulous coordinator the native cases
/// use. `load_candidate` is the ordinary load followed by the seal — so v2 is
/// built by exactly the code that builds every other weave, and the object that
/// prepares is the object that goes live.
DynCast load_pair(Switchboard& bus, Kernel& kernel) {
    DynCast d{register_probe(bus, {pong_schema()}),
              register_probe(bus, {schema_of<versioned::CandidateReady>(),
                                   schema_of<versioned::CandidateRefused>(),
                                   schema_of<versioned::VersionReply>()}),
              register_probe(bus, {schema_of<versioned::VersionReply>()})};
    std::shared_ptr<CastLog> log = d.log;
    d.observer.weave->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        const Cell* v = in.payload.get("version");
        log->answers.push_back(v == nullptr ? std::string("<none>") : std::string(v->as_text()));
    };
    std::shared_ptr<TxnId> live = d.live_txn;
    d.coordinator.weave->on_handle = [&bus, log, live](const Message& in, Bus&, ProbeWeave&) {
        if (!live->valid()) {
            return;
        }
        const Cell* claimed = in.payload.get("transaction");
        const TxnId named = claimed == nullptr
                                ? *live
                                : TxnId{static_cast<std::uint64_t>(claimed->as_int())};
        const bool refusal =
            in.payload.schema().name() == std::string_view(versioned::CandidateRefused::zen_name);
        log->readiness.push_back(bus.accept_preparation_answer(
            named, refusal ? PreparationAnswer::Refused : PreparationAnswer::Ready));
    };

    LoadResult v1 = kernel.load("v1", ZEN_SO_VERSIONED_V1, kService);
    REQUIRE(v1.ok);
    d.incumbent = v1.id;
    LoadResult v2 = kernel.load_candidate("v2", ZEN_SO_VERSIONED_V2, d.coordinator.id);
    REQUIRE(v2.ok);
    d.candidate = v2.id;
    return d;
}

/// Ask the real artifact to prepare. `escape_to` is the stranger it will try to
/// reach on its way; every ask carries one, so the isolation regression runs on
/// every path rather than in one case that could rot.
void dyn_ask(Switchboard& bus, DynCast& d, TxnId id, const char* plan) {
    *d.live_txn = id;
    versioned::PrepareReplacement ask;
    ask.transaction = static_cast<std::int64_t>(id.value);
    ask.plan = plan;
    ask.escape_to = static_cast<std::int64_t>(d.observer.id.value);
    REQUIRE(bus.ask_candidate_to_prepare(id, Message(to_value(ask))).ok);
    bus.pump();
}

void dyn_continue(Switchboard& bus, DynCast& d, TxnId id) {
    bus.send_as(d.coordinator.id, d.candidate,
                Message(to_value(versioned::ContinuePreparation{
                    static_cast<std::int64_t>(id.value)})));
    bus.pump();
}

/// Everything an ordinary observer could notice about the incumbent, asked of the
/// bus rather than of any object a failing case might already have destroyed.
void v1_still_the_service(Switchboard& bus, DynCast& d) {
    CHECK(bus.alive(d.incumbent));
    CHECK_FALSE(bus.sealed(d.incumbent));
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK(d.ask(bus) == "v1");
    CHECK(bus.role_holder(kService) != d.candidate);
}

/// A tap that remembers, in order, every delivery the candidate actually received.
struct CandidateTap {
    std::vector<std::string> got;
};

/// Announce a real, Loom-attested activation for `target`.
///
/// A lifecycle authority can only be SPENT through a weave's own bus — that is the
/// R2B-1a boundary — so a one-shot herald does the honours, which is the same road
/// `zen::control` takes in production. There is deliberately no host shortcut.
void announce_activation(Switchboard& bus, WeaveId target, std::int64_t sequence) {
    Registered herald = register_probe(bus, {tick_schema()});
    const LifecycleAuthority authority = host_lifecycle_authority(bus);
    herald.weave->on_handle = [authority, target, sequence](const Message&, Bus& wb, ProbeWeave&) {
        wb.announce_lifecycle(authority, target, Message(to_value(loom::Activated{sequence})),
                              sequence);
    };
    bus.send(herald.id, Message(tick(1)));
    bus.pump();
    (void)bus.unregister_weave(herald.id); // its one errand is done
}

} // namespace

TEST_CASE("R2B-3b-3: a sealed dynamic candidate prepares across deliveries, answers for itself, "
          "and only then becomes the service") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    // Every refusal the sealed candidate earns, counted at the source.
    std::vector<std::string> sealed_speech;
    bus.add_observer([&sealed_speech](const BusEvent& e) {
        if (e.kind == EventKind::Refused && e.refusal.reason == RefusalReason::SealedSpeech) {
            sealed_speech.push_back(e.schema_name);
        }
    });

    // 1-3. The incumbent is loaded, activated, holds the role, and answers "v1".
    announce_activation(bus, d.incumbent, 1);
    CHECK(state_field(bus, d.incumbent, "activations") == 1);
    REQUIRE(d.ask(bus) == "v1");

    // 4-5. A sealed candidate, and one transaction naming all four participants.
    CHECK(bus.sealed(d.candidate));
    CHECK(bus.candidate_owner(d.candidate).who == d.coordinator.id);
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);

    // 6-8. The ask goes through the coordinator-only door; the candidate reaches
    // for the world on its way, defers its answer, and is still nobody.
    dyn_ask(bus, d, t.id, "defer");
    CHECK(state_field(bus, d.candidate, "prepares") == 1);
    CHECK(state_field(bus, d.candidate, "deferred") == 1);
    CHECK_FALSE(state_field(bus, d.candidate, "answered"));
    CHECK(state_field(bus, d.candidate, "escapes") == 4); // it really did try
    // ...and three of the four were REFUSED as sealed speech: the publication, the
    // role-addressed send, and the direct send to a stranger. The fourth — a
    // domain message to its own coordinator — is delivered, because a seal is a
    // conversation and not a quarantine.
    CHECK(sealed_speech.size() == 3);
    CHECK(bus.transaction_state(t.id) == TxnState::Preparing);
    CHECK(bus.sealed(d.candidate));
    CHECK(bus.role_holder(kService) != d.candidate);
    CHECK(state_field(bus, d.candidate, "served") == 0);      // it answered no production
    CHECK(state_field(bus, d.candidate, "activations") == 0); // and was told nothing
    // The one thing it CAN say — an ordinary message to its own coordinator — was
    // offered to the bus as readiness by a credulous coordinator, and refused.
    REQUIRE(d.log->readiness.size() == 1);
    CHECK(d.last_readiness().why == TxnReason::InvalidReadiness);

    // 9-10. Unrelated traffic runs, and the incumbent is still the service.
    CHECK(d.ask(bus) == "v1");
    REQUIRE(bus.tick_preparation(t.id).ok);
    CHECK(d.ask(bus) == "v1");

    // 11-15. The continuation arrives; the candidate spends what it kept; the bus
    // — not the coordinator — decides the answer is this transaction's.
    dyn_continue(bus, d, t.id);
    CHECK(state_field(bus, d.candidate, "continues") == 1);
    CHECK(state_field(bus, d.candidate, "answered") == 1);
    REQUIRE(d.log->readiness.size() == 2);
    CHECK(d.last_readiness().ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    CHECK(d.ask(bus) == "v1"); // STILL. Readiness is not admission.
    CHECK(bus.role_holder(kService) == d.incumbent);

    // 16-19. Commit delegates to admit_candidate, and no observer sees a gap.
    const std::int64_t served_before = state_field(bus, d.incumbent, "served");
    loom::Activated fact{2};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 2).ok);
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(bus.sealed(d.incumbent));     // sealed for retirement, not merely renamed
    CHECK_FALSE(bus.sealed(d.candidate));
    bus.pump();
    CHECK(state_field(bus, d.candidate, "activations") == 1);
    CHECK(state_field(bus, d.candidate, "last_activation") == 2);

    // 20-23. The new production answer, exactly one Committed result, and no
    // second commit or second collection.
    CHECK(d.ask(bus) == "v2");
    CHECK(state_field(bus, d.incumbent, "served") == served_before); // it answers no more
    exactly_one_outcome(bus, d.op.id, t.id, TxnState::Committed, TxnReason::None);
    CHECK_FALSE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                                Message(to_value(fact)), 3).ok);
    CHECK(bus.active_transactions() == 0);
}

TEST_CASE("R2B-3b-3: the same readiness, answered inside the preparation handler") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);

    dyn_ask(bus, d, t.id, "ready");
    CHECK(state_field(bus, d.candidate, "continues") == 0); // no later delivery was needed
    CHECK(state_field(bus, d.candidate, "deferred") == 0);  // and no slot was borrowed
    CHECK(state_field(bus, d.candidate, "token") == 0);
    CHECK(d.last_readiness().ok);
    CHECK(bus.transaction_state(t.id) == TxnState::Ready);
    CHECK(d.ask(bus) == "v1");

    loom::Activated fact{1};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 1).ok);
    bus.pump();
    CHECK(d.ask(bus) == "v2");
    exactly_one_outcome(bus, d.op.id, t.id, TxnState::Committed, TxnReason::None);
}

TEST_CASE("R2B-3b-3: queued production waiting on the role reaches the new service only after "
          "its activation") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    CandidateTap tap;
    const WeaveId watch = d.candidate;
    bus.add_observer([&tap, watch](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.target == watch) {
            tap.got.push_back(e.schema_name);
        }
    });
    REQUIRE(d.ask(bus) == "v1");

    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    REQUIRE(bus.transaction_state(t.id) == TxnState::Ready);

    // Real production, addressed to the ROLE, queued while the incumbent still
    // holds it — and resolved at delivery, which is after the commit.
    bus.send_as_to_role(d.observer.id, kService,
                        Message(to_value(versioned::QueryVersion{9}), d.observer.id,
                                d.observer.id, 0));
    REQUIRE(bus.pending() == 1);

    loom::Activated fact{5};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 5).ok);
    bus.pump();

    // The activation was INSERTED AHEAD of traffic that was already waiting — the
    // narrowest placement that works, and nothing was dropped to achieve it.
    REQUIRE(tap.got.size() >= 2);
    CHECK(tap.got[tap.got.size() - 2] == std::string(loom::Activated::zen_name));
    CHECK(tap.got.back() == std::string(versioned::QueryVersion::zen_name));
    REQUIRE_FALSE(d.log->answers.empty());
    CHECK(d.log->answers.back() == "v2"); // the queued query was answered by v2
    CHECK(state_field(bus, d.candidate, "activations") == 1);
}

TEST_CASE("R2B-3b-3: a real candidate's refusal ends it, and v1 never notices") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    const char* plan = "refuse";
    SUBCASE("refused immediately") { plan = "refuse"; }
    SUBCASE("refused after deferring") { plan = "defer-refuse"; }
    SUBCASE("refused because the plan made no sense") { plan = "sing-a-song"; }

    REQUIRE(d.ask(bus) == "v1");
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, plan);
    if (std::string(plan) == "defer-refuse") {
        CHECK(bus.transaction_active(t.id));
        dyn_continue(bus, d, t.id);
    }

    CHECK(d.last_readiness().ok);
    CHECK(d.last_readiness().why == TxnReason::CandidateRefused);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    CHECK_FALSE(bus.alive(d.candidate));
    exactly_one_outcome(bus, d.op.id, t.id, TxnState::Aborted, TxnReason::CandidateRefused);
    v1_still_the_service(bus, d);
    // AND THE KERNEL'S BOOKS FOLLOWED (R2B-3b-3a). The transaction discarded the
    // candidate; nobody called the Kernel; the artifact is gone from it anyway.
    // Before this phase the record survived, so `unload("v2")` answered true and
    // meant "I closed a library whose weave had already been destroyed".
    CHECK_FALSE(kernel.is_loaded("v2"));
    CHECK(kernel.status("v2") == ArtifactStatus::NotLoaded);
    CHECK_FALSE(kernel.unload("v2")); // truthfully not loaded, rather than falsely tidy
}

TEST_CASE("R2B-3b-3: every pre-commit failure leaves v1 serving and the candidate outside") {
    int how = 0;
    SUBCASE("the candidate dies while holding the answer") { how = 0; }
    SUBCASE("the candidate's code is replaced") { how = 1; }
    SUBCASE("the coordinator dies") { how = 2; }
    SUBCASE("the incumbent dies") { how = 3; }
    SUBCASE("the operator dies") { how = 4; }
    SUBCASE("the operator is explicitly abandoned") { how = 5; }
    SUBCASE("preparation runs out of budget") { how = 6; }
    SUBCASE("the incumbent's code is replaced") { how = 7; }

    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 3);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "defer");
    REQUIRE(state_field(bus, d.candidate, "deferred") == 1);
    const std::size_t offered = d.log->readiness.size();

    TxnReason expected = TxnReason::CandidateChanged;
    bool operator_can_collect = true;
    if (how == 0) {
        bus.kill(d.candidate);
    } else if (how == 1) {
        REQUIRE(bus.swap_state(d.candidate, bus.snapshot_bytes(d.candidate)).revived);
    } else if (how == 2) {
        bus.kill(d.coordinator.id);
        expected = TxnReason::CoordinatorChanged;
    } else if (how == 3) {
        bus.kill(d.incumbent);
        expected = TxnReason::IncumbentChanged;
    } else if (how == 4) {
        bus.kill(d.op.id);
        expected = TxnReason::OperatorChanged;
        operator_can_collect = false; // a result is kept for a life that has ended
    } else if (how == 5) {
        REQUIRE(bus.abort_prepared_replacement(t.id, d.op.id).ok);
        expected = TxnReason::ExplicitAbort;
    } else if (how == 6) {
        REQUIRE(bus.tick_preparation(t.id).ok);
        REQUIRE(bus.tick_preparation(t.id).ok);
        CHECK_FALSE(bus.tick_preparation(t.id).ok);
        expected = TxnReason::PreparationExhausted;
    } else {
        // Its code is replaced in place — it never stopped living, and it is
        // still not the participant the transaction bound.
        REQUIRE(bus.swap_state(d.incumbent, bus.snapshot_bytes(d.incumbent)).revived);
        expected = TxnReason::IncumbentChanged;
    }

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0); // capacity reclaimed
    if (operator_can_collect) {
        exactly_one_outcome(bus, d.op.id, t.id, TxnState::Aborted, expected);
    }

    // The retained authority is worthless from here: there is no transaction to
    // name, and in every case but the incumbent's death the candidate is gone.
    bus.send_as(d.coordinator.id, d.candidate,
                Message(to_value(versioned::ContinuePreparation{
                    static_cast<std::int64_t>(t.id.value)})));
    bus.pump();
    CHECK(d.log->readiness.size() == offered);
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.role_holder(kService) != d.candidate);

    if (how != 3) { // the incumbent's own death is the one case where it stopped
        v1_still_the_service(bus, d);
    } else {
        CHECK_FALSE(bus.alive(d.incumbent));
        CHECK_FALSE(bus.sealed(d.incumbent)); // aborting is not retirement
    }
    if (how == 7) { // ...and replaced code is still the service, under its own name
        CHECK(state_field(bus, d.incumbent, "served") > 0);
    }
    // EVERY ROUTE RELEASES THE ARTIFACT (R2B-3b-3a). Aborting discards the sealed
    // candidate whatever ended the transaction, so the Kernel's books follow on
    // all eight — with no cleanup call, from a lifecycle change it never saw.
    CHECK_FALSE(kernel.is_loaded("v2"));
    CHECK(kernel.status("v2") == ArtifactStatus::NotLoaded);
    CHECK(kernel.unload("v1"));
}

TEST_CASE("R2B-3b-3: a candidate artifact that cannot load never becomes a candidate") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");

    // An artifact built against the PREVIOUS ABI: refused at load, so there is
    // nothing to seal, nothing to name in a transaction, and nothing to undo.
    LoadResult stale = kernel.load_candidate("stale", ZEN_SO_STALEABI, d.coordinator.id);
    CHECK_FALSE(stale.ok);
    CHECK_FALSE(kernel.is_loaded("stale"));
    CHECK_FALSE(stale.id.valid());

    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       stale.id, kService, 8);
    CHECK_FALSE(t.ok);
    CHECK(t.why == TxnReason::PreconditionFailed);
    CHECK(bus.active_transactions() == 0);
    v1_still_the_service(bus, d);
}

TEST_CASE("R2B-3b-3: after a successful commit, retirement failing changes nothing about the "
          "new service") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    loom::Activated fact{1};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 1).ok);
    bus.pump();
    REQUIRE(d.ask(bus) == "v2");
    exactly_one_outcome(bus, d.op.id, t.id, TxnState::Committed, TxnReason::None);

    // RETIREMENT IS A CONVERSATION THE OPERATOR OWNS, and the substrate's only
    // part in it is that a sealed weave can still hear its coordinator. Here it
    // works, and then it stops working — the retired incumbent dies before the
    // word reaches it.
    bus.send_as(d.coordinator.id, d.incumbent, Message(to_value(versioned::RetireNow{1})));
    bus.pump();
    CHECK(state_field(bus, d.incumbent, "retired") == 1);

    bus.kill(d.incumbent);
    const Ticket late = bus.send_as(d.coordinator.id, d.incumbent,
                                    Message(to_value(versioned::RetireNow{2})));
    bus.pump();
    // ONE TRUTHFUL DIAGNOSTIC, and no rollback of anything.
    CHECK(bus.outcome(late).disposition == Disposition::Refused);
    CHECK(bus.outcome(late).refusal.reason == RefusalReason::TargetUnavailable);
    CHECK(bus.role_holder(kService) == d.candidate); // the candidate is still the service
    CHECK(d.ask(bus) == "v2");
    CHECK(bus.sealed(d.incumbent));                  // sealed wreckage, exactly as it was
    TxnOutcome second{};
    CHECK_FALSE(bus.take_outcome(d.op.id, second));  // and no second transaction result
    CHECK(bus.active_transactions() == 0);
}

TEST_CASE("R2B-3b-3: the artifact contracts are the real ones, at preparation and at commit") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);

    // What the bus PUBLISHED for each artifact — the same list delivery is matched
    // against, not a manifest read separately for the test.
    const auto names = [&bus](WeaveId id) {
        std::vector<std::string> out;
        for (const auto& s : bus.accepted_schemas(id)) {
            out.push_back(s->name() + " v" + std::to_string(s->version()));
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const std::vector<std::string> incumbent_contract{"QueryVersion v1", "RetireNow v1",
                                                      "zen.Activated v1"};
    const std::vector<std::string> candidate_contract{
        "ContinuePreparation v1", "PrepareReplacement v1", "QueryVersion v1", "RetireNow v1",
        "zen.Activated v1"};
    CHECK(names(d.incumbent) == incumbent_contract);
    CHECK(names(d.candidate) == candidate_contract);

    // NO WILDCARD ACCEPTANCE: a shape the candidate does not declare is refused
    // even from its own coordinator, which is the only party that can reach it.
    const Ticket undeclared =
        bus.send_as(d.coordinator.id, d.candidate, Message(to_value(versioned::CandidateReady{1})));
    bus.pump();
    CHECK(bus.outcome(undeclared).refusal.reason == RefusalReason::NotAccepted);

    // ...and the contract does not change at admission. The list after commit is
    // the list that prepared.
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    loom::Activated fact{1};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 1).ok);
    bus.pump();
    CHECK(names(d.candidate) == candidate_contract);
    CHECK(d.ask(bus) == "v2");
}

// ---- R2B-3b-3a: the Kernel's books follow reality ---------------------------
//
//   When the Switchboard destroys a Kernel-loaded weave, the Kernel releases its
//   artifact record and library exactly once.
//
//   When the Switchboard changes production topology, the Kernel's queries and
//   cleanup immediately agree with it.
//
// The Switchboard already changed reality correctly. What follows proves the
// Kernel's records describe the world that actually exists.

namespace {

/// THE HOST-SIDE LIFETIME LEDGER, READ AS A DELTA. The counts are process-wide
/// and monotonic, so an absolute number says nothing about one operation; an
/// "exactly once" law needs the difference either side of the act. Constructed
/// where the measurement starts.
struct LifetimeDelta {
    KernelLifetimeCounts before = kernel_lifetime_counts();

    std::uint64_t created() const {
        return kernel_lifetime_counts().instances_created - before.instances_created;
    }
    std::uint64_t destroyed() const {
        return kernel_lifetime_counts().instances_destroyed - before.instances_destroyed;
    }
    std::uint64_t opened() const {
        return kernel_lifetime_counts().libraries_opened - before.libraries_opened;
    }
    std::uint64_t closed() const {
        return kernel_lifetime_counts().libraries_closed - before.libraries_closed;
    }
};

/// Everything the Kernel can be asked about an artifact it must no longer hold.
/// One place, because "released" is a conjunction and checking three of its five
/// terms is how a partial release reads as a clean one.
void artifact_released(Kernel& kernel, const char* name, const char* path) {
    CHECK_FALSE(kernel.is_loaded(name));
    CHECK(kernel.status(name) == ArtifactStatus::NotLoaded);
    CHECK_FALSE(kernel.weave_id(name).valid());
    CHECK(kernel.role_of(name).empty());
    const std::vector<std::string> names = kernel.loaded();
    CHECK(std::find(names.begin(), names.end(), std::string(name)) == names.end());
    // ...and the two operations that would have touched stale state answer
    // truthfully instead. `reload_from` is the one that MATTERED: unload never
    // dereferenced the adapter, and reload does, on its very first line.
    CHECK_FALSE(kernel.unload(name));
    const ReloadResult r = kernel.reload_from(name, path);
    CHECK_FALSE(r.ok);
    CHECK(r.error == std::string("not loaded: ") + name);
}

/// The role query, asked with a shape ONLY the candidate declares — so the
/// answer distinguishes the two artifacts on both of its fields rather than
/// agreeing by accident.
Kernel::RoleQuery service_query(const Kernel& kernel) {
    return kernel.query_role(kService, versioned::PrepareReplacement::zen_name,
                             versioned::PrepareReplacement::zen_version);
}

} // namespace

TEST_CASE("R2B-3b-3a: a lifecycle-driven abort releases the candidate's artifact, and nobody "
          "had to ask") {
    int route = 0;
    const char* plan = "defer";
    SUBCASE("the candidate refuses, authentically") { route = 0; plan = "refuse"; }
    SUBCASE("the candidate dies") { route = 1; }
    SUBCASE("the candidate's code is replaced") { route = 2; }
    SUBCASE("the coordinator dies") { route = 3; }
    SUBCASE("the operator dies") { route = 4; }
    SUBCASE("the incumbent's code is replaced") { route = 5; }
    SUBCASE("preparation runs out of budget") { route = 6; }
    SUBCASE("the operator aborts explicitly") { route = 7; }

    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const WeaveId doomed = d.candidate;

    // Measured from here: both artifacts are already open, and nothing below
    // opens or creates anything. So the whole delta belongs to the abort.
    LifetimeDelta ledger;

    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 3);
    REQUIRE(t.ok);
    // BEFORE: the Kernel and the Switchboard agree about both artifacts.
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);
    CHECK(kernel.role_of("v1") == kService);
    CHECK(kernel.role_of("v2").empty());
    CHECK(service_query(kernel).holder == d.incumbent);

    dyn_ask(bus, d, t.id, plan);

    // THE ABORT. Every route but the first is a lifecycle change the Kernel never
    // sees and never could: no Kernel call is made from here to the end.
    if (route == 1) {
        bus.kill(doomed);
    } else if (route == 2) {
        REQUIRE(bus.swap_state(doomed, bus.snapshot_bytes(doomed)).revived);
    } else if (route == 3) {
        bus.kill(d.coordinator.id);
    } else if (route == 4) {
        bus.kill(d.op.id);
    } else if (route == 5) {
        REQUIRE(bus.swap_state(d.incumbent, bus.snapshot_bytes(d.incumbent)).revived);
    } else if (route == 6) {
        REQUIRE(bus.tick_preparation(t.id).ok);
        REQUIRE(bus.tick_preparation(t.id).ok);
        CHECK_FALSE(bus.tick_preparation(t.id).ok);
    } else if (route == 7) {
        REQUIRE(bus.abort_prepared_replacement(t.id, d.op.id).ok);
    }

    // The Switchboard's side, as R2B-3b-3 left it.
    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.active_transactions() == 0);
    CHECK(bus.weave(doomed) == nullptr); // the candidate is not merely dead: it is gone
    CHECK_FALSE(bus.alive(doomed));
    CHECK(bus.role_holder(kService) == d.incumbent);

    // THE KERNEL'S SIDE — WITHOUT A CLEANUP CALL. This is the phase.
    artifact_released(kernel, "v2", ZEN_SO_VERSIONED_V2);
    CHECK(kernel.is_loaded("v1")); // and the incumbent's artifact is untouched
    CHECK(kernel.role_of("v1") == kService);

    // EXACTLY ONCE, both of them. One instance destroyed (the candidate's), one
    // library closed (the candidate's) — and nothing created or opened, so a
    // second destruction anywhere would show up here as a two.
    CHECK(ledger.created() == 0);
    CHECK(ledger.opened() == 0);
    CHECK(ledger.destroyed() == 1);
    CHECK(ledger.closed() == 1);

    // The name is free, and the new artifact is a fresh live record — not the old
    // one revived. (Loaded ordinarily: route 3 killed the coordinator, and a dead
    // coordinator cannot own a seal.)
    REQUIRE(kernel.load("v2", ZEN_SO_VERSIONED_V2).ok);
    CHECK(kernel.status("v2") == ArtifactStatus::Live);
    CHECK(kernel.weave_id("v2") != doomed);
    CHECK(ledger.created() == 1);
    CHECK(ledger.opened() == 1);

    // ...and v1 is still the service throughout, except where v1 itself was the
    // thing that changed.
    if (route != 5) {
        CHECK(d.ask(bus) == "v1");
    }
}

TEST_CASE("R2B-3b-3a: shutdown after a lifecycle-driven abort repeats no destruction") {
    Switchboard bus;
    LifetimeDelta ledger;
    WeaveId doomed{};
    {
        Kernel kernel(bus);
        DynCast d = load_pair(bus, kernel);
        doomed = d.candidate;
        const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                           d.candidate, kService, 8);
        REQUIRE(t.ok);
        dyn_ask(bus, d, t.id, "defer");
        REQUIRE(bus.abort_prepared_replacement(t.id, d.op.id).ok);
        REQUIRE_FALSE(kernel.is_loaded("v2"));
        CHECK(ledger.destroyed() == 1);
        CHECK(ledger.closed() == 1);

        // A fourth artifact with no connection to any of this, so the destructor
        // has real work to do as well as an absence to not repeat.
        REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
        // the Kernel is destroyed here
    }
    CHECK(bus.weave(doomed) == nullptr);
    // Balanced: every instance the host created was destroyed once, every library
    // it opened was closed once. A destructor walking a stale record would push
    // `destroyed` or `closed` one past `created`/`opened`.
    CHECK(ledger.created() == ledger.destroyed());
    CHECK(ledger.opened() == ledger.closed());
    CHECK(ledger.created() == 3); // v1, v2, t
    CHECK(ledger.opened() == 3);
}

TEST_CASE("R2B-3b-3a: a released candidate name is reusable, and nothing of the old artifact "
          "comes back with it") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");

    std::vector<WeaveId> was;
    TxnId stale_txn{};
    for (int round = 0; round < 3; ++round) {
        if (round > 0) {
            REQUIRE(kernel.load_candidate("v2", ZEN_SO_VERSIONED_V2, d.coordinator.id).ok);
            d.candidate = kernel.weave_id("v2");
        }
        was.push_back(d.candidate);
        const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                           d.candidate, kService, 8);
        REQUIRE(t.ok);
        if (round == 0) {
            stale_txn = t.id;
        }
        dyn_ask(bus, d, t.id, "defer");
        REQUIRE(bus.abort_prepared_replacement(t.id, d.op.id).ok);
        REQUIRE_FALSE(kernel.is_loaded("v2"));
        v1_still_the_service(bus, d);
    }

    // IDENTITY IS THE WeaveId, NEVER AN ADDRESS. Three artifacts wore the same
    // name in the same allocator, and very possibly the same memory — this test
    // deliberately does not care, and asserts nothing about addresses. What it
    // asserts is that each predecessor's id resolves to nothing, which stays true
    // however the allocator behaved.
    CHECK(was[0] != was[1]);
    CHECK(was[1] != was[2]);
    for (const WeaveId old : was) {
        CHECK(bus.weave(old) == nullptr);
        CHECK_FALSE(bus.alive(old));
        CHECK(bus.role_of(old).empty());
        const Ticket t = bus.send(old, Message(to_value(versioned::QueryVersion{1})));
        bus.pump();
        CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
    }

    // A fourth load under the same name gets a genuinely fresh live record — one
    // more id nobody has held, sealed to the same coordinator, and unloadable
    // like any other artifact.
    REQUIRE(kernel.load_candidate("v2", ZEN_SO_VERSIONED_V2, d.coordinator.id).ok);
    const WeaveId fresh = kernel.weave_id("v2");
    CHECK(std::find(was.begin(), was.end(), fresh) == was.end());
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);
    CHECK(bus.candidate_owner(fresh).who == d.coordinator.id);
    CHECK(kernel.unload("v2"));
    artifact_released(kernel, "v2", ZEN_SO_VERSIONED_V2);

    // ...and the first round's transaction id is worthless against the third
    // round's artifact: it named a transaction that ended, not a slot to reuse.
    CHECK_FALSE(bus.transaction_active(stale_txn));
    const TxnResult late = bus.ask_candidate_to_prepare(
        stale_txn, Message(to_value(versioned::PrepareReplacement{})));
    CHECK_FALSE(late.ok);
    CHECK(late.why == TxnReason::LateReadiness);
}

TEST_CASE("R2B-3b-3a: the committed candidate is the service, in the Kernel's books as well as "
          "the Switchboard's") {
    Switchboard bus;
    Kernel kernel(bus);
    LifetimeDelta ledger;
    // 1-2. The incumbent, and a sealed candidate beside it.
    DynCast d = load_pair(bus, kernel);
    announce_activation(bus, d.incumbent, 1);
    REQUIRE(d.ask(bus) == "v1");

    // 5. BEFORE COMMIT the two layers agree: v1 owns production, v2 is loaded and
    //    sealed. Asked with a shape only v2 declares, so both fields discriminate.
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);
    CHECK(kernel.role_of("v1") == kService);
    CHECK(kernel.role_of("v2").empty());
    CHECK(service_query(kernel).holder == d.incumbent);
    CHECK_FALSE(service_query(kernel).accepts);

    // 3-4. One transaction, prepared and authenticated by the candidate itself.
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    REQUIRE(bus.transaction_state(t.id) == TxnState::Ready);
    CHECK(service_query(kernel).holder == d.incumbent); // readiness is not admission

    // 6-7. Commit, which delegates to activation-first admission.
    loom::Activated fact{2};
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 2).ok);

    // 8-12. IMMEDIATELY — no pump, no later host call, no reconciliation pass.
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
    const Kernel::RoleQuery q = service_query(kernel);
    CHECK(q.holder == d.candidate);
    CHECK(q.accepts); // and the answer's SECOND field moved with it
    CHECK(kernel.is_loaded("v1"));
    CHECK(kernel.status("v1") == ArtifactStatus::Sealed); // retirement-private, still loaded
    CHECK(kernel.status("v2") == ArtifactStatus::Live);

    // The four things the Kernel must never say about a committed topology.
    CHECK(service_query(kernel).holder.valid());            // never "no holder"
    CHECK_FALSE(kernel.role_of("v1") == kernel.role_of("v2")); // never both
    CHECK_FALSE(kernel.status("v1") == ArtifactStatus::Live);  // incumbent is not live production
    CHECK_FALSE(kernel.role_of("v2").empty());                 // candidate is not roleless

    // 13. And production answers "v2".
    bus.pump();
    CHECK(d.ask(bus) == "v2");
    CHECK(state_field(bus, d.candidate, "activations") == 1);

    // 14-15. unload_role selects the LIVE holder — the candidate — not whoever
    //        was loaded under that role's name.
    LifetimeDelta unloading;
    REQUIRE(kernel.unload_role(kService));
    artifact_released(kernel, "v2", ZEN_SO_VERSIONED_V2);
    CHECK(kernel.is_loaded("v1")); // the retired incumbent was NOT the one selected
    CHECK(unloading.destroyed() == 1);
    CHECK(unloading.closed() == 1);
    CHECK(bus.role_holder(kService) == WeaveId{}); // the slot is free for a successor

    // 16-17. The retired incumbent unloads explicitly, and each artifact's
    //        instance and library went exactly once.
    REQUIRE(kernel.unload("v1"));
    artifact_released(kernel, "v1", ZEN_SO_VERSIONED_V1);
    CHECK(ledger.created() == 2);
    CHECK(ledger.destroyed() == 2);
    CHECK(ledger.opened() == 2);
    CHECK(ledger.closed() == 2);
}

TEST_CASE("R2B-3b-3a: unloading the retired incumbent does not disturb the new service") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    REQUIRE(bus.commit_prepared_replacement(t.id, host_lifecycle_authority(bus),
                                            Message(to_value(loom::Activated{3})), 3).ok);
    bus.pump();
    REQUIRE(d.ask(bus) == "v2");

    LifetimeDelta ledger;
    REQUIRE(kernel.unload("v1"));
    CHECK(ledger.destroyed() == 1);
    CHECK(ledger.closed() == 1);
    artifact_released(kernel, "v1", ZEN_SO_VERSIONED_V1);

    // The successor is untouched by its predecessor's retirement.
    CHECK(kernel.status("v2") == ArtifactStatus::Live);
    CHECK(kernel.role_of("v2") == kService);
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(d.ask(bus) == "v2");
}

TEST_CASE("R2B-3b-3a: a role moved by DIRECT admission — no transaction at all — is seen by the "
          "Kernel immediately") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    REQUIRE(service_query(kernel).holder == d.incumbent);

    // `admit_candidate` is the sole admission mutation, and a prepared
    // transaction is not its only caller: a trusted host may move production
    // topology directly. The synchronization must not be a property of one call
    // site — so this exercises the road that bypasses the transaction layer
    // entirely, and the Kernel is never told.
    REQUIRE(bus.admit_candidate(d.candidate, d.incumbent, kService,
                                host_lifecycle_authority(bus),
                                Message(to_value(loom::Activated{7})), 7));

    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
    const Kernel::RoleQuery q = service_query(kernel);
    CHECK(q.holder == d.candidate);
    CHECK(q.accepts);
    CHECK(kernel.status("v1") == ArtifactStatus::Sealed);
    CHECK(kernel.status("v2") == ArtifactStatus::Live);
    bus.pump();
    CHECK(d.ask(bus) == "v2");

    // ...and the LEGACY road agrees too: `unload_role` is what a graceful
    // `SwapWeave` reaches through, and it selects the live holder.
    REQUIRE(kernel.unload_role(kService));
    CHECK_FALSE(kernel.is_loaded("v2"));
    CHECK(kernel.is_loaded("v1"));
}

TEST_CASE("R2B-3b-3a: Kernel::commit_candidate leaves no bookkeeping to catch up") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");

    // The Kernel's own commit door. It used to patch two cached role fields after
    // the bus answered; there is nothing to patch now, and the answers are the
    // same ones the transaction road produces.
    REQUIRE(kernel.commit_candidate("v1", "v2", kService));
    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
    CHECK(service_query(kernel).holder == d.candidate);
    CHECK(bus.role_holder(kService) == d.candidate);
    // commit_candidate is R2B-3a's narrower door: it unseals without retiring, so
    // v1 is left an ordinary weave holding no role.
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Live);

    // A refused commit changes nothing, on either side.
    CHECK_FALSE(kernel.commit_candidate("v2", "v1", kService));
    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
}

TEST_CASE("R2B-3b-3a: reloading a weave a transaction bound as its candidate releases the "
          "artifact, and the reload says so") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const WeaveId doomed = d.candidate;
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 8);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "defer");

    // THE REENTRANT CASE. New code is a new participant, so this reload aborts
    // the transaction from inside `swap_state` — and the abort discards the
    // candidate, which destroys the very adapter and record this call is holding.
    // The reload must survive its own subject vanishing mid-flight.
    LifetimeDelta ledger;
    const ReloadResult r = kernel.reload_from("v2", ZEN_SO_VERSIONED_V2);
    CHECK(r.ok);
    CHECK_FALSE(r.reloaded); // the swap happened and was then undone by the discard
    CHECK_FALSE(r.version_mismatch);
    CHECK(r.error.find("prepared replacement") != std::string::npos);

    CHECK(bus.transaction_state(t.id) == TxnState::Aborted);
    CHECK(bus.weave(doomed) == nullptr);
    artifact_released(kernel, "v2", ZEN_SO_VERSIONED_V2);
    v1_still_the_service(bus, d);

    // TWO instances and TWO libraries were in play — the incumbent code and the
    // replacement — and each went exactly once. One library was opened here; two
    // closed, because the artifact's original was released on the way out too.
    CHECK(ledger.opened() == 1);
    CHECK(ledger.created() == 1);
    CHECK(ledger.destroyed() == 2);
    CHECK(ledger.closed() == 2);
}

TEST_CASE("R2B-3b-3a: an adapter a host keeps holds its library open, and the Kernel says so") {
    Switchboard bus;
    LifetimeDelta ledger;
    {
        Kernel kernel(bus);
        REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
        const WeaveId id = kernel.weave_id("t");
        CHECK(kernel.status("t") == ArtifactStatus::Live);

        // A host takes ownership of the adapter and does not destroy it. The
        // artifact is real and its code can still run, so the library MUST stay
        // open — and the Kernel must not call it a live weave.
        std::unique_ptr<Weave> held = bus.unregister_weave(id);
        REQUIRE(held != nullptr);
        CHECK(kernel.is_loaded("t"));
        CHECK(kernel.status("t") == ArtifactStatus::Unregistered);
        CHECK(ledger.destroyed() == 0);
        CHECK(ledger.closed() == 0);

        // Dropping it destroys the instance and, only then, closes the library —
        // and the same destructor releases the Kernel's record, so the artifact
        // name is free without the Kernel being called at all.
        held.reset();
        CHECK(ledger.destroyed() == 1);
        CHECK(ledger.closed() == 1);
        CHECK_FALSE(kernel.is_loaded("t"));
        CHECK(kernel.status("t") == ArtifactStatus::NotLoaded);
    }
    CHECK(ledger.opened() == 1);
    CHECK(ledger.closed() == 1);
    CHECK(ledger.created() == ledger.destroyed());
}

TEST_CASE("R2B-3b-3a: unloading an artifact whose adapter a host still holds does not close its "
          "library early") {
    Switchboard bus;
    LifetimeDelta ledger;
    std::unique_ptr<Weave> held;
    {
        Kernel kernel(bus);
        REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
        held = bus.unregister_weave(kernel.weave_id("t"));
        REQUIRE(held != nullptr);
        // The Kernel gives up the name — and deliberately does NOT close, because
        // an adapter that can still run code from that library exists.
        CHECK(kernel.unload("t"));
        CHECK_FALSE(kernel.is_loaded("t"));
        CHECK(ledger.closed() == 0);
        CHECK(ledger.destroyed() == 0);
    }
    CHECK(ledger.closed() == 0); // ...not even when the Kernel itself is gone
    held.reset();
    CHECK(ledger.destroyed() == 1);
    CHECK(ledger.closed() == 1); // the last holder closes it, once
}

TEST_CASE("R2B-3b-3a: every artifact status is reachable, and aliveness outranks the seal") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);

    // The four states an artifact on the bus can be in, each reached by the
    // ordinary road to it. `Dead` is here because an enum value with no case
    // asserting it is a branch nobody has proven reachable.
    CHECK(kernel.status("nothing-by-that-name") == ArtifactStatus::NotLoaded);
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);

    bus.kill(d.incumbent);
    CHECK(kernel.status("v1") == ArtifactStatus::Dead);
    CHECK(kernel.is_loaded("v1")); // dead is still loaded — it is awaiting revival

    // ...AND ALIVENESS OUTRANKS THE SEAL, which is the documented precedence: a
    // weave that receives nothing is Dead whatever its seal says. Killing the
    // incumbent already aborted nothing here (no transaction), so the candidate
    // is still sealed and can be killed while sealed.
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);
    bus.kill(d.candidate);
    CHECK(bus.sealed(d.candidate)); // still sealed...
    CHECK(kernel.status("v2") == ArtifactStatus::Dead); // ...and reported dead

    // Revival restores the finer answer rather than leaving it coarse.
    REQUIRE(bus.swap_state(d.candidate, bus.snapshot_bytes(d.candidate)).revived);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);

    // The spellings exist for operators, and each is its own word.
    CHECK(std::string(name_of(ArtifactStatus::NotLoaded)) == "NotLoaded");
    CHECK(std::string(name_of(ArtifactStatus::Live)) == "Live");
    CHECK(std::string(name_of(ArtifactStatus::Sealed)) == "Sealed");
    CHECK(std::string(name_of(ArtifactStatus::Dead)) == "Dead");
    CHECK(std::string(name_of(ArtifactStatus::Unregistered)) == "Unregistered");
}

TEST_CASE("R2B-3b-3a: a namesake load is not reaped by its predecessor's adapter") {
    Switchboard bus;
    LifetimeDelta ledger;
    std::unique_ptr<Weave> held;
    {
        Kernel kernel(bus);
        REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
        held = bus.unregister_weave(kernel.weave_id("t")); // a host takes the adapter
        REQUIRE(held != nullptr);
        REQUIRE(kernel.unload("t")); // ...and the Kernel gives up the name
        REQUIRE_FALSE(kernel.is_loaded("t"));

        // THE SAME NAME, A DIFFERENT ARTIFACT — while the predecessor's adapter is
        // still alive and still remembers the string "t". This is the input every
        // other case in this file fails to produce, which is why the namesake
        // identity check survived a solo cut: it was unwatched, not redundant.
        REQUIRE(kernel.load("t", ZEN_SO_WEAVE_B).ok);
        const WeaveId fresh = kernel.weave_id("t");
        CHECK(kernel.status("t") == ArtifactStatus::Live);

        // Now the predecessor dies. It must reap NOTHING. Two independent walls
        // stand between it and the namesake's record — it was detached when its own
        // record was dropped, and it is not this record's adapter — and the paired
        // mutation that cuts BOTH is what reddens here.
        held.reset();
        CHECK(kernel.is_loaded("t"));
        CHECK(kernel.weave_id("t") == fresh);
        CHECK(kernel.status("t") == ArtifactStatus::Live);
        CHECK(bus.alive(fresh));
        CHECK(kernel.unload("t"));
    }
    CHECK(ledger.created() == ledger.destroyed());
    CHECK(ledger.opened() == ledger.closed());
    CHECK(ledger.created() == 2);
}

TEST_CASE("R2B-3b-3a: a candidate that loads but cannot be sealed leaves no artifact behind") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");

    // The artifact is fine; the SEAL is refused, because a dead coordinator
    // cannot own a preparation. `load_candidate` is the ordinary load then the
    // seal, so this is the one path where a record exists and is then undone.
    bus.kill(d.coordinator.id);
    LifetimeDelta ledger;
    const LoadResult lr = kernel.load_candidate("v3", ZEN_SO_VERSIONED_V2, d.coordinator.id);
    CHECK_FALSE(lr.ok);
    CHECK(lr.error == "candidate could not be sealed");
    artifact_released(kernel, "v3", ZEN_SO_VERSIONED_V2);
    CHECK(ledger.created() == 1);
    CHECK(ledger.destroyed() == 1);
    CHECK(ledger.opened() == 1);
    CHECK(ledger.closed() == 1);
    v1_still_the_service(bus, d);
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
