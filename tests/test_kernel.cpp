// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "weavelib/office_protocol.hpp"
#include "weavelib/prepared_replacement_protocol.hpp"

#include <zen/gate.hpp>
#include <zen/host/lifecycle_wiring.hpp>
#include <zen/host/prepared_replacement.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/weave/shape.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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
    // SCHEDULED, NOT DONE (R2B-3d). Until the admission is dispatched the
    // incumbent is still exactly the service it was — which is the whole point:
    // there is no interval in which the world has changed and the successor has
    // not been told.
    CHECK_FALSE(bus.sealed(incumbent));
    CHECK(bus.sealed(candidate));
    CHECK(bus.role_holder("worker") == incumbent);
    bus.pump();
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
    CHECK_FALSE(r.scheduled);
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
    REQUIRE(r.scheduled);
    CHECK(r.why == AdmitRefusal::None);
    CHECK(r.ticket.valid()); // the admission is a delivery, and it has a receipt

    // Scheduled means scheduled: nothing has moved yet (R2B-3d).
    CHECK(bus.role_holder("worker") == p.incumbent);
    CHECK(bus.sealed(p.candidate));
    CHECK_FALSE(bus.sealed(p.incumbent));

    bus.send_to_role("worker", Message(ping(1)));
    bus.pump();
    CHECK(bus.role_holder("worker") == p.candidate);
    CHECK_FALSE(bus.sealed(p.candidate));
    CHECK(bus.sealed(p.incumbent));
    REQUIRE(p.candidate_raw->handled_names.size() == 2);
    CHECK(p.candidate_raw->handled_names[0] == std::string(loom::Activated::zen_name));
    CHECK(p.candidate_raw->handled_names[1] == "Ping");

    // THE ADMISSION'S OWN RECEIPT SAYS DELIVERED, never refused. That is the
    // fact the whole phase turns on: a successful admission's activation cannot
    // have been rejected, because the two are one delivery.
    const DeliveryOutcome o = bus.outcome(r.ticket);
    CHECK(o.disposition == Disposition::Delivered);
    CHECK(o.refusal.reason == RefusalReason::None);
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
    /// EVERY SHAPE THE COORDINATOR WAS HANDED, and whether Loom called it an
    /// answer (R2B-3d-1). The coordinator stays credulous — it judges nothing —
    /// so a forged answer that got through would be visible here as a
    /// `VersionReply` the coordinator never asked for.
    std::vector<std::string> heard;
    std::size_t answers_ask_deliveries = 0;
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

    // COMMITTING SCHEDULES; IT DOES NOT COMMIT (R2B-3d). The transaction is
    // AdmissionPending, holds its slot, has produced NO terminal outcome, and the
    // incumbent is still the service answering "v1".
    CHECK(bus.transaction_state(begun.id) == TxnState::AdmissionPending);
    CHECK(bus.transaction_active(begun.id));
    CHECK(bus.active_transactions() == 1);
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(bus.sealed(c.candidate));
    CHECK(bus.role_holder(kRole) == c.incumbent);
    TxnOutcome early{};
    CHECK_FALSE(bus.take_outcome(c.op.id, early)); // nothing to collect: nothing ended
    // ...and it cannot be committed a second time while the first is in flight.
    CHECK_FALSE(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                                Message(to_value(fact)), 1)
                    .ok);

    bus.pump();

    // One terminal result, for the exact operator, consumed once.
    TxnOutcome out{};
    REQUIRE(bus.take_outcome(c.op.id, out));
    CHECK(out.state == TxnState::Committed);
    CHECK(out.reason == TxnReason::None);
    CHECK_FALSE(bus.take_outcome(c.op.id, out)); // consumed
    CHECK(bus.active_transactions() == 0);       // the slot came back

    // The topology moved exactly as R2B-3b-1 proved it does — in the same dispatch
    // that told the candidate it was alive.
    CHECK(bus.sealed(c.incumbent));
    CHECK_FALSE(bus.sealed(c.candidate));
    CHECK(bus.role_holder(kRole) == c.candidate);
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
    CHECK_FALSE(admitted.scheduled);
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
    // Still exclusively promised while its admission is in flight: the slot is
    // held, so nobody else can name this candidate even now (R2B-3d).
    CHECK(bus.active_transactions() == 1);
    CHECK_FALSE(bus.begin_prepared_replacement(b.op.id, b.coordinator.id, b.incumbent,
                                               a.candidate, "role-b", 8)
                    .ok);
    bus.pump();
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
    bus.pump(); // the admission is a dispatch now (R2B-3d); the drift is real after it
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
                              .field("act_answer", Kind::Int)
                              .field("act_defer", Kind::Int)
                              .field("act_send", Kind::Int)
                              .field("act_late_spend", Kind::Int)
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
///
/// `coordinator_grant` is a parameter because R2B-3d's keystone case is a
/// coordinator that CANNOT emit `zen.Activated` — the exact grant shape that used
/// to commit successfully and leave its candidate untold.
///
/// `with_candidate=false` loads only the incumbent (R2B-4a): the facade vertical
/// must load its own candidate through the handle, or it would be proving the
/// rig's plumbing instead of the handle's.
DynCast load_pair(Switchboard& bus, Kernel& kernel,
                  Grant coordinator_grant = Grant{}.allow_any(),
                  bool with_candidate = true) {
    DynCast d{register_probe(bus, {pong_schema()}),
              register_probe(bus,
                             {schema_of<versioned::CandidateReady>(),
                              schema_of<versioned::CandidateRefused>(),
                              schema_of<versioned::VersionReply>(),
                              // R2B-3d-1: the coordinator hears the candidate's
                              // ordinary speech from inside its activation — and
                              // would equally have heard a forged answer, which
                              // is why it accepts `VersionReply` above.
                              schema_of<versioned::ActivationObserved>()},
                             2, true, std::move(coordinator_grant)),
              register_probe(bus, {schema_of<versioned::VersionReply>()})};
    std::shared_ptr<CastLog> log = d.log;
    d.observer.weave->on_handle = [log](const Message& in, Bus&, ProbeWeave&) {
        const Cell* v = in.payload.get("version");
        log->answers.push_back(v == nullptr ? std::string("<none>") : std::string(v->as_text()));
    };
    std::shared_ptr<TxnId> live = d.live_txn;
    d.coordinator.weave->on_handle = [&bus, log, live](const Message& in, Bus&, ProbeWeave&) {
        // RECORDED BEFORE ANYTHING IS JUDGED (R2B-3d-1), and outside the
        // `live` guard, so the record is of what ARRIVED rather than of what the
        // coordinator felt like reacting to.
        log->heard.push_back(std::string(in.payload.schema().name()));
        if (in.provenance.answers_ask()) {
            ++log->answers_ask_deliveries;
        }
        if (!live->valid()) {
            return;
        }
        // ORDINARY SPEECH IS NOT A READINESS OFFER, and skipping it here is not
        // consumer caution: `ActivationObserved` carries no transaction and is
        // the candidate's own initiative. A forged ANSWER would still be offered
        // to the bus below, which is where credulity has to live.
        if (in.payload.schema().name() ==
            std::string_view(versioned::ActivationObserved::zen_name)) {
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
    if (with_candidate) {
        LoadResult v2 = kernel.load_candidate("v2", ZEN_SO_VERSIONED_V2, d.coordinator.id);
        REQUIRE(v2.ok);
        d.candidate = v2.id;
    }
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
    // Scheduled (R2B-3d): v1 is still the service and still answers as one until
    // the admission is dispatched. The commit call promised nothing else.
    CHECK(bus.transaction_state(t.id) == TxnState::AdmissionPending);
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK_FALSE(bus.sealed(d.incumbent));
    CHECK(bus.sealed(d.candidate));
    bus.pump();
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(bus.sealed(d.incumbent));     // sealed for retirement, not merely renamed
    CHECK_FALSE(bus.sealed(d.candidate));
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

    // 7a. WHILE THE ADMISSION IS PENDING THE BOOKS SAY WHAT IS TRUE (R2B-3d), and
    //     they say it with the same immediacy: a queued admission is not a
    //     topology change, so nothing here may report the candidate as production
    //     yet. This is the half the Kernel could most easily have got wrong — it
    //     is exactly where a cache would have been "helpfully" updated early.
    CHECK(bus.transaction_state(t.id) == TxnState::AdmissionPending);
    CHECK(service_query(kernel).holder == d.incumbent);
    CHECK(kernel.role_of("v1") == kService);
    CHECK(kernel.role_of("v2").empty());
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);

    bus.pump();

    // 8-12. IMMEDIATELY — no later host call, no reconciliation pass.
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
    const AdmitResult r = bus.admit_candidate(d.candidate, d.incumbent, kService,
                                              host_lifecycle_authority(bus),
                                              Message(to_value(loom::Activated{7})), 7);
    REQUIRE(r.scheduled);

    // Direct admission is SCHEDULED too (R2B-3d) — one primitive, one behaviour,
    // so the direct road cannot keep the split-brain the transaction road lost.
    // The Kernel says the truthful thing in both windows, immediately in both.
    CHECK(kernel.role_of("v1") == kService);
    CHECK(kernel.role_of("v2").empty());
    CHECK(kernel.status("v1") == ArtifactStatus::Live);
    CHECK(kernel.status("v2") == ArtifactStatus::Sealed);

    bus.pump();

    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
    const Kernel::RoleQuery q = service_query(kernel);
    CHECK(q.holder == d.candidate);
    CHECK(q.accepts);
    CHECK(kernel.status("v1") == ArtifactStatus::Sealed);
    CHECK(kernel.status("v2") == ArtifactStatus::Live);
    // ...and the direct caller learns the real outcome from its own receipt.
    CHECK(bus.outcome(r.ticket).disposition == Disposition::Delivered);
    CHECK(state_field(bus, d.candidate, "activations") == 1);
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

// ---- R2B-3d: admission includes first breath --------------------------------
//
// R2B-3b put the activation ahead of production. It could not make the activation
// CERTAIN: `admit_candidate` moved the role and queued the activation as an
// ordinary gated send stamped as the coordinator, so the topology changed here
// and the message was authorized later — against a grant, a sender life and a
// seal the commit had already stopped being able to guarantee.
//
//     commit -> ok ; role moves ; then Activated -> CapabilityDenied
//
// A successor that is publicly the service and was never told it is alive. So the
// two halves stopped being two things: one envelope now IS the admission and IS
// the activation, dispatched as one queue turn, and there is no representable
// state in which one of them happened.
//
//     Entering the world and being told that you entered it are one event.

namespace {

/// Every refusal the bus emitted, keyed by the seq that owns it — so a proof can
/// say "THIS EXACT DELIVERY was never refused" instead of "no refusal that looked
/// like it went past". The distinction matters here: a retiring incumbent
/// legitimately produces refusals of its own, and an assertion that merely
/// counted them would pass while the activation was being rejected beside them.
struct RefusalLog {
    struct Entry {
        std::uint64_t seq = 0;
        WeaveId target{};
        std::string schema;
        RefusalReason reason = RefusalReason::None;
    };
    std::vector<Entry> entries;
    std::vector<std::string> delivered_to_candidate;

    /// WHERE WE ARE NOW. Everything here is scoped from a mark, because a
    /// preparation legitimately delivers to the candidate and a test may
    /// legitimately provoke refusals of its own — an assertion that swept the
    /// whole log would pass on the wrong evidence.
    std::size_t refusal_mark() const { return entries.size(); }
    std::size_t delivery_mark() const { return delivered_to_candidate.size(); }

    std::vector<std::string> delivered_since(std::size_t from) const {
        return std::vector<std::string>(delivered_to_candidate.begin() +
                                            static_cast<std::ptrdiff_t>(from),
                                        delivered_to_candidate.end());
    }
    bool refused(std::uint64_t seq) const {
        for (const Entry& e : entries) {
            if (e.seq == seq) {
                return true;
            }
        }
        return false;
    }
};

/// Watch the bus for a candidate: what it was handed, in order, and every refusal
/// anybody suffered.
std::shared_ptr<RefusalLog> watch(Switchboard& bus, WeaveId candidate) {
    auto log = std::make_shared<RefusalLog>();
    bus.add_observer([log, candidate](const BusEvent& e) {
        if (e.kind == EventKind::Refused) {
            log->entries.push_back(
                RefusalLog::Entry{e.seq, e.target, e.schema_name, e.refusal.reason});
        } else if (e.kind == EventKind::Delivered && e.target == candidate) {
            log->delivered_to_candidate.push_back(e.schema_name);
        }
    });
    return log;
}

/// THE EXCLUSION LIST, written once. After a successful admission the activation
/// must not be rejectable for any of these — they are exactly the questions the
/// old gated path asked after the topology had already moved.
///
/// Scoped from the mark taken when the admission was scheduled, so a refusal the
/// test itself provoked earlier cannot be mistaken for the activation's.
void no_activation_refusal(const RefusalLog& log, WeaveId candidate, std::size_t from,
                           std::size_t from_deliveries) {
    // A POSITIVE CONTROL FIRST: exactly one activation was actually DELIVERED. An
    // exclusion list on its own is satisfied by an activation that never happened.
    std::size_t delivered = 0;
    for (const std::string& s : log.delivered_since(from_deliveries)) {
        if (s == std::string(loom::Activated::zen_name)) {
            ++delivered;
        }
    }
    CHECK(delivered == 1);
    for (std::size_t i = from; i < log.entries.size(); ++i) {
        const RefusalLog::Entry& e = log.entries[i];
        if (e.target != candidate || e.schema != loom::Activated::zen_name) {
            continue;
        }
        // Named individually so a failure says WHICH question came back to life.
        CHECK(e.reason != RefusalReason::CapabilityDenied);
        CHECK(e.reason != RefusalReason::SenderLifeEnded);
        CHECK(e.reason != RefusalReason::NoSuchTarget);
        CHECK(e.reason != RefusalReason::TargetUnavailable);
        CHECK(e.reason != RefusalReason::NotAccepted);
        CHECK(e.reason != RefusalReason::GateRefused);
        CHECK(e.reason != RefusalReason::AnswerTargetChanged);
        CHECK(e.reason != RefusalReason::SealedSpeech);
        CHECK(e.reason != RefusalReason::AdmissionRevoked);
    }
}

/// A coordinator grant that carries the preparation vocabulary and NOT
/// `zen.Activated` — the original defect's exact shape, as a value.
Grant preparation_only() {
    Grant g;
    g.allow_to_any(versioned::PrepareReplacement::zen_name,
                   versioned::PrepareReplacement::zen_version);
    g.allow_to_any(versioned::ContinuePreparation::zen_name,
                   versioned::ContinuePreparation::zen_version);
    g.allow_to_any(versioned::RetireNow::zen_name, versioned::RetireNow::zen_version);
    return g;
}

/// Drive a dynamic pair all the way to `Ready`, immediately.
TxnId dyn_ready(Switchboard& bus, DynCast& d) {
    const TxnResult t = bus.begin_prepared_replacement(d.op.id, d.coordinator.id, d.incumbent,
                                                       d.candidate, kService, 16);
    REQUIRE(t.ok);
    dyn_ask(bus, d, t.id, "ready");
    REQUIRE(bus.transaction_state(t.id) == TxnState::Ready);
    return t.id;
}

} // namespace

TEST_CASE("R2B-3d: admission and first breath are one event — v2 is never publicly the service "
          "without having been told") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    auto log = watch(bus, d.candidate);

    // 1-5. v1 is the live service; v2 is loaded sealed, prepared authentically,
    //      and the transaction reaches Ready without any of it touching v1.
    REQUIRE(d.ask(bus) == "v1");
    const TxnId t = dyn_ready(bus, d);
    v1_still_the_service(bus, d);
    CHECK(state_field(bus, d.candidate, "activations") == 0);

    // 6. Production that WILL reach v2 once the role moves, queued while it is
    //    still v1's. Role-addressed, so resolution is a delivery-time decision.
    bus.send_as_to_role(d.observer.id, kService,
                        Message(to_value(versioned::QueryVersion{1}), d.observer.id,
                                d.observer.id, 0));
    bus.send_as_to_role(d.observer.id, kService,
                        Message(to_value(versioned::QueryVersion{2}), d.observer.id,
                                d.observer.id, 0));
    REQUIRE(bus.pending() == 2);

    // 7-8. Commit. NO SUCCESSFUL RESULT IS VISIBLE YET: the transaction is
    //      pending, no outcome exists to collect, and the world is untouched.
    const std::size_t refusals_before = log->refusal_mark();
    const std::size_t deliveries_before = log->delivery_mark();
    loom::Activated fact{11};
    const TxnResult scheduled = bus.commit_prepared_replacement(
        t, host_lifecycle_authority(bus), Message(to_value(fact)), 11);
    REQUIRE(scheduled.ok);
    CHECK(bus.transaction_state(t) == TxnState::AdmissionPending);
    TxnOutcome nothing_yet{};
    CHECK_FALSE(bus.take_outcome(d.op.id, nothing_yet));
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK(bus.sealed(d.candidate));
    CHECK_FALSE(bus.sealed(d.incumbent));
    CHECK(kernel.role_of("v1") == kService);
    CHECK(state_field(bus, d.candidate, "activations") == 0);

    // 9-12. One dispatch does the whole thing.
    bus.pump();

    // Topology changed EXACTLY ONCE, and in the committed direction.
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(bus.sealed(d.incumbent));
    CHECK_FALSE(bus.sealed(d.candidate));

    // v2 received ONE authentic activation, it was its FIRST live delivery, and
    // the queued production followed it — nothing dropped, nothing reordered.
    CHECK(state_field(bus, d.candidate, "activations") == 1);
    CHECK(state_field(bus, d.candidate, "last_activation") == 11);
    const std::vector<std::string> live = log->delivered_since(deliveries_before);
    REQUIRE(live.size() == 3);
    CHECK(live[0] == std::string(loom::Activated::zen_name));
    CHECK(live[1] == std::string(versioned::QueryVersion::zen_name));
    CHECK(live[2] == std::string(versioned::QueryVersion::zen_name));

    // 13-14. v2 answers production; v1 accepts none.
    const std::int64_t v1_served = state_field(bus, d.incumbent, "served");
    CHECK(d.ask(bus) == "v2");
    CHECK(state_field(bus, d.incumbent, "served") == v1_served);
    CHECK(state_field(bus, d.candidate, "served") == 3); // the two queued, plus that one

    // 15. Exactly one Committed terminal outcome, consumed once.
    exactly_one_outcome(bus, d.op.id, t, TxnState::Committed, TxnReason::None);
    CHECK(bus.active_transactions() == 0);

    // 16. The Kernel's role and artifact records agree with the Switchboard.
    CHECK(kernel.role_of("v2") == kService);
    CHECK(kernel.role_of("v1").empty());
    CHECK(kernel.status("v2") == ArtifactStatus::Live);
    CHECK(kernel.status("v1") == ArtifactStatus::Sealed);

    // And the whole point: the activation was delivered exactly once, and no
    // refusal of any kind names it.
    no_activation_refusal(*log, d.candidate, refusals_before, deliveries_before);
}

TEST_CASE("R2B-3d: THE ORIGINAL DEFECT — a coordinator with no zen.Activated grant admits, and "
          "the activation is authentic anyway") {
    // Before the repair this scenario was: commit -> ok, the role moves, and the
    // activation is then refused at delivery as CapabilityDenied. The chosen law
    // is that LIFECYCLE AUTHORITY owns a committed activation, so the ordinary
    // grant is not consulted — and this proves both halves of that: the admission
    // is whole, and the ordinary door is exactly as narrow as it was.
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, preparation_only());
    auto log = watch(bus, d.candidate);

    // The coordinator genuinely cannot emit the shape. Proven, not assumed —
    // against the live INCUMBENT, which accepts `zen.Activated` and is not sealed,
    // so the grant is the only thing that can be refusing.
    const Ticket forged = bus.send_as(d.coordinator.id, d.incumbent,
                                      Message(to_value(loom::Activated{99})));
    bus.pump();
    CHECK(bus.outcome(forged).refusal.reason == RefusalReason::CapabilityDenied);
    CHECK(state_field(bus, d.incumbent, "activations") == 0);

    REQUIRE(d.ask(bus) == "v1");
    const TxnId t = dyn_ready(bus, d);
    const std::size_t refusals_before = log->refusal_mark();
    const std::size_t deliveries_before = log->delivery_mark();
    loom::Activated fact{5};
    REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                            Message(to_value(fact)), 5)
                .ok);
    bus.pump();

    // Committed, authentic, and production follows.
    exactly_one_outcome(bus, d.op.id, t, TxnState::Committed, TxnReason::None);
    CHECK(bus.role_holder(kService) == d.candidate);
    CHECK(state_field(bus, d.candidate, "activations") == 1);
    CHECK(state_field(bus, d.candidate, "last_activation") == 5);
    const std::vector<std::string> live = log->delivered_since(deliveries_before);
    REQUIRE(live.size() >= 1);
    CHECK(live[0] == std::string(loom::Activated::zen_name));
    CHECK(d.ask(bus) == "v2");
    no_activation_refusal(*log, d.candidate, refusals_before, deliveries_before);

    // ...AND NO PUBLIC BYPASS APPEARED. The same coordinator, now speaking to the
    // weave it just admitted — public, unsealed, accepting the shape — still
    // cannot say `zen.Activated`. The ordinary grant governs ordinary speech
    // exactly as before; what changed is only that a committed activation was
    // never ordinary speech.
    const Ticket after = bus.send_as(d.coordinator.id, d.candidate,
                                     Message(to_value(loom::Activated{6})));
    bus.pump();
    CHECK(bus.outcome(after).refusal.reason == RefusalReason::CapabilityDenied);
    CHECK(state_field(bus, d.candidate, "activations") == 1); // still one
}

TEST_CASE("R2B-3d: an ordinary weave holding the grant sends a perfect zen.Activated that is not "
          "a lifecycle fact") {
    // The other half of the same law. Removing the grant from the committed path
    // must not make the shape mean anything on its own: a weave that IS permitted
    // to emit it produces a delivered, well-formed, correctly-stamped message
    // carrying no attestation, and the consumer ignores it.
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    Registered impostor = register_probe(bus, {pong_schema()});

    // First against the live incumbent, which is public and accepts the shape.
    const Ticket at_v1 =
        bus.send_as(impostor.id, d.incumbent, Message(to_value(loom::Activated{1})));
    bus.pump();
    CHECK(bus.outcome(at_v1).disposition == Disposition::Delivered); // it arrived...
    CHECK(state_field(bus, d.incumbent, "activations") == 0);        // ...and meant nothing

    // Then the real thing, and then a REPLAY of it: the same shape, the same
    // sequence, sent ordinarily by a weave that may emit it. A copied activation
    // is a costume — the attestation is a delivery fact and there is nothing on
    // the wire to copy.
    const TxnId t = dyn_ready(bus, d);
    REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                            Message(to_value(loom::Activated{4})), 4)
                .ok);
    bus.pump();
    CHECK(state_field(bus, d.candidate, "activations") == 1);
    CHECK(state_field(bus, d.candidate, "last_activation") == 4);

    const Ticket replay =
        bus.send_as(impostor.id, d.candidate, Message(to_value(loom::Activated{5})));
    bus.pump();
    CHECK(bus.outcome(replay).disposition == Disposition::Delivered);
    CHECK(state_field(bus, d.candidate, "activations") == 1);      // unmoved
    CHECK(state_field(bus, d.candidate, "last_activation") == 4);  // and unadvanced
}

TEST_CASE("R2B-3d: a candidate that cannot receive its own activation is not admissible, and the "
          "refusal happens before anything moves") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");

    // Two ways to be unable to receive it, and both are the candidate's own
    // contract rather than anything about the coordinator or the world.
    std::vector<std::shared_ptr<const Schema>> accept{ping_schema()};
    Message activation(to_value(loom::Activated{3}));
    const AdmitRefusal expected = AdmitRefusal::CandidateContract;
    SUBCASE("it does not accept zen.Activated at all") {}
    SUBCASE("its gate refuses this exact activation") {
        accept.push_back(schema_of<loom::Activated>());
        // The same (name, version), a different shape — so the door MATCHES and
        // the gate is what refuses. An honest operator cannot produce this; the
        // pin exists because "the door accepted it" and "the gate admitted it"
        // are two questions and the old code asked neither until after commit.
        const auto impostor = SchemaBuilder(std::string(loom::Activated::zen_name),
                                            loom::Activated::zen_version)
                                  .field("sequence", Kind::Text)
                                  .build();
        Value bad(impostor);
        bad.set("sequence", Cell::text("3"));
        activation = Message(std::move(bad));
    }

    auto cand_weave = std::make_unique<ProbeWeave>(accept);
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    const AdmitResult r = bus.admit_candidate(candidate, incumbent, "worker",
                                              host_lifecycle_authority(bus),
                                              std::move(activation), 3);
    CHECK_FALSE(r.scheduled);
    CHECK(r.why == expected);
    CHECK_FALSE(r.ticket.valid()); // a refusal queues nothing

    // NOTHING MOVED and nothing was queued — the refusal is indistinguishable
    // from never having asked.
    CHECK(bus.pending() == 0);
    CHECK(bus.role_holder("worker") == incumbent);
    CHECK(bus.sealed(candidate));
    CHECK_FALSE(bus.sealed(incumbent));
    bus.pump();
    CHECK(cand_raw->handled_names.empty());
}

TEST_CASE("R2B-3d: a transaction whose candidate cannot be activated ends Aborted, and the "
          "incumbent never learns of it") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult begun = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                           c.incumbent, c.candidate, kRole, 8);
    REQUIRE(begun.ok);
    const TxnId t = begun.id;
    make_ready(bus, c, t);
    REQUIRE(bus.transaction_state(t) == TxnState::Ready);

    // A well-formed activation the candidate's gate cannot admit.
    const auto impostor =
        SchemaBuilder(std::string(loom::Activated::zen_name), loom::Activated::zen_version)
            .field("sequence", Kind::Text)
            .build();
    Value bad(impostor);
    bad.set("sequence", Cell::text("1"));
    const TxnResult refused = bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                                              Message(std::move(bad)), 1);
    CHECK_FALSE(refused.ok);
    CHECK(refused.why == TxnReason::AdmissionRefused);
    exactly_one_outcome(bus, c.op.id, t, TxnState::Aborted, TxnReason::AdmissionRefused);
    CHECK(bus.pending() == 0);
    CHECK(bus.role_holder(kRole) == c.incumbent);
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(c.ask(bus, kRole) == "v1");
}

TEST_CASE("R2B-3d: a scheduled admission whose world drifts before dispatch refuses, and the "
          "incumbent is still the service") {
    // THE INTERVAL THE PHASE CREATED, and the proof that it is safe. Scheduling an
    // admission promises nothing; between the schedule and the dispatch anything
    // may happen, and every one of these roads ends in the same place — the
    // envelope refuses as `AdmissionRevoked`, no topology moved, and no weave was
    // told anything.
    //
    // These are DIRECT admissions on purpose: with no transaction there is no
    // invalidation hook standing in front, so the envelope's own recorded
    // participants are the only wall, which is exactly the wall under test.
    enum class Drift { CoordinatorDies, CoordinatorReloads, CandidateDies, CandidateReloads,
                       IncumbentDies, IncumbentRemoved, CandidateRemoved };
    Drift drift = Drift::CoordinatorDies;
    SUBCASE("the coordinator dies") { drift = Drift::CoordinatorDies; }
    SUBCASE("the coordinator's code is replaced") { drift = Drift::CoordinatorReloads; }
    SUBCASE("the candidate dies") { drift = Drift::CandidateDies; }
    SUBCASE("the candidate's code is replaced") { drift = Drift::CandidateReloads; }
    SUBCASE("the incumbent dies") { drift = Drift::IncumbentDies; }
    SUBCASE("the incumbent is permanently removed") { drift = Drift::IncumbentRemoved; }
    SUBCASE("the candidate is removed and a fresh weave takes its place") {
        drift = Drift::CandidateRemoved;
    }

    Switchboard bus;
    Prepared p = prepare(bus);
    // `unregister_weave` HANDS THE OBJECT BACK, and dropping that owner destroys
    // it — which would leave `p.candidate_raw` dangling for the final check below.
    // Held here on purpose: ASan found this, and the Debug lane was green on it.
    std::unique_ptr<Weave> removed;

    const AdmitResult r = bus.admit_candidate(p.candidate, p.incumbent, "worker",
                                              host_lifecycle_authority(bus),
                                              Message(to_value(loom::Activated{2})), 2);
    REQUIRE(r.scheduled);
    const Topology scheduled = Topology::of(bus, p);
    CHECK(scheduled.holder == p.incumbent); // scheduling moved nothing

    switch (drift) {
    case Drift::CoordinatorDies:
        bus.kill(p.coordinator.id);
        break;
    case Drift::CoordinatorReloads:
        REQUIRE(bus.swap_state(p.coordinator.id, bus.snapshot_bytes(p.coordinator.id)).revived);
        break;
    case Drift::CandidateDies:
        bus.kill(p.candidate);
        break;
    case Drift::CandidateReloads:
        REQUIRE(bus.swap_state(p.candidate, bus.snapshot_bytes(p.candidate)).revived);
        break;
    case Drift::IncumbentDies:
        bus.kill(p.incumbent);
        break;
    case Drift::IncumbentRemoved:
        // Gone entirely, which also releases the role — so the admission arrives
        // to find no incumbent AND no slot. It replaces nothing rather than
        // installing a successor over an absence.
        removed = bus.unregister_weave(p.incumbent);
        break;
    case Drift::CandidateRemoved:
        removed = bus.unregister_weave(p.candidate);
        // A brand-new weave, taking the same place in the world. WeaveIds are
        // never reused, so it cannot BE the old id — and the envelope names a
        // life and an incarnation as well, so even a namesake could not pass.
        (void)bus.register_weave(std::make_unique<ProbeWeave>(
                                     std::vector<std::shared_ptr<const Schema>>{
                                         schema_of<loom::Activated>()}),
                                 Grant{}.allow_any());
        break;
    }

    bus.pump();

    // Refused, named, and nothing moved.
    const DeliveryOutcome o = bus.outcome(r.ticket);
    CHECK(o.disposition == Disposition::Refused);
    CHECK(o.refusal.reason == RefusalReason::AdmissionRevoked);
    if (drift == Drift::IncumbentRemoved) {
        CHECK_FALSE(bus.role_holder("worker").valid()); // released with its holder
    } else {
        CHECK(bus.role_holder("worker") == p.incumbent); // still the service's slot
        CHECK_FALSE(bus.sealed(p.incumbent)); // never sealed for a retirement that did not happen
    }
    if (drift != Drift::CandidateRemoved) {
        CHECK(bus.sealed(p.candidate)); // still outside the world
    }
    CHECK(p.candidate_raw->handled_names.empty()); // and never told anything
}

TEST_CASE("R2B-3d: two admissions racing for one role — the first wins whole, the second refuses "
          "whole") {
    // Both are scheduled while the world still permits both. The queue decides,
    // and there is no partial outcome on either side: the loser's envelope finds
    // an incumbent that is no longer a public service and changes nothing.
    Switchboard bus;
    Prepared p = prepare(bus);
    auto other = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* other_raw = other.get();
    const WeaveId rival = bus.register_weave(std::move(other), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(rival, p.coordinator.id));

    const AdmitResult first = bus.admit_candidate(p.candidate, p.incumbent, "worker",
                                                  host_lifecycle_authority(bus),
                                                  Message(to_value(loom::Activated{1})), 1);
    const AdmitResult second = bus.admit_candidate(rival, p.incumbent, "worker",
                                                   host_lifecycle_authority(bus),
                                                   Message(to_value(loom::Activated{2})), 2);
    REQUIRE(first.scheduled);
    REQUIRE(second.scheduled); // both look fine from here, and that is honest
    bus.pump();

    CHECK(bus.outcome(first.ticket).disposition == Disposition::Delivered);
    CHECK(bus.outcome(second.ticket).refusal.reason == RefusalReason::AdmissionRevoked);
    CHECK(bus.role_holder("worker") == p.candidate);
    CHECK(p.candidate_raw->handled_names.size() == 1);
    CHECK(other_raw->handled_names.empty()); // the loser was told nothing at all
    CHECK(bus.sealed(rival));                // and is still outside the world
}

TEST_CASE("R2B-3d: aborting a pending admission stops it, once — the queued action cannot revive "
          "a transaction that has ended") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    const TxnResult begun = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                           c.incumbent, c.candidate, kRole, 8);
    REQUIRE(begun.ok);
    make_ready(bus, c, begun.id);
    REQUIRE(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                            Message(to_value(loom::Activated{1})), 1)
                .ok);
    REQUIRE(bus.transaction_state(begun.id) == TxnState::AdmissionPending);

    // The operator changes its mind while the admission is in flight.
    REQUIRE(bus.abort_prepared_replacement(begun.id, c.op.id).ok);
    exactly_one_outcome(bus, c.op.id, begun.id, TxnState::Aborted, TxnReason::ExplicitAbort);
    CHECK(bus.active_transactions() == 0); // capacity reclaimed at once

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.pump();

    // THE QUEUED ADMISSION FINDS NOTHING AND DOES NOTHING. In particular it does
    // not write a second terminal outcome — the abort already wrote the only one.
    CHECK(bus.role_holder(kRole) == c.incumbent);
    CHECK_FALSE(bus.sealed(c.incumbent));
    CHECK(c.ask(bus, kRole) == "v1");
    TxnOutcome second{};
    CHECK_FALSE(bus.take_outcome(c.op.id, second));
    std::size_t revoked = 0;
    for (const TapRecord& t : tap) {
        if (t.kind == EventKind::Refused && t.reason == RefusalReason::AdmissionRevoked) {
            ++revoked;
        }
    }
    CHECK(revoked == 1);

    // ...and the transaction cannot be committed again from any door.
    CHECK_FALSE(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                                Message(to_value(loom::Activated{2})), 2)
                    .ok);
    CHECK(bus.transaction_state(begun.id) == TxnState::Aborted);
}

TEST_CASE("R2B-3d: a pending admission holds its promises — no second commit, no second candidate, "
          "no readiness, no budget") {
    Switchboard bus;
    Cast c = cast_with_role(bus, kRole);
    Cast other = cast_with_role(bus, "other");
    const TxnResult begun = bus.begin_prepared_replacement(c.op.id, c.coordinator.id,
                                                           c.incumbent, c.candidate, kRole, 8);
    REQUIRE(begun.ok);
    make_ready(bus, c, begun.id);
    REQUIRE(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                            Message(to_value(loom::Activated{1})), 1)
                .ok);

    // Every door that was closed in Ready is closed in AdmissionPending, and each
    // says so as itself rather than as a bare false.
    CHECK(bus.commit_prepared_replacement(begun.id, host_lifecycle_authority(bus),
                                          Message(to_value(loom::Activated{2})), 2)
              .why == TxnReason::WrongState);
    CHECK(bus.tick_preparation(begun.id).why == TxnReason::WrongState);
    CHECK(bus.ask_candidate_to_prepare(begun.id, Message(to_value(versioned::PrepareReplacement{})))
              .why == TxnReason::WrongState);
    CHECK(bus.accept_preparation_answer(begun.id, PreparationAnswer::Ready).why ==
          TxnReason::WrongState);
    // The candidate is still exclusively this transaction's, and the incumbent
    // still busy — the slot has not been released early. Asked through the SAME
    // coordinator, so the exclusivity check is the term that decides rather than
    // an earlier ownership one.
    CHECK(bus.begin_prepared_replacement(other.op.id, c.coordinator.id, other.incumbent,
                                         c.candidate, "other", 8)
              .why == TxnReason::CandidateBusy);
    CHECK(bus.begin_prepared_replacement(c.op.id, other.coordinator.id, c.incumbent,
                                         other.candidate, kRole, 8)
              .why == TxnReason::IncumbentBusy);
    CHECK(bus.active_transactions() == 1);

    bus.pump();
    exactly_one_outcome(bus, c.op.id, begun.id, TxnState::Committed, TxnReason::None);
    CHECK(c.candidate_raw->activations == 1); // exactly one, ever
}

TEST_CASE("R2B-3d: the admission dispatch keeps activation-first ordering, unrelated FIFO, and "
          "drops nothing") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered bystander = register_probe(bus, {ping_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    ProbeWeave* inc_raw = static_cast<ProbeWeave*>(bus.weave(incumbent));
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    // Four kinds of traffic, queued before the commit request. A publication too:
    // `fanout` chooses recipients at ENQUEUE time and skips sealed records, so a
    // publication from before the admission is not retroactively the candidate's.
    bus.send(bystander.id, Message(ping(1)));      // unrelated
    bus.send_to_role("worker", Message(ping(2)));  // will resolve to the candidate
    bus.send(candidate, Message(ping(3)));         // direct to the candidate
    bus.publish(Message(ping(4)));                 // to the world as it is now
    bus.send(bystander.id, Message(ping(5)));      // unrelated, after
    const std::size_t queued = bus.pending();

    REQUIRE(bus.admit_candidate(candidate, incumbent, "worker", host_lifecycle_authority(bus),
                                Message(to_value(loom::Activated{9})), 9));
    CHECK(bus.pending() == queued + 1); // exactly one envelope added; nothing removed
    bus.pump();

    // The candidate: activation first, then everything that was waiting for it,
    // in the order it was queued. Nothing was dropped to achieve that.
    REQUIRE(cand_raw->handled_names.size() == 3);
    CHECK(cand_raw->handled_names[0] == std::string(loom::Activated::zen_name));
    CHECK(cand_raw->handled_values[1] == 2); // the role message, queued first
    CHECK(cand_raw->handled_values[2] == 3); // then the direct one
    // The bystander's FIFO is untouched — including the one queued BEFORE the
    // admission point, which is the half a head-insertion would have broken.
    REQUIRE(bystander.weave->handled_values.size() == 3);
    CHECK(bystander.weave->handled_values[0] == 1);
    CHECK(bystander.weave->handled_values[1] == 4); // the publication reached the world...
    CHECK(bystander.weave->handled_values[2] == 5);
    // ...and the incumbent got the publication too, because it was still public
    // when the publication chose its recipients. The candidate did not.
    REQUIRE(inc_raw->handled_values.size() == 1);
    CHECK(inc_raw->handled_values[0] == 4);

    // A publication AFTER the admission reaches the new service and not the old.
    bus.publish(Message(ping(6)));
    bus.pump();
    CHECK(cand_raw->handled_values.back() == 6);
    CHECK(inc_raw->handled_values.size() == 1); // sealed for retirement, hears nothing
}

TEST_CASE("R2B-3d: a foreign lifecycle authority cannot even schedule an admission") {
    Switchboard bus;
    Switchboard decoy;
    Prepared p = prepare(bus);
    const Topology before = Topology::of(bus, p);

    const AdmitResult r = bus.admit_candidate(p.candidate, p.incumbent, "worker",
                                              host_lifecycle_authority(decoy),
                                              Message(to_value(loom::Activated{1})), 1);
    CHECK_FALSE(r.scheduled);
    CHECK(r.why == AdmitRefusal::ForeignAuthority);
    CHECK_FALSE(r.ticket.valid());
    CHECK(bus.pending() == 0); // nothing was queued to be revoked later
    bus.pump();
    CHECK(Topology::of(bus, p) == before);
}

TEST_CASE("R2B-3d: a lifecycle change during a pending admission ends the transaction, releases "
          "the candidate's artifact, and leaves the Kernel agreeing with the Switchboard") {
    // The transaction road, where the invalidation hook stands in front of the
    // queued envelope. It ends the transaction with the reason that describes what
    // actually happened — not `AdmissionRefused`, which would blame the admission
    // for a coordinator that died — and the envelope then finds nothing to do.
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");
    const TxnId t = dyn_ready(bus, d);

    TxnReason expected = TxnReason::CoordinatorChanged;
    SUBCASE("the coordinator dies") {
        expected = TxnReason::CoordinatorChanged;
        REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                                Message(to_value(loom::Activated{1})), 1)
                    .ok);
        bus.kill(d.coordinator.id);
    }
    SUBCASE("the candidate's code is replaced under it") {
        expected = TxnReason::CandidateChanged;
        REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                                Message(to_value(loom::Activated{1})), 1)
                    .ok);
        REQUIRE(bus.swap_state(d.candidate, bus.snapshot_bytes(d.candidate)).revived);
    }
    SUBCASE("the incumbent dies") {
        expected = TxnReason::IncumbentChanged;
        REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                                Message(to_value(loom::Activated{1})), 1)
                    .ok);
        bus.kill(d.incumbent);
    }

    exactly_one_outcome(bus, d.op.id, t, TxnState::Aborted, expected);
    CHECK(bus.active_transactions() == 0);

    bus.pump();

    // The role never moved, and the Kernel says so — including about the artifact
    // an aborted transaction discards: a candidate that never entered the world is
    // unregistered, and its record and library go with it (R2B-3b-3a's law,
    // unchanged by the pending window).
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK(kernel.role_of("v1") == kService);
    CHECK(kernel.status("v2") == ArtifactStatus::NotLoaded);
    CHECK_FALSE(kernel.is_loaded("v2"));
    TxnOutcome second{};
    CHECK_FALSE(bus.take_outcome(d.op.id, second)); // still exactly one

    // And the transaction cannot be revived by anything the queue held.
    CHECK(bus.transaction_state(t) == TxnState::Aborted);
    CHECK_FALSE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                                Message(to_value(loom::Activated{2})), 2)
                    .ok);
}

TEST_CASE("R2B-3d: a stale queued admission cannot land on a namesake artifact loaded in the "
          "candidate's place") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    REQUIRE(d.ask(bus) == "v1");

    const AdmitResult r = bus.admit_candidate(d.candidate, d.incumbent, kService,
                                              host_lifecycle_authority(bus),
                                              Message(to_value(loom::Activated{8})), 8);
    REQUIRE(r.scheduled);

    // The artifact is unloaded and the SAME NAME is loaded again from the same
    // file, sealed to the same coordinator. Everything an operator would recognise
    // is identical; the only thing that is not is the identity the envelope wrote
    // down when it was scheduled.
    REQUIRE(kernel.unload("v2"));
    const LoadResult again = kernel.load_candidate("v2", ZEN_SO_VERSIONED_V2, d.coordinator.id);
    REQUIRE(again.ok);
    CHECK_FALSE(again.id == d.candidate);

    bus.pump();
    CHECK(bus.outcome(r.ticket).refusal.reason == RefusalReason::AdmissionRevoked);
    v1_still_the_service(bus, d);
    CHECK(bus.sealed(again.id));
    CHECK(state_field(bus, again.id, "activations") == 0);
}

// ---- R2B-3d-1: first breath is not a question -------------------------------
//
// R2B-3d made the committed activation Loom's own act rather than the
// coordinator's speech — and then built its delivery context in the ordinary
// path's image, which fabricated a requester. The stamped sender is the OPERATOR
// that admitted the candidate, not a weave that asked it anything, so a reply
// authority naming it let a candidate answer a request nobody made.
//
//     Lifecycle activation is an authenticated fact, not an ask.
//
// The model already had the right category: `answer_as` and `defer_answer_as`
// refuse when there is no valid requester — the case they document as "the
// request came from a root, so there is no requester to answer". Loom's own act
// belongs there, and putting it there needed no new machinery, no new state and
// no ABI change.

namespace {

/// A candidate that, from inside an accepted activation, tries everything a
/// delivery-that-had-been-an-ask would grant — and then does the one thing that
/// is genuinely still its right.
struct ActivationAttempts {
    bool answered = false;      ///< did `answer()` hand back a real ticket?
    bool deferred_valid = false;///< did `defer_answer()` hand back a real capability?
    bool sent = false;          ///< did ordinary domain speech go out?
    bool late_spend = false;    ///< could what it kept ever be spent?
    loom::DeferredAnswer kept{};
};

} // namespace

TEST_CASE("R2B-3d-1: a candidate cannot answer its own activation, cannot defer an answer to it, "
          "and is not thereby made mute") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema(), ping_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");

    ActivationAttempts tried;
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));

    cand_raw->on_handle = [&tried, coordinator](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() != std::string_view(loom::Activated::zen_name)) {
            // A LATER, REAL ASK. Authority is scoped per delivery, not removed
            // from the weave — so an ordinary request answers normally.
            if (in.sender.valid()) {
                (void)b.answer(Message(pong(99)));
            }
            return;
        }
        REQUIRE(in.provenance.lifecycle_activation()); // it IS an authentic activation...
        tried.answered = b.answer(Message(pong(1))).valid();       // ...and not a question
        tried.kept = b.make_deferred_answer();
        tried.deferred_valid = tried.kept.valid();
        tried.sent = b.send(coordinator.id, Message(pong(2))).valid();
    };

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    REQUIRE(bus.admit_candidate(candidate, incumbent, "worker", host_lifecycle_authority(bus),
                                Message(to_value(loom::Activated{5})), 5));
    bus.pump();

    // THE ACTIVATION ITSELF IS UNTOUCHED: authentic, delivered, exactly once.
    REQUIRE(cand_raw->handled_names.size() == 1);
    CHECK(cand_raw->handled_names[0] == std::string(loom::Activated::zen_name));
    CHECK(bus.role_holder("worker") == candidate);

    // NOBODY ASKED, so there was nothing to answer and nothing to defer.
    CHECK_FALSE(tried.answered);
    CHECK_FALSE(tried.deferred_valid);
    CHECK(tried.kept.opaque_token() == 0); // an invalid capability, not a real one

    // ...AND IT IS NOT MUTE. Ordinary domain speech, to the very weave whose
    // imaginary question it was just refused, went out and arrived.
    CHECK(tried.sent);
    REQUIRE(coordinator.weave->handled_names.size() == 1);
    CHECK(coordinator.weave->handled_values[0] == 2);

    // The refusal is VISIBLE and says AUTHORITY, and no answer envelope exists:
    // the only refusal on the tap is the answer attempt, and the coordinator
    // received exactly one message — the ordinary one.
    std::size_t denied = 0;
    for (const TapRecord& t : tap) {
        if (t.kind == EventKind::Refused && t.reason == RefusalReason::CapabilityDenied) {
            ++denied;
        }
        CHECK(t.reason != RefusalReason::Exhausted); // no bounded capacity was touched
    }
    CHECK(denied == 1);

    // A LATER REAL ASK IS ANSWERABLE NORMALLY — the authority was scoped to a
    // delivery, not taken away from the candidate.
    bus.send_as(coordinator.id, candidate, Message(ping(7), coordinator.id, coordinator.id, 42));
    bus.pump();
    REQUIRE(coordinator.weave->handled_names.size() == 2);
    CHECK(coordinator.weave->handled_values[1] == 99); // the answer to the real question
}

TEST_CASE("R2B-3d-1: an activation's refused deferral consumes none of the bounded capacity — "
          "proven with the registry held one slot from full") {
    // A BOUND IS ONLY AN INSTRUMENT IF THE TEST HOLDS IT SATURATED (R2B-3b-1a's
    // lesson, applied again). Answering past the bound proves nothing if each
    // slot is returned before the next is taken.
    Switchboard bus;
    Registered asker = register_probe(bus, {pong_schema()});
    Registered coordinator = register_probe(bus, {pong_schema(), ping_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");

    // A responder that keeps every answer right it is given, so the registry
    // fills and STAYS full.
    std::vector<loom::DeferredAnswer> held;
    Registered responder = register_probe(bus, {ping_schema()});
    responder.weave->on_handle = [&held](const Message&, Bus& b, ProbeWeave&) {
        held.push_back(b.make_deferred_answer());
    };

    // Fill it to exactly one slot short of the bound.
    const std::size_t bound = Switchboard::kMaxDeferredAnswers;
    for (std::size_t i = 0; i < bound - 1; ++i) {
        bus.send_as(asker.id, responder.id, Message(ping(static_cast<std::int64_t>(i))));
    }
    bus.pump();
    REQUIRE(held.size() == bound - 1);
    for (const loom::DeferredAnswer& d : held) {
        REQUIRE(d.valid()); // every one of them is real
    }

    ActivationAttempts tried;
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));
    cand_raw->on_handle = [&tried](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() != std::string_view(loom::Activated::zen_name)) {
            return;
        }
        tried.kept = b.make_deferred_answer();
        tried.deferred_valid = tried.kept.valid();
    };

    REQUIRE(bus.admit_candidate(candidate, incumbent, "worker", host_lifecycle_authority(bus),
                                Message(to_value(loom::Activated{2})), 2));
    bus.pump();
    CHECK_FALSE(tried.deferred_valid);

    // THE LAST SLOT IS STILL THERE. If the activation had taken it, this
    // legitimate deferral — a real ask, from a real requester — would fail.
    bus.send_as(asker.id, responder.id, Message(ping(1000)));
    bus.pump();
    REQUIRE(held.size() == bound);
    CHECK(held.back().valid());

    // ...and the bound is real, which is what makes the check above mean
    // something: the next one has nowhere to go.
    bus.send_as(asker.id, responder.id, Message(ping(1001)));
    bus.pump();
    REQUIRE(held.size() == bound + 1);
    CHECK_FALSE(held.back().valid());
}

TEST_CASE("R2B-3d-1: nothing delivered because of an activation can later be made to look like "
          "an answer") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema(), ping_schema()});
    const WeaveId incumbent = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");

    ActivationAttempts tried;
    auto cand_weave = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<loom::Activated>(), ping_schema()});
    ProbeWeave* cand_raw = cand_weave.get();
    const WeaveId candidate = bus.register_weave(std::move(cand_weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(candidate, coordinator.id));
    cand_raw->on_handle = [&tried, coordinator](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() != std::string_view(loom::Activated::zen_name)) {
            return;
        }
        tried.kept = b.make_deferred_answer();
        // COPY EVERYTHING THE ACTIVATION CARRIED and send it back by hand: the
        // coordinator's own id as the target, and the activation's correlation.
        // An honest weave cannot forge provenance, so the sharpest attack it CAN
        // express is to reproduce every visible fact and see whether the shape
        // of the thing is enough. It is not.
        (void)b.send(in.sender, Message(pong(1), in.sender, in.sender, in.correlation));
        tried.sent = true;
    };

    // Whether Loom ever calls a delivery to the coordinator an ANSWER.
    std::size_t answers_seen = 0;
    coordinator.weave->on_handle = [&answers_seen](const Message& in, Bus&, ProbeWeave&) {
        if (in.provenance.answers_ask()) {
            ++answers_seen;
        }
    };

    REQUIRE(bus.admit_candidate(candidate, incumbent, "worker", host_lifecycle_authority(bus),
                                Message(to_value(loom::Activated{3})), 3));
    bus.pump();

    // The copy ARRIVED — it is ordinary speech and the candidate is entitled to
    // send it — and Loom called it exactly what it is.
    CHECK(tried.sent);
    REQUIRE(coordinator.weave->handled_names.size() == 1);
    CHECK(answers_seen == 0);

    // Spending the kept capability LATER cannot work either: invalid is invalid
    // forever, not merely at the moment it was refused.
    CHECK_FALSE(bus.weave(candidate) == nullptr);
    bus.send_as(coordinator.id, candidate, Message(ping(4)));
    bus.pump();
    CHECK(answers_seen == 0);
    CHECK(coordinator.weave->handled_names.size() == 1); // still just the copy
}

TEST_CASE("R2B-3d-1: an ORDINARY zen.Activated-shaped message is still answerable — the "
          "distinction is provenance, not payload type") {
    // The correction must not spread by SHAPE. A weave that legitimately holds
    // the grant may send `zen.Activated` as ordinary speech; that message is not
    // a lifecycle fact (it carries no attestation, and the consumer ignores it
    // as one) but it IS an ordinary delivery from a real sender — so the
    // recipient may answer it exactly as it may answer anything else.
    Switchboard bus;
    Registered sender = register_probe(bus, {pong_schema()});
    Registered target = register_probe(bus, {schema_of<loom::Activated>()});

    bool answered = false;
    bool attested = false;
    target.weave->on_handle = [&answered, &attested](const Message& in, Bus& b, ProbeWeave&) {
        attested = in.provenance.lifecycle_activation();
        answered = b.answer(Message(pong(1))).valid();
    };

    bus.send_as(sender.id, target.id, Message(to_value(loom::Activated{1}), sender.id,
                                              sender.id, 7));
    bus.pump();

    CHECK_FALSE(attested);  // not a lifecycle fact...
    CHECK(answered);        // ...and still an ordinary question, answerable
    REQUIRE(sender.weave->handled_names.size() == 1);
    CHECK(sender.weave->handled_names[0] == "Pong");
}

TEST_CASE("R2B-3d-1: the dynamic candidate tries all three across the library seam") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel);
    auto log = watch(bus, d.candidate);
    REQUIRE(d.ask(bus) == "v1");

    const TxnId t = dyn_ready(bus, d);
    const std::size_t heard_before = d.log->heard.size();
    const std::size_t deliveries_before = log->delivery_mark();
    const std::size_t refusals_before = log->refusal_mark();

    REQUIRE(bus.commit_prepared_replacement(t, host_lifecycle_authority(bus),
                                            Message(to_value(loom::Activated{6})), 6)
                .ok);
    bus.pump();

    // The activation is authentic, first, and exactly once.
    CHECK(state_field(bus, d.candidate, "activations") == 1);
    CHECK(state_field(bus, d.candidate, "last_activation") == 6);
    const std::vector<std::string> live = log->delivered_since(deliveries_before);
    REQUIRE(live.size() >= 1);
    CHECK(live[0] == std::string(loom::Activated::zen_name));
    no_activation_refusal(*log, d.candidate, refusals_before, deliveries_before);

    // ANSWER: refused, and that verdict is REAL across the seam — R2B-3b-1a gave
    // the dynamic `answer`/`defer_answer` doors genuine success/failure, so a
    // zero here is the host refusing rather than the ABI shrugging. (An ordinary
    // `send` is the one that always returns an invalid ticket, which is why
    // `act_send` records only that the handler got that far; its ARRIVAL is
    // judged below, from what the coordinator was actually handed.)
    CHECK(state_field(bus, d.candidate, "act_answer") == 0);
    CHECK(state_field(bus, d.candidate, "act_defer") == 0);
    CHECK(state_field(bus, d.candidate, "act_send") == 1);

    // The credulous coordinator heard exactly one thing from the new service —
    // its ordinary speech — and Loom never called anything it received an answer.
    std::vector<std::string> heard;
    for (std::size_t i = heard_before; i < d.log->heard.size(); ++i) {
        heard.push_back(d.log->heard[i]);
    }
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == std::string(versioned::ActivationObserved::zen_name));
    CHECK(d.log->answers_ask_deliveries == 1); // the readiness answer, and nothing else

    // A LATER REAL ASK IS ANSWERED NORMALLY — production works, which is also
    // the proof that the correction is per-delivery rather than a mute weave.
    CHECK(d.ask(bus) == "v2");
    // ...and the capability the activation kept still could not be spent.
    CHECK(state_field(bus, d.candidate, "act_late_spend") == 0);
}

// ---- R2B-4a: one good handle ------------------------------------------------
//
// The substrate is complete; this is its authoring surface. `loom::
// PreparedReplacement` composes the accepted primitives — it validates nothing
// twice, caches nothing, decides nothing, and every one of its operations
// visibly delegates. What these cases prove is exactly that: the sugar removed
// the plumbing (id-carrying, authority wiring, cleanup branches) and removed
// NOTHING else — not a state, not a refusal reason, not a decision.

namespace {

// The handle is honest about what it is at compile time: bound to a host
// context at construction, never copied, moveable.
static_assert(!std::is_copy_constructible_v<PreparedReplacement>);
static_assert(!std::is_copy_assignable_v<PreparedReplacement>);
static_assert(!std::is_default_constructible_v<PreparedReplacement>);
static_assert(std::is_move_constructible_v<PreparedReplacement>);
static_assert(std::is_move_assignable_v<PreparedReplacement>);

/// The coordinator behaviour a facade author actually writes: when the
/// candidate's domain answer arrives, OFFER the delivery being handled to the
/// handle's gate and let the bus judge. The payload's `transaction` field is
/// deliberately never read — the R2B-4a question, answered in the affirmative:
/// domain payloads need no transaction id, because the bus (not the payload)
/// proves which conversation an answer belongs to. `which` is a pointer so a
/// test can retarget the same coordinator at a second replacement.
void offer_through(DynCast& d, PreparedReplacement*& which, std::vector<TxnResult>& offers) {
    d.coordinator.weave->on_handle = [&which, &offers](const Message& in, Bus&, ProbeWeave&) {
        const std::string_view shape = in.payload.schema().name();
        if (shape == std::string_view(versioned::CandidateReady::zen_name)) {
            offers.push_back(which->offer_current_answer(PreparationAnswer::Ready));
        } else if (shape == std::string_view(versioned::CandidateRefused::zen_name)) {
            offers.push_back(which->offer_current_answer(PreparationAnswer::Refused));
        }
    };
}

/// A native sealed candidate for the existing-candidate road: accepts the
/// activation contract plus Ping (the preparation ask), and authentically
/// answers Pong to any ask it hears.
WeaveId native_candidate(Switchboard& bus, WeaveId coordinator, ProbeWeave** raw_out = nullptr) {
    auto weave = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        schema_of<loom::Activated>(), ping_schema()});
    weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == std::string_view("Ping")) {
            (void)b.answer(Message(pong(in.payload.get("seq")->as_int())));
        }
    };
    if (raw_out != nullptr) {
        *raw_out = weave.get();
    }
    const WeaveId id = bus.register_weave(std::move(weave), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(id, coordinator));
    return id;
}

} // namespace

TEST_CASE("R2B-4a: the facade vertical — a Night-Lab-shaped v1->v2 replacement drives only the "
          "handle, and the substrate underneath is unchanged") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);

    // 1. v1 is the live service.
    REQUIRE(d.ask(bus) == "v1");

    // 2-3. One handle; the dynamic candidate starts THROUGH it. No raw
    // prepared-replacement call appears anywhere in this case.
    PreparedReplacement upgrade(bus, kernel);
    CHECK_FALSE(upgrade.started());
    const PreparedReplacement::StartResult started = upgrade.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 16,
    });
    REQUIRE_MESSAGE(started.ok, "stage=", static_cast<int>(started.stage), " ", started.error);
    REQUIRE(upgrade.started());
    d.candidate = upgrade.candidate();
    auto log = watch(bus, upgrade.candidate());

    // 4-5. The candidate is genuinely sealed; the incumbent — resolved from the
    // role by the facade, never supplied — still serves.
    CHECK(bus.sealed(upgrade.candidate()));
    CHECK(upgrade.incumbent() == d.incumbent);
    CHECK(upgrade.role() == kService);
    CHECK(upgrade.candidate_name() == "v2");
    CHECK(upgrade.state() == TxnState::Preparing);
    REQUIRE(d.ask(bus) == "v1");

    // 6-8. The ask goes through the handle — the payload carries NO transaction
    // id — and the coordinator offers the delivery it is handling to the gate.
    PreparedReplacement* live_handle = &upgrade;
    std::vector<TxnResult> offers;
    offer_through(d, live_handle, offers);
    versioned::PrepareReplacement ask;
    ask.plan = "ready";
    ask.escape_to = static_cast<std::int64_t>(d.observer.id.value);
    REQUIRE(upgrade.ask(ask).ok);
    bus.pump(); // pumping is always the caller's

    // 9-10. Real Ready — the handle's word is the Switchboard's word — and the
    // incumbent still serves.
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].ok);
    CHECK(upgrade.state() == TxnState::Ready);
    CHECK(bus.transaction_state(upgrade.id()) == TxnState::Ready);
    REQUIRE(d.ask(bus) == "v1");

    // 11-13. Commit means SCHEDULED. Production is queued first so ordering is
    // observable; after commit the world is untouched until the dispatch.
    bus.send_as_to_role(d.observer.id, kService,
                        Message(to_value(versioned::QueryVersion{9}), d.observer.id,
                                d.observer.id, 0));
    const std::size_t deliveries_before = log->delivery_mark();
    REQUIRE(upgrade.commit(41).ok);
    CHECK(upgrade.state() == TxnState::AdmissionPending);
    CHECK(bus.transaction_state(upgrade.id()) == TxnState::AdmissionPending);
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK(bus.sealed(upgrade.candidate()));
    CHECK_FALSE(bus.sealed(d.incumbent));
    CHECK_FALSE(upgrade.take_outcome().has_value()); // nothing has ended

    // 14-17. One EXTERNAL pump: activation first, then the queued production;
    // the role moves; v2 serves.
    bus.pump();
    const std::vector<std::string> live = log->delivered_since(deliveries_before);
    REQUIRE(live.size() >= 2);
    CHECK(live[0] == std::string(loom::Activated::zen_name));
    CHECK(live[1] == std::string(versioned::QueryVersion::zen_name));
    CHECK(bus.role_holder(kService) == upgrade.candidate());
    CHECK(state_field(bus, upgrade.candidate(), "activations") == 1);
    CHECK(state_field(bus, upgrade.candidate(), "last_activation") == 41);
    CHECK(d.ask(bus) == "v2");

    // 18-20. Exactly one Committed outcome, consumed once; the raw state agrees;
    // and the handle's incumbent is the one that was BOUND, not a re-resolution
    // of a role that has since moved on.
    const std::optional<TxnOutcome> outcome = upgrade.take_outcome();
    REQUIRE(outcome.has_value());
    CHECK(outcome->id == upgrade.id());
    CHECK(outcome->state == TxnState::Committed);
    CHECK(outcome->reason == TxnReason::None);
    CHECK_FALSE(upgrade.take_outcome().has_value());
    CHECK(upgrade.incumbent() == d.incumbent);
    CHECK(bus.sealed(d.incumbent)); // retirement-private, exactly as the raw law says
}

TEST_CASE("R2B-4a: the deferred candidate is identical from the coordinator's side") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
    REQUIRE(d.ask(bus) == "v1");

    PreparedReplacement upgrade(bus, kernel);
    REQUIRE(upgrade.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 16,
    }).ok);
    d.candidate = upgrade.candidate();

    // THE SAME COORDINATOR CODE as the immediate case — that is the assertion.
    PreparedReplacement* live_handle = &upgrade;
    std::vector<TxnResult> offers;
    offer_through(d, live_handle, offers);

    versioned::PrepareReplacement ask;
    ask.plan = "defer";
    ask.escape_to = static_cast<std::int64_t>(d.observer.id.value);
    REQUIRE(upgrade.ask(ask).ok);
    bus.pump();

    // The candidate took the answer right away with it: no answer yet, budget
    // spendable while it works, the transaction honestly still Preparing.
    CHECK(offers.empty());
    CHECK(upgrade.state() == TxnState::Preparing);
    CHECK(state_field(bus, upgrade.candidate(), "deferred") == 1);
    REQUIRE(upgrade.tick().ok);

    // The continuation is ordinary coordinator speech (application vocabulary,
    // not a transaction operation) — and the answer it produces reaches the
    // SAME hook, which offers it the same way.
    bus.send_as(d.coordinator.id, upgrade.candidate(),
                Message(to_value(versioned::ContinuePreparation{})));
    bus.pump();
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].ok);
    CHECK(upgrade.state() == TxnState::Ready);
    REQUIRE(d.ask(bus) == "v1"); // readiness is not admission

    REQUIRE(upgrade.commit(7).ok);
    bus.pump();
    CHECK(d.ask(bus) == "v2");
    const std::optional<TxnOutcome> outcome = upgrade.take_outcome();
    REQUIRE(outcome.has_value());
    CHECK(outcome->state == TxnState::Committed);
}

TEST_CASE("R2B-4a: the candidate's refusal arrives whole — reason, cleanup, and a serving "
          "incumbent, with no facade interpretation on top") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
    REQUIRE(d.ask(bus) == "v1");

    PreparedReplacement upgrade(bus, kernel);
    REQUIRE(upgrade.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 16,
    }).ok);
    d.candidate = upgrade.candidate();
    PreparedReplacement* live_handle = &upgrade;
    std::vector<TxnResult> offers;
    offer_through(d, live_handle, offers);

    versioned::PrepareReplacement ask;
    ask.plan = "refuse";
    ask.escape_to = static_cast<std::int64_t>(d.observer.id.value);
    REQUIRE(upgrade.ask(ask).ok);
    bus.pump();

    // The candidate said no, authentically; the transaction ended once with the
    // candidate's OWN reason; the substrate discarded the candidate and released
    // its artifact; the incumbent never learned any of it happened.
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].ok);
    CHECK(offers[0].why == TxnReason::CandidateRefused);
    CHECK(upgrade.state() == TxnState::Aborted);
    const std::optional<TxnOutcome> outcome = upgrade.take_outcome();
    REQUIRE(outcome.has_value());
    CHECK(outcome->state == TxnState::Aborted);
    CHECK(outcome->reason == TxnReason::CandidateRefused); // never flattened
    CHECK_FALSE(upgrade.take_outcome().has_value());
    CHECK_FALSE(kernel.is_loaded("v2"));
    CHECK(kernel.status("v2") == ArtifactStatus::NotLoaded);
    CHECK(bus.role_holder(kService) == d.incumbent);
    CHECK(d.ask(bus) == "v1");
}

TEST_CASE("R2B-4a: the start-failure ladder — every rung leaves the world exactly as it was") {
    Switchboard bus;
    Kernel kernel(bus);

    SUBCASE("nobody holds the role: refused before anything loads") {
        Registered coordinator = register_probe(bus, {pong_schema()});
        Registered op = register_probe(bus, {pong_schema()});
        PreparedReplacement upgrade(bus, kernel);
        const auto r = upgrade.start({
            .operator_id = op.id,
            .coordinator = coordinator.id,
            .role = "nobody.holds.this",
            .candidate_name = "v2",
            .candidate_path = ZEN_SO_VERSIONED_V2,
            .budget = 8,
        });
        CHECK_FALSE(r.ok);
        CHECK(r.stage == PreparedReplacement::StartStage::NoRoleHolder);
        CHECK_FALSE(kernel.is_loaded("v2")); // never loaded, so never leaked
        CHECK(bus.active_transactions() == 0);
        CHECK_FALSE(upgrade.started());
    }

    SUBCASE("the artifact refuses to load: the loader's words survive, no transaction exists") {
        DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
        PreparedReplacement upgrade(bus, kernel);
        const auto r = upgrade.start({
            .operator_id = d.op.id,
            .coordinator = d.coordinator.id,
            .role = kService,
            .candidate_name = "v2",
            .candidate_path = "/nonexistent/not-a-service.so",
            .budget = 8,
        });
        CHECK_FALSE(r.ok);
        CHECK(r.stage == PreparedReplacement::StartStage::CandidateLoad);
        CHECK_FALSE(r.error.empty()); // the loader's own words, not "start failed"
        CHECK(bus.active_transactions() == 0);
        CHECK_FALSE(kernel.is_loaded("v2"));
        CHECK(d.ask(bus) == "v1");
    }

    SUBCASE("begin refuses after a successful load: the candidate is removed exactly once and "
            "the name is reusable") {
        DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
        // A REAL refusal road: somebody is already replacing this incumbent.
        ProbeWeave* other_raw = nullptr;
        const WeaveId other = native_candidate(bus, d.coordinator.id, &other_raw);
        PreparedReplacement occupier(bus);
        REQUIRE(occupier.start_existing({
            .operator_id = d.op.id,
            .coordinator = d.coordinator.id,
            .role = kService,
            .candidate = other,
            .budget = 8,
        }).ok);

        LifetimeDelta cleanup;
        PreparedReplacement upgrade(bus, kernel);
        const auto r = upgrade.start({
            .operator_id = d.op.id,
            .coordinator = d.coordinator.id,
            .role = kService,
            .candidate_name = "v2",
            .candidate_path = ZEN_SO_VERSIONED_V2,
            .budget = 8,
        });
        CHECK_FALSE(r.ok);
        CHECK(r.stage == PreparedReplacement::StartStage::BeginTransaction);
        CHECK(r.begin_reason == TxnReason::IncumbentBusy); // the substrate's exact reason
        CHECK_FALSE(r.cleanup_failed);
        CHECK_FALSE(upgrade.started());
        // Removed exactly once: one instance created and one destroyed, one
        // library opened and one closed — the whole failed start, measured.
        CHECK(cleanup.created() == 1);
        CHECK(cleanup.destroyed() == 1);
        CHECK(cleanup.opened() == 1);
        CHECK(cleanup.closed() == 1);
        artifact_released(kernel, "v2", ZEN_SO_VERSIONED_V2);
        const LoadResult again = kernel.load("v2", ZEN_SO_VERSIONED_V2);
        REQUIRE(again.ok); // the name is genuinely reusable...
        REQUIRE(kernel.unload("v2"));
        CHECK(d.ask(bus) == "v1"); // ...and the incumbent never noticed anything
    }

    SUBCASE("begin refuses around a candidate the CALLER brought: the facade destroys nothing") {
        DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
        Registered other_coordinator = register_probe(bus, {pong_schema()});
        // Sealed to a DIFFERENT coordinator than the transaction names — a
        // representable coordinator mismatch, refused by begin.
        const WeaveId candidate = native_candidate(bus, other_coordinator.id);
        PreparedReplacement upgrade(bus);
        const auto r = upgrade.start_existing({
            .operator_id = d.op.id,
            .coordinator = d.coordinator.id,
            .role = kService,
            .candidate = candidate,
            .budget = 8,
        });
        CHECK_FALSE(r.ok);
        CHECK(r.stage == PreparedReplacement::StartStage::BeginTransaction);
        CHECK(r.begin_reason == TxnReason::CoordinatorChanged);
        // THE CALLER BROUGHT IT; THE CALLER KEEPS IT. Alive, still sealed to its
        // real owner, untouched.
        CHECK(bus.alive(candidate));
        CHECK(bus.sealed(candidate));
        CHECK(bus.candidate_owner(candidate).who == other_coordinator.id);
    }
}

TEST_CASE("R2B-4a: a delivery offered to the wrong handle refuses, consumes nothing, and the "
          "right handle still collects it") {
    // Two concurrent replacements under ONE coordinator — the case that decides
    // whether the facade's answer surface is genuinely transaction-bound.
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered op = register_probe(bus, {pong_schema()});
    const WeaveId inc_a = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "role-a");
    const WeaveId inc_b = bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "role-b");
    (void)inc_a;
    (void)inc_b;
    const WeaveId cand_a = native_candidate(bus, coordinator.id);
    const WeaveId cand_b = native_candidate(bus, coordinator.id);

    PreparedReplacement a(bus);
    PreparedReplacement b(bus);
    REQUIRE(a.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                              .role = "role-a", .candidate = cand_a, .budget = 8}).ok);
    REQUIRE(b.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                              .role = "role-b", .candidate = cand_b, .budget = 8}).ok);

    // Candidate A answers; inside that ONE delivery the coordinator offers it to
    // B (wrong), then to A (right), then to A again (spent).
    std::vector<TxnResult> results;
    coordinator.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        if (in.payload.schema().name() != std::string_view("Pong")) {
            return;
        }
        results.push_back(b.offer_current_answer(PreparationAnswer::Ready));
        results.push_back(a.offer_current_answer(PreparationAnswer::Ready));
        results.push_back(a.offer_current_answer(PreparationAnswer::Ready));
    };
    REQUIRE(a.ask(Message(ping(1))).ok);
    bus.pump();

    REQUIRE(results.size() == 3);
    CHECK_FALSE(results[0].ok); // the wrong handle...
    CHECK(results[0].why == TxnReason::InvalidReadiness);
    CHECK(b.state() == TxnState::Preparing); // ...moved nothing
    CHECK(results[1].ok); // the wrong offer consumed nothing: the right one lands
    CHECK(a.state() == TxnState::Ready);
    // The second offer to A refuses as WrongState, not InvalidReadiness — the
    // accepted readiness already moved the state machine, and the state check
    // answers first (the substrate's recorded law, reported rather than bent:
    // the consumed-conversation term is reachable only when the state did NOT
    // move, which is the role-drift road R2B-3b-3 pinned).
    CHECK_FALSE(results[2].ok); // one ask, one answer — the second offer is spent
    CHECK(results[2].why == TxnReason::WrongState);

    // ...and offered OUTSIDE any delivery, the gate refuses the same way.
    CHECK(b.offer_current_answer(PreparationAnswer::Ready).why == TxnReason::InvalidReadiness);
}

TEST_CASE("R2B-4a: two handles, one operator — each collects exactly its own outcome") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered op = register_probe(bus, {pong_schema()});
    (void)bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "role-a");
    (void)bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "role-b");
    const WeaveId cand_a = native_candidate(bus, coordinator.id);
    const WeaveId cand_b = native_candidate(bus, coordinator.id);

    PreparedReplacement a(bus);
    PreparedReplacement b(bus);
    REQUIRE(a.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                              .role = "role-a", .candidate = cand_a, .budget = 8}).ok);
    REQUIRE(b.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                              .role = "role-b", .candidate = cand_b, .budget = 8}).ok);

    // B ends first. A — same operator — must see NOTHING: not B's outcome, and
    // no outcome of its own, because A has not ended.
    REQUIRE(b.abort().ok);
    CHECK_FALSE(a.take_outcome().has_value());
    const std::optional<TxnOutcome> b_out = b.take_outcome();
    REQUIRE(b_out.has_value()); // B's result was still there to collect
    CHECK(b_out->id == b.id());
    CHECK(b_out->reason == TxnReason::ExplicitAbort);

    REQUIRE(a.abort().ok);
    const std::optional<TxnOutcome> a_out = a.take_outcome();
    REQUIRE(a_out.has_value());
    CHECK(a_out->id == a.id());
    CHECK_FALSE(a.take_outcome().has_value()); // consumed once, each
}

TEST_CASE("R2B-4a: dropping a live handle changes nothing — a scope is not a lifecycle decision") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
    REQUIRE(d.ask(bus) == "v1");

    TxnId id{};
    WeaveId candidate{};
    {
        PreparedReplacement upgrade(bus, kernel);
        REQUIRE(upgrade.start({
            .operator_id = d.op.id,
            .coordinator = d.coordinator.id,
            .role = kService,
            .candidate_name = "v2",
            .candidate_path = ZEN_SO_VERSIONED_V2,
            .budget = 8,
        }).ok);

        // Move once: the binding travels whole, and the source is unbound.
        PreparedReplacement moved = std::move(upgrade);
        CHECK_FALSE(upgrade.started());
        CHECK(moved.started());
        id = moved.id();
        candidate = moved.candidate();
        CHECK(moved.state() == TxnState::Preparing);
        // ...and the moved-into handle is dropped here, live, with no abort call.
    }

    // No hidden abort, no hidden unload, no hidden pump: the transaction and its
    // candidate are exactly as the Switchboard says, and the queue is untouched.
    CHECK(bus.transaction_active(id));
    CHECK(bus.transaction_state(id) == TxnState::Preparing);
    CHECK(bus.sealed(candidate));
    CHECK(kernel.is_loaded("v2"));
    CHECK(bus.pending() == 0);
    CHECK(d.ask(bus) == "v1");

    // Explicit teardown, through the raw trusted surface this time.
    REQUIRE(bus.abort_prepared_replacement(id, d.op.id).ok);
    TxnOutcome out{};
    REQUIRE(bus.take_outcome(d.op.id, out));
    CHECK(out.id == id);
    CHECK_FALSE(kernel.is_loaded("v2")); // the SUBSTRATE discarded it, as its law says
}

TEST_CASE("R2B-4a: the handle's state is the Switchboard's, under every mutation the facade "
          "never saw coming") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
    PreparedReplacement upgrade(bus, kernel);
    REQUIRE(upgrade.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 16,
    }).ok);
    REQUIRE(upgrade.state() == TxnState::Preparing);

    SUBCASE("the coordinator dies underneath it") {
        bus.kill(d.coordinator.id);
        CHECK(upgrade.state() == TxnState::Aborted); // immediately, no refresh call
        const auto out = upgrade.take_outcome();
        REQUIRE(out.has_value());
        CHECK(out->reason == TxnReason::CoordinatorChanged);
    }
    SUBCASE("the candidate is reloaded underneath it") {
        REQUIRE(bus.swap_state(upgrade.candidate(),
                               bus.snapshot_bytes(upgrade.candidate())).revived);
        CHECK(upgrade.state() == TxnState::Aborted);
        const auto out = upgrade.take_outcome();
        REQUIRE(out.has_value());
        CHECK(out->reason == TxnReason::CandidateChanged);
    }
    SUBCASE("the transaction is aborted through the RAW surface") {
        REQUIRE(bus.abort_prepared_replacement(upgrade.id(), d.op.id).ok);
        CHECK(upgrade.state() == TxnState::Aborted);
    }
    SUBCASE("the pending admission dispatches") {
        PreparedReplacement* live_handle = &upgrade;
        std::vector<TxnResult> offers;
        offer_through(d, live_handle, offers);
        versioned::PrepareReplacement ask;
        ask.plan = "ready";
        REQUIRE(upgrade.ask(ask).ok);
        bus.pump();
        REQUIRE(upgrade.state() == TxnState::Ready);
        REQUIRE(upgrade.commit(3).ok);
        CHECK(upgrade.state() == TxnState::AdmissionPending);
        bus.pump();
        CHECK(upgrade.state() == TxnState::Committed); // read straight off the store
        REQUIRE(upgrade.take_outcome().has_value());
    }
}

TEST_CASE("R2B-4a: the activation sequence is the caller's, passed through exactly — and gaps "
          "obey the existing law") {
    Switchboard bus;
    Kernel kernel(bus);
    DynCast d = load_pair(bus, kernel, Grant{}.allow_any(), /*with_candidate=*/false);
    REQUIRE(d.ask(bus) == "v1");

    // First replacement, with a distinctive sequence.
    PreparedReplacement first(bus, kernel);
    REQUIRE(first.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 8,
    }).ok);
    d.candidate = first.candidate();
    PreparedReplacement* live_handle = &first;
    std::vector<TxnResult> offers;
    offer_through(d, live_handle, offers);
    versioned::PrepareReplacement ask;
    ask.plan = "ready";
    REQUIRE(first.ask(ask).ok);
    bus.pump();
    REQUIRE(first.state() == TxnState::Ready);
    REQUIRE(first.commit(31337).ok);
    bus.pump();
    CHECK(d.ask(bus) == "v2");
    // The candidate observed EXACTLY the caller's number, through the same
    // attested-sequence check the Zengine cursor applies.
    CHECK(state_field(bus, first.candidate(), "activations") == 1);
    CHECK(state_field(bus, first.candidate(), "last_activation") == 31337);
    REQUIRE(first.take_outcome().has_value());

    // Second replacement over the NEW incumbent — the facade re-resolves the
    // role at ITS start, finding v2 — with a gapped higher sequence. Gaps are
    // legal (the existing law), and the facade neither invents nor reuses.
    PreparedReplacement second(bus, kernel);
    REQUIRE(second.start({
        .operator_id = d.op.id,
        .coordinator = d.coordinator.id,
        .role = kService,
        .candidate_name = "v2b",
        .candidate_path = ZEN_SO_VERSIONED_V2,
        .budget = 8,
    }).ok);
    CHECK(second.incumbent() == first.candidate()); // v2 is the incumbent now
    live_handle = &second;
    REQUIRE(second.ask(ask).ok);
    bus.pump();
    REQUIRE(second.state() == TxnState::Ready);
    REQUIRE(second.commit(31400).ok);
    bus.pump();
    CHECK(state_field(bus, second.candidate(), "last_activation") == 31400);
    REQUIRE(second.take_outcome().has_value());
}

TEST_CASE("R2B-4a: the budget is the author's, one unit per tick, and nothing ticks it secretly") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered op = register_probe(bus, {pong_schema()});
    (void)bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    const WeaveId candidate = native_candidate(bus, coordinator.id);

    SUBCASE("exactly one unit per tick") {
        PreparedReplacement r(bus);
        REQUIRE(r.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                                  .role = "worker", .candidate = candidate, .budget = 2}).ok);
        REQUIRE(r.tick().ok);                                  // 2 -> 1
        const TxnResult exhausted = r.tick();                  // 1 -> 0: aborts
        CHECK_FALSE(exhausted.ok);
        CHECK(exhausted.why == TxnReason::PreparationExhausted); // still visible, still exact
        CHECK(r.state() == TxnState::Aborted);
    }
    SUBCASE("a budget of one survives the whole ceremony: nothing else spends it") {
        PreparedReplacement r(bus);
        REQUIRE(r.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                                  .role = "worker", .candidate = candidate, .budget = 1}).ok);
        std::vector<TxnResult> offers;
        coordinator.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
            if (in.payload.schema().name() == std::string_view("Pong")) {
                offers.push_back(r.offer_current_answer(PreparationAnswer::Ready));
            }
        };
        REQUIRE(r.ask(Message(ping(1))).ok);
        bus.pump();
        REQUIRE(offers.size() == 1);
        REQUIRE(offers[0].ok);
        // Ask, delivery, answer, offer, readiness — and the one budget unit is
        // still there, because none of those is a tick.
        CHECK(r.state() == TxnState::Ready);
        REQUIRE(r.commit(2).ok);
        bus.pump();
        REQUIRE(r.take_outcome().has_value());
    }
}

TEST_CASE("R2B-4a: every refusal keeps the substrate's own words") {
    Switchboard bus;
    Kernel kernel(bus);
    Registered coordinator = register_probe(bus, {pong_schema()});
    Registered op = register_probe(bus, {pong_schema()});
    (void)bus.register_weave(
        std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()}),
        Grant{}.allow_any(), "worker");
    const WeaveId candidate = native_candidate(bus, coordinator.id);

    // An unstarted handle refuses every transaction operation with the
    // substrate's own word for "there is no such transaction".
    PreparedReplacement unstarted(bus);
    CHECK(unstarted.tick().why == TxnReason::NoSuchTransaction);
    CHECK(unstarted.ask(Message(ping(1))).why == TxnReason::NoSuchTransaction);
    CHECK(unstarted.commit(1).why == TxnReason::NoSuchTransaction);
    CHECK(unstarted.abort().why == TxnReason::NoSuchTransaction);
    CHECK_FALSE(unstarted.take_outcome().has_value());

    PreparedReplacement r(bus);
    REQUIRE(r.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                              .role = "worker", .candidate = candidate, .budget = 8}).ok);
    // A second start on a bound handle refuses as the FACADE's own fact — the
    // one truth it owns is which transaction it is.
    const auto again = r.start_existing({.operator_id = op.id, .coordinator = coordinator.id,
                                         .role = "worker", .candidate = candidate, .budget = 8});
    CHECK_FALSE(again.ok);
    CHECK(again.stage == PreparedReplacement::StartStage::AlreadyStarted);
    // Dynamic start without a Kernel refuses as itself, too.
    PreparedReplacement kernel_less(bus);
    const auto no_kernel = kernel_less.start({.operator_id = op.id,
                                              .coordinator = coordinator.id,
                                              .role = "worker",
                                              .candidate_name = "x",
                                              .candidate_path = "/nowhere",
                                              .budget = 8});
    CHECK_FALSE(no_kernel.ok);
    CHECK(no_kernel.stage == PreparedReplacement::StartStage::NoKernel);

    // One conversation, exactly: the second ask is the substrate's refusal,
    // untranslated.
    REQUIRE(r.ask(Message(ping(1))).ok);
    CHECK(r.ask(Message(ping(2))).why == TxnReason::PreparationAlreadyAsked);

    // After the transaction ends, the handle's operations answer as the
    // substrate answers for a transaction that is over.
    REQUIRE(r.abort().ok);
    CHECK(r.abort().why == TxnReason::NoSuchTransaction);
    CHECK(r.commit(1).why == TxnReason::NoSuchTransaction);
    const auto out = r.take_outcome();
    REQUIRE(out.has_value());
    CHECK(out->reason == TxnReason::ExplicitAbort);
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

// ---- R2D-0: office authorship across the dynamic seam (ABI v5) ---------------
//
// Full semantic parity: the same `mail.as_role(...)` a native weave writes, the
// same `mail.authored_from_role(...)` a native recipient reads — spoken and
// heard on the far side of the .so seam, with the HOST verifying membership at
// every authorship moment. The fixture is deliberately obedient (it attempts
// whatever it is told, including offices it does not hold) so these cases
// measure the host's verdicts, not the fixture's manners.

namespace {

/// One durable record of a delivery the office suite's native probes heard.
struct OfficeHeard {
    std::string schema;
    std::string authored_role;
    // OfficeReport fields, when the delivery was one.
    std::string what;
    bool authored = false;
    std::int64_t recipients = 0;
    std::string seen_role;
};

void record_office(std::shared_ptr<std::vector<OfficeHeard>> log, const Message& in) {
    OfficeHeard h;
    h.schema = in.payload.schema().name();
    h.authored_role = std::string(in.provenance.authored_role());
    if (h.schema == office::OfficeReport::zen_name) {
        const office::OfficeReport r = from_value<office::OfficeReport>(in.payload);
        h.what = r.what;
        h.authored = r.authored;
        h.recipients = r.recipients;
        h.seen_role = r.seen_role;
    }
    log->push_back(std::move(h));
}

/// The reports named `what`, in arrival order.
std::vector<OfficeHeard> reports_named(const std::vector<OfficeHeard>& log,
                                       const std::string& what) {
    std::vector<OfficeHeard> out;
    for (const OfficeHeard& h : log) {
        if (h.schema == office::OfficeReport::zen_name && h.what == what) {
            out.push_back(h);
        }
    }
    return out;
}

/// The WorkerNews deliveries, in arrival order.
std::vector<OfficeHeard> news_of(const std::vector<OfficeHeard>& log) {
    std::vector<OfficeHeard> out;
    for (const OfficeHeard& h : log) {
        if (h.schema == office::WorkerNews::zen_name) {
            out.push_back(h);
        }
    }
    return out;
}

struct OfficeStage {
    Switchboard bus;
    Kernel kernel{bus};
    std::shared_ptr<std::vector<OfficeHeard>> commander_log =
        std::make_shared<std::vector<OfficeHeard>>();
    std::shared_ptr<std::vector<OfficeHeard>> dispatcher_log =
        std::make_shared<std::vector<OfficeHeard>>();
    WeaveId commander{};
    WeaveId dispatcher{};
    WeaveId worker{};

    OfficeStage() {
        auto cmd = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
            schema_of<office::OfficeReport>(), schema_of<office::WorkerNews>()});
        auto cmd_log = commander_log;
        cmd->on_handle = [cmd_log](const Message& in, Bus&, ProbeWeave&) {
            record_office(cmd_log, in);
        };
        commander = bus.register_weave(std::move(cmd), Grant{}.allow_any(), "commander");

        auto dsp = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
            schema_of<office::WorkerNews>()});
        auto dsp_log = dispatcher_log;
        dsp->on_handle = [dsp_log](const Message& in, Bus&, ProbeWeave&) {
            record_office(dsp_log, in);
        };
        dispatcher = bus.register_weave(std::move(dsp), Grant{}.allow_any(), "dispatcher");

        LoadResult lr = kernel.load("worker", ZEN_SO_OFFICE_WORKER, "worker.a");
        REQUIRE(lr.ok);
        worker = lr.id;
    }

    void command(const char* mode, WeaveId target = WeaveId{}) {
        office::OfficeCommand c;
        c.mode = mode;
        c.target = static_cast<std::int64_t>(target.value);
        bus.send_as(commander, worker, Message(to_value(c)));
        bus.pump();
    }
};

} // namespace

TEST_CASE("R2D-0/v5: a loaded weave deliberately authors office speech through every door, and "
          "its personal speech stays personal") {
    OfficeStage s;

    s.command("direct", s.commander);
    s.command("to-role");
    s.command("publish");
    s.command("personal");

    // Outbound verdicts, reported from inside the .so: every office act was
    // authored; the publication counted its real recipients (commander,
    // dispatcher, and the worker itself accept WorkerNews); personal was not.
    REQUIRE(reports_named(*s.commander_log, "direct").size() == 1);
    CHECK(reports_named(*s.commander_log, "direct")[0].authored);
    REQUIRE(reports_named(*s.commander_log, "to-role").size() == 1);
    CHECK(reports_named(*s.commander_log, "to-role")[0].authored);
    REQUIRE(reports_named(*s.commander_log, "publish").size() == 1);
    CHECK(reports_named(*s.commander_log, "publish")[0].authored);
    CHECK(reports_named(*s.commander_log, "publish")[0].recipients == 3);
    REQUIRE(reports_named(*s.commander_log, "personal").size() == 1);
    CHECK_FALSE(reports_named(*s.commander_log, "personal")[0].authored);

    // What the native listeners actually received, with the stamped fact:
    // direct + office publication authored as worker.a; the personal
    // publication — same shape, same sender — carries no office.
    const auto commander_news = news_of(*s.commander_log);
    REQUIRE(commander_news.size() == 3); // direct, office publish, personal publish
    CHECK(commander_news[0].authored_role == "worker.a");
    CHECK(commander_news[1].authored_role == "worker.a");
    CHECK(commander_news[2].authored_role.empty());
    const auto dispatcher_news = news_of(*s.dispatcher_log);
    REQUIRE(dispatcher_news.size() == 3); // role-addressed, office publish, personal publish
    CHECK(dispatcher_news[0].authored_role == "worker.a");
    CHECK(dispatcher_news[1].authored_role == "worker.a");
    CHECK(dispatcher_news[2].authored_role.empty());

    // The worker heard its own office publication as office speech, and its own
    // personal publication as personal — the inbound facts crossed the seam.
    const auto heard = reports_named(*s.commander_log, "heard");
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].authored);
    CHECK(heard[0].seen_role == "worker.a");
    CHECK_FALSE(heard[1].authored);
    CHECK(heard[1].seen_role.empty());
}

TEST_CASE("R2D-0/v5: a loaded weave's request to speak for an office it does not hold is "
          "refused across the seam — precisely, and nothing is queued") {
    OfficeStage s;
    std::size_t denied = 0;
    s.bus.add_observer([&denied](const BusEvent& ev) {
        if (ev.kind == EventKind::Refused &&
            ev.refusal.reason == RefusalReason::RoleAuthorshipDenied) {
            ++denied;
        }
    });

    s.command("forge-direct", s.commander);
    s.command("forge-publish");

    // The library was told the truth: not authored, zero recipients.
    REQUIRE(reports_named(*s.commander_log, "forge-direct").size() == 1);
    CHECK_FALSE(reports_named(*s.commander_log, "forge-direct")[0].authored);
    REQUIRE(reports_named(*s.commander_log, "forge-publish").size() == 1);
    CHECK_FALSE(reports_named(*s.commander_log, "forge-publish")[0].authored);
    CHECK(reports_named(*s.commander_log, "forge-publish")[0].recipients == 0);
    // No forged news reached anybody — refusal is never downgrade.
    CHECK(news_of(*s.commander_log).empty());
    CHECK(news_of(*s.dispatcher_log).empty());
    // And the host's tap names the exact reason, once per attempt.
    CHECK(denied == 2);
}

TEST_CASE("R2D-0/v5: inbound office provenance crosses the seam — authored_from_role answers "
          "identically on both sides") {
    OfficeStage s;

    // Office speech from ANOTHER office: the dispatcher's own.
    REQUIRE(s.bus
                .office_send_as(s.dispatcher, "dispatcher", s.worker,
                                Message(to_value(office::WorkerNews{"flash"})))
                .valid());
    // Personal speech from the same dispatcher.
    s.bus.send_as(s.dispatcher, s.worker, Message(to_value(office::WorkerNews{"psst"})));
    s.bus.pump();

    const auto heard = reports_named(*s.commander_log, "heard");
    REQUIRE(heard.size() == 2);
    // The worker's Mail saw: authored as "dispatcher" — which is NOT worker.a,
    // so authored_from_role("worker.a") is false while the exact office is
    // visible; then personal speech with no office at all.
    CHECK_FALSE(heard[0].authored);
    CHECK(heard[0].seen_role == "dispatcher");
    CHECK_FALSE(heard[1].authored);
    CHECK(heard[1].seen_role.empty());
}

TEST_CASE("R2D-0/v5: the previous-ABI artifact refuses at load by version — no instance becomes "
          "live, no capability goes silently missing") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult stale = kernel.load("stale", ZEN_SO_STALEABI);
    CHECK_FALSE(stale.ok);
    CHECK(stale.error.find("abi_version") != std::string::npos);
    CHECK(stale.error.find(std::to_string(ZEN_ABI_VERSION - 1u)) != std::string::npos);
    CHECK(stale.error.find(std::to_string(ZEN_ABI_VERSION)) != std::string::npos);
    CHECK_FALSE(kernel.is_loaded("stale"));
    CHECK(bus.list_weaves().empty());
}

// ---- R2E-0 / S3: Senses across a real replacement ---------------------------
//
// The half of S3 that needs THIS ceremony: a committed admission overwrites the
// role holder IN PLACE, so the role never passes through unheld. That is exactly
// the case where a predecessor's office claim must survive — stamped stale, and
// never relabelled as the successor's.

namespace {

struct SenseStatus {
    std::string text;
    ZEN_SHAPE(SenseStatus, 1, ZEN_FIELD(text));
};

/// A probe that can be told to claim as an office from inside a delivery, which
/// is the only place a weave can claim at all.
struct OfficeClaimer : ProbeWeave {
    explicit OfficeClaimer(std::string office)
        : ProbeWeave({ping_schema(), schema_of<loom::Activated>()}), office_(std::move(office)) {}

    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return {schema_of<SenseStatus>()};
    }
    void handle(const Message& in, Bus& bus) override {
        ProbeWeave::handle(in, bus);
        if (!say.empty()) {
            last = bus.office_claim(office_, to_value(SenseStatus{say}));
            say.clear();
        }
    }
    std::string say;
    SenseClaimResult last{};

private:
    std::string office_;
};

} // namespace

TEST_CASE("R2E-0/S3: a committed admission moves the role in place — the predecessor's office "
          "claim survives, stamped stale, and is never attributed to the successor") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});

    auto inc = std::make_unique<OfficeClaimer>("worker");
    OfficeClaimer* incumbent = inc.get();
    const WeaveId inc_id = bus.register_weave(std::move(inc), Grant{}.allow_any(), "worker");

    auto cand = std::make_unique<OfficeClaimer>("worker");
    OfficeClaimer* successor = cand.get();
    const WeaveId cand_id = bus.register_weave(std::move(cand), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(cand_id, coordinator.id));

    // The incumbent deliberately claims AS the office.
    incumbent->say = "incumbent on duty";
    bus.send(inc_id, Message(ping(1)));
    bus.pump();
    REQUIRE(incumbent->last.accepted);
    SenseReading before = bus.observe_office("worker", "SenseStatus", 1);
    REQUIRE(before);
    CHECK(before.by.author == inc_id);
    CHECK(before.by.office_holder_is_current);

    // COMMIT THE REPLACEMENT. The role moves from the incumbent to the successor
    // inside one dispatch, never passing through unheld.
    loom::Activated fact{1};
    REQUIRE(bus.admit_candidate(cand_id, inc_id, "worker", host_lifecycle_authority(bus),
                                Message(to_value(fact)), 1));
    bus.pump();
    REQUIRE(bus.role_holder("worker") == cand_id);

    // THE MANDATORY WITNESS. The claim is still readable and still the
    // incumbent's — Loom does not relabel it, and does not delete it either.
    SenseReading after = bus.observe_office("worker", "SenseStatus", 1);
    REQUIRE(after);
    CHECK(from_value<SenseStatus>(*after.value).text == "incumbent on duty");
    CHECK(after.by.author == inc_id);           // the predecessor claimed it
    CHECK(after.by.author != cand_id);          // NOT the successor
    CHECK(after.by.office == "worker");         // as the office
    CHECK_FALSE(after.by.office_holder_is_current); // whose holder has changed
    CHECK(after.by.office_claim_is_stale());
    // ...and the successor is not considered to have claimed anything at all.
    CHECK_FALSE(bus.observe(cand_id, "SenseStatus", 1));

    // Once the successor deliberately claims as the office — legally, after its
    // activation — the role-bound view follows it and is current again.
    successor->say = "successor on duty";
    bus.send(cand_id, Message(ping(2)));
    bus.pump();
    REQUIRE(successor->last.accepted);
    SenseReading now = bus.observe_office("worker", "SenseStatus", 1);
    REQUIRE(now);
    CHECK(from_value<SenseStatus>(*now.value).text == "successor on duty");
    CHECK(now.by.author == cand_id);
    CHECK(now.by.office_holder_is_current);
    CHECK(now.by.revision == 2); // a replacement of the same office key, not a reset
}

TEST_CASE("R2E-0/S3: a SEALED candidate cannot claim as the office it does not yet hold — an "
          "office Sense cannot appear before legal activation") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {pong_schema()});
    const WeaveId inc_id = bus.register_weave(std::make_unique<OfficeClaimer>("worker"),
                                              Grant{}.allow_any(), "worker");
    auto cand = std::make_unique<OfficeClaimer>("worker");
    OfficeClaimer* candidate = cand.get();
    const WeaveId cand_id = bus.register_weave(std::move(cand), Grant{}.allow_any());
    REQUIRE(bus.seal_weave(cand_id, coordinator.id));

    // The sealed candidate tries to claim the office DURING preparation.
    candidate->say = "I am the worker";
    bus.send_as(coordinator.id, cand_id, Message(ping(1))); // its coordinator may reach it
    bus.pump();

    CHECK_FALSE(candidate->last.accepted);
    CHECK(candidate->last.why == SenseRefusal::OfficeNotHeld);
    // The office's claim is still the incumbent's business; nothing appeared.
    CHECK_FALSE(bus.observe_office("worker", "SenseStatus", 1));
    CHECK_FALSE(bus.observe(cand_id, "SenseStatus", 1));
    CHECK(bus.role_holder("worker") == inc_id);
}

// ---- R2E-0 / v6: Senses across the dynamic seam -----------------------------
//
// The parity question, and it is the whole question: do the four public verbs
// mean here what they mean natively? A Sense is meant for real loadable
// components, so a gap would make the feature host-only in practice.

namespace {

/// Read the Sense fixture's own window on itself (Counter v6). A loaded weave has
/// no window but its snapshot, so that is where the fixture puts what it learned.
struct SenseWindow {
    std::int64_t count = 0;
    bool claimed = false;
    std::int64_t revision = 0;
    bool office_denied = false;
    std::int64_t read_hp = -1;
    std::uint64_t read_author = 0;
    bool read_personal = false;
    std::string read_office;      // the office identity the reading carried, VERBATIM
    bool read_life_current = false;
    bool read_inc_current = false;
};

SenseWindow sense_window(Switchboard& bus, WeaveId id) {
    static const auto counter6 = SchemaBuilder("Counter", 6)
                                     .field("count", Kind::Int)
                                     .field("claimed", Kind::Int)
                                     .field("revision", Kind::Int)
                                     .field("office_denied", Kind::Int)
                                     .field("read_hp", Kind::Int)
                                     .field("read_author", Kind::Int)
                                     .field("read_personal", Kind::Int)
                                     .field("read_office", Kind::Text, /*required=*/false)
                                     .field("read_life_current", Kind::Int, /*required=*/false)
                                     .field("read_inc_current", Kind::Int, /*required=*/false)
                                     .build();
    Unverified u = parse(bus.snapshot_bytes(id));
    Admission a = admit(u, counter6);
    REQUIRE(a.ok());
    const Value& v = a.value();
    SenseWindow w;
    w.count = v.get("count")->as_int();
    w.claimed = v.get("claimed")->as_int() != 0;
    w.revision = v.get("revision")->as_int();
    w.office_denied = v.get("office_denied")->as_int() != 0;
    w.read_hp = v.get("read_hp")->as_int();
    w.read_author = static_cast<std::uint64_t>(v.get("read_author")->as_int());
    w.read_personal = v.get("read_personal")->as_int() != 0;
    if (const Cell* c = v.get("read_office")) {
        w.read_office = std::string(c->as_text());
    }
    if (const Cell* c = v.get("read_life_current")) {
        w.read_life_current = c->as_int() != 0;
    }
    if (const Cell* c = v.get("read_inc_current")) {
        w.read_inc_current = c->as_int() != 0;
    }
    return w;
}

} // namespace

TEST_CASE("R2E-0/v6: a LOADED weave claims, is refused a forged office claim, and reads its own "
          "claim back synchronously — the same four verbs, the same meanings") {
    Switchboard bus;
    Kernel kernel(bus);
    LoadResult dyn = kernel.load("sensor", ZEN_SO_SENSES, "",
                                 Grant{}.allow_any().allow_observe("SenseHealth", 1));
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    // DISCOVERY BEFORE ANY CLAIM: the manifest carried the claim-set, so the host
    // already knows what this artifact can provide and the shape resolves.
    const auto declared = bus.claimed_schemas(dyn.id);
    REQUIRE(declared.size() == 1);
    CHECK(declared[0]->name() == "SenseHealth");
    CHECK(bus.resolve_schema("SenseHealth", 1) != nullptr);
    CHECK(bus.retained_claim_count() == 0);

    // reply_to points the fixture's read-back at itself.
    bus.send(dyn.id, Message(ping(42), WeaveId{}, dyn.id, 0));
    bus.pump();

    const SenseWindow w = sense_window(bus, dyn.id);
    CHECK(w.claimed);            // the claim took, across the seam
    CHECK(w.revision == 1);      // ...at revision 1 under its key
    CHECK(w.office_denied);      // the forged office claim refused PRECISELY
    CHECK(w.read_hp == 42);      // the synchronous read-back saw its own claim
    CHECK(w.read_author == dyn.id.value); // with the host's authorship, not its own word
    CHECK(w.read_personal);      // and it was a personal claim, correctly unstamped

    // The host sees exactly the same claim natively — one repository, one truth.
    SenseReading host_view = bus.observe(dyn.id, "SenseHealth", 1);
    REQUIRE(host_view);
    CHECK(host_view.value->get("hp")->as_int() == 42);
    CHECK(host_view.by.author == dyn.id);
    CHECK(host_view.by.office.empty());
    CHECK(host_view.by.revision == 1);
    CHECK(bus.retained_claim_count() == 1);

    // A second claim REPLACES rather than accumulating, exactly as natively.
    bus.send(dyn.id, Message(ping(7), WeaveId{}, dyn.id, 0));
    bus.pump();
    CHECK(bus.retained_claim_count() == 1);
    CHECK(bus.observe(dyn.id, "SenseHealth", 1).by.revision == 2);

    // ...and unloading takes its keys with it.
    REQUIRE(kernel.unload("sensor"));
    CHECK(bus.retained_claim_count() == 0);
}

// ---- R2E-0a: the observed office identity IS the authored one ---------------
//
// The v6 seam first carried the office name in a fixed `char office[128]` and
// TRUNCATED at the bound. That is not a smaller answer, it is a WRONG one: a
// 200-character role came back as a plausible 127-character prefix, and nothing
// in the reading said so. A reader cannot audit "who claims this?" against an
// identity that was manufactured on the way out, and two genuinely different
// offices could arrive looking identical.
//
// The bound is gone rather than raised — a larger buffer only moves the lie
// further out — and the name now crosses through the same caller-owned
// ZenByteSink the VALUE already used. These cases are written so that restoring
// any truncation fails them.

namespace {

/// The shape the loaded Sense fixture declares, spelled natively. Agreement is
/// by content-id over the shape, so this and the artifact's own are one schema.
std::shared_ptr<const Schema> sensehealth_v1() {
    static const auto s = SchemaBuilder("SenseHealth", 1).field("hp", Kind::Int).build();
    return s;
}

/// A native office-holder that claims the shape the dynamic fixture reads, so
/// the two tiers are looking at one repository and one claim.
struct HealthOfficer : ProbeWeave {
    explicit HealthOfficer(std::string office)
        : ProbeWeave({ping_schema()}), office_(std::move(office)) {}

    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return {sensehealth_v1()};
    }
    void handle(const Message& in, Bus& bus) override {
        ProbeWeave::handle(in, bus);
        Value v(sensehealth_v1());
        v.set("hp", Cell::integer(in.payload.get("seq")->as_int()));
        last = bus.office_claim(office_, std::move(v));
    }
    SenseClaimResult last{};

private:
    std::string office_;
};

/// A role name far past the old 127-byte bound, built so that the ONLY thing
/// distinguishing two of them lives past that bound. Under truncation both
/// names collapse to the same prefix and the test cannot tell them apart —
/// which is precisely the failure being pinned.
std::string long_role(char suffix) {
    std::string r = "this-is-a-real-role-name-";
    r.append(200, 'x'); // well past 127, and past any plausible "generous" bound
    r += "-suffix-";
    r += suffix;
    return r;
}

Value sense_probe(const std::string& role) {
    static const auto s = SchemaBuilder("SenseProbe", 1).field("role", Kind::Text).build();
    Value v(s);
    v.set("role", Cell::text(role));
    return v;
}

} // namespace

TEST_CASE("R2E-0a/v6: a dynamic observation reports the EXACT authored office identity — a "
          "name past the old buffer bound crosses whole, not as a plausible prefix") {
    Switchboard bus;
    Kernel kernel(bus);

    const std::string role_a = long_role('A');
    REQUIRE(role_a.size() > 128); // the case is meaningless if it fits the old buffer

    // A native holder of the long office claims through it.
    const WeaveId officer =
        bus.register_weave(std::make_unique<HealthOfficer>(role_a), Grant{}.allow_any(), role_a);
    bus.send(officer, Message(ping(42)));
    bus.pump();

    // NATIVE: exact, and it always was — std::string imposes no bound.
    SenseReading native = bus.observe_office(role_a, "SenseHealth", 1);
    REQUIRE(native);
    CHECK(native.by.office == role_a);
    CHECK(native.by.office.size() == role_a.size());

    // DYNAMIC: the same question across the seam, and the same answer required.
    LoadResult dyn = kernel.load("reader", ZEN_SO_SENSES, "",
                                 Grant{}.allow_any().allow_observe("SenseHealth", 1));
    REQUIRE_MESSAGE(dyn.ok, dyn.error);
    bus.send(dyn.id, Message(sense_probe(role_a)));
    bus.pump();

    SenseWindow w = sense_window(bus, dyn.id);
    CHECK(w.read_hp == 42);                    // it really did read the claim
    CHECK_FALSE(w.read_personal);              // ...as an office claim
    CHECK(w.read_office.size() == role_a.size()); // nothing was dropped
    CHECK(w.read_office == role_a);             // ...and nothing was altered
    CHECK(w.read_office == native.by.office);   // native and dynamic agree exactly
}

// The generation facts have to cross the seam SEPARATELY, not just exist natively.
// `author_life_is_current` and `author_incarnation_is_current` answer different
// questions, and a live replacement is the case where they disagree — so a
// dynamic reader that collapses them, or drops the second in transport or decode,
// reports a predecessor's claim as the current incarnation's. This case runs the
// disagreement through a real .so; the native-only twin lives in suite `sense`
// (S3b) and cannot catch a seam that loses the fact.

TEST_CASE("R2E-0a/v6: a LOADED reader is told the incarnation moved while the life stood — the "
          "two generation facts cross the seam separately") {
    Switchboard bus;
    Kernel kernel(bus);

    // A short role on purpose: what this pins is the generation facts, not name
    // length (that is the pair of cases above).
    const std::string role = "clinic.duty";
    const WeaveId officer =
        bus.register_weave(std::make_unique<HealthOfficer>(role), Grant{}.allow_any(), role);
    bus.send(officer, Message(ping(42)));
    bus.pump();

    LoadResult dyn = kernel.load("reader", ZEN_SO_SENSES, "",
                                 Grant{}.allow_any().allow_observe("SenseHealth", 1));
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    // BEFORE: the claim is the current code's, on the current life. Both true,
    // as reported to the LIBRARY through ZenSenseBy.
    bus.send(dyn.id, Message(sense_probe(role)));
    bus.pump();
    const SenseWindow before = sense_window(bus, dyn.id);
    REQUIRE(before.read_hp == 42);
    CHECK(before.read_life_current);
    CHECK(before.read_inc_current);

    // NEW CODE BEHIND THE SAME LIVE ID. `swap_state` bumps the incarnation, and
    // `begin_new_life` advances only from `!alive` — so the life stands.
    REQUIRE(bus.swap_state(officer, bus.snapshot_bytes(officer)).revived);

    // AFTER: the predecessor's claim is still materialized, still says who made
    // it, and the loaded reader is told the code behind that author has been
    // replaced. A seam that derived this from life-currentness reports `true`
    // here and hides the replacement completely.
    bus.send(dyn.id, Message(sense_probe(role)));
    bus.pump();
    const SenseWindow stale = sense_window(bus, dyn.id);
    REQUIRE(stale.read_hp == 42);        // not rewritten, not withdrawn
    CHECK(stale.read_life_current);      // the life stands
    CHECK_FALSE(stale.read_inc_current); // the code does not

    // The successor claims for itself, and both read current again — so the flag
    // tracks the topology rather than latching once a swap has ever happened.
    bus.send(officer, Message(ping(7)));
    bus.pump();
    bus.send(dyn.id, Message(sense_probe(role)));
    bus.pump();
    const SenseWindow current = sense_window(bus, dyn.id);
    REQUIRE(current.read_hp == 7);
    CHECK(current.read_life_current);
    CHECK(current.read_inc_current);

    // ...and the host's own reading agrees at every step, so the seam neither
    // invented a fact nor lost one.
    const SenseReading native = bus.observe_office(role, "SenseHealth", 1);
    REQUIRE(native);
    CHECK(native.by.author_life_is_current == current.read_life_current);
    CHECK(native.by.author_incarnation_is_current == current.read_inc_current);
}

TEST_CASE("R2E-0a/v6: two long offices differing ONLY past the old bound stay distinguishable "
          "across the seam — truncation would report them as the same office") {
    Switchboard bus;
    Kernel kernel(bus);

    const std::string role_a = long_role('A');
    const std::string role_b = long_role('B');
    // The adversarial property: identical for far longer than the old buffer,
    // so a truncating seam hands back one identity for two different offices.
    REQUIRE(role_a.size() == role_b.size());
    REQUIRE(role_a.substr(0, 128) == role_b.substr(0, 128));
    REQUIRE(role_a != role_b);

    bus.register_weave(std::make_unique<HealthOfficer>(role_a), Grant{}.allow_any(), role_a);
    bus.register_weave(std::make_unique<HealthOfficer>(role_b), Grant{}.allow_any(), role_b);
    bus.send_to_role(role_a, Message(ping(1)));
    bus.send_to_role(role_b, Message(ping(2)));
    bus.pump();

    LoadResult dyn = kernel.load("reader", ZEN_SO_SENSES, "",
                                 Grant{}.allow_any().allow_observe("SenseHealth", 1));
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    bus.send(dyn.id, Message(sense_probe(role_a)));
    bus.pump();
    const SenseWindow saw_a = sense_window(bus, dyn.id);

    bus.send(dyn.id, Message(sense_probe(role_b)));
    bus.pump();
    const SenseWindow saw_b = sense_window(bus, dyn.id);

    // Each reading names ITS OWN office, and the two are not confusable.
    CHECK(saw_a.read_office == role_a);
    CHECK(saw_b.read_office == role_b);
    CHECK(saw_a.read_office != saw_b.read_office);
    // ...and they really were different claims, not one office read twice.
    CHECK(saw_a.read_hp == 1);
    CHECK(saw_b.read_hp == 2);
}

TEST_CASE("R2E-0/v6: a loaded weave without observe authority is refused a read, and its claim "
          "of an undeclared shape is refused — the seam carries the distinct reasons") {
    Switchboard bus;
    Kernel kernel(bus);
    // Full SEND authority, no observe rule: a send rule answers a different
    // question and does not become a read.
    LoadResult dyn = kernel.load("sensor", ZEN_SO_SENSES, "", Grant{}.allow_any());
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    bus.send(dyn.id, Message(ping(5), WeaveId{}, dyn.id, 0));
    bus.pump();

    const SenseWindow w = sense_window(bus, dyn.id);
    CHECK(w.claimed);       // claiming needs no observe rule — it is its own act
    CHECK(w.read_hp == -1); // ...but the read-back was refused
    // The claim really is there; the loaded reader simply may not read it.
    CHECK(bus.observe(dyn.id, "SenseHealth", 1));
    CHECK(bus.observe_as(dyn.id, dyn.id, "SenseHealth", 1).refusal ==
          SenseRefusal::NotAuthorized);
}

// ---- R2E-0 / P-011: the silent dynamic seam --------------------------------
//
// Night Lab III found that a loaded weave's emission whose shape nobody
// registered vanishes: no recipient, no BusEvent, no journal entry, and the
// shim is fire-and-forget by design. The identical NATIVE intent refuses
// loudly. These cases pin the tier-parity claim from both sides, and pin the
// three ways this fix could over-reach: a false refusal on a healthy dynamic
// send, a doubled refusal on a path that already reported one, and a
// manufactured target where none was ever named.

// Count Refused events carrying `reason`, and remember the last one whole.
struct RefusalTap {
    std::size_t count = 0;
    BusEvent last{};

    ObserverId arm(Switchboard& bus, RefusalReason reason) {
        return bus.add_observer([this, reason](const BusEvent& ev) {
            if (ev.kind == EventKind::Refused && ev.refusal.reason == reason) {
                ++count;
                last = ev;
                last.payload = nullptr; // valid only during the callback
            }
        });
    }
};

TEST_CASE("R2E-0/P-011: a loaded weave's unresolvable emission leaves ONE Loom-owned fact, "
          "naming the sender, the claimed shape and the seam that refused it") {
    Switchboard bus;
    Kernel kernel(bus);
    RefusalTap seam;
    seam.arm(bus, RefusalReason::SeamUnresolved);
    std::size_t all_refusals = 0;
    bus.add_observer([&all_refusals](const BusEvent& ev) {
        if (ev.kind == EventKind::Refused) {
            ++all_refusals;
        }
    });

    LoadResult dyn = kernel.load("lamp", ZEN_SO_SEAM_EMIT);
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    // One Ping in; the fixture reaches for `nobody.home` carrying SeamOnly v1,
    // a shape no registry in this process has ever heard of.
    bus.send(dyn.id, Message(ping(7)));
    bus.pump();

    // THE FACT THAT USED TO NOT EXIST.
    CHECK(seam.count == 1);
    CHECK(seam.last.sender == dyn.id);          // which artifact attempted it
    CHECK(seam.last.schema_name == "SeamOnly"); // which shape it claimed
    CHECK(seam.last.schema_version == 1u);
    // NO TARGET IS MANUFACTURED. The emission never reached role resolution, so
    // there is no weave to name — and inventing one would be a second lie on top
    // of the silence this fixes.
    CHECK_FALSE(seam.last.target.valid());

    // ...and it is exactly one refusal in total: the seam rejection is the only
    // thing that happened. Nothing was queued, so no delivery-time refusal
    // follows it (that is what a doubled report would look like).
    CHECK(all_refusals == 1);
}

TEST_CASE("R2E-0/P-011: the comparable NATIVE reach is still observable, and now the two tiers "
          "report at the same altitude") {
    Switchboard bus;
    Kernel kernel(bus);
    std::size_t native_refusals = 0;
    RefusalTap seam;
    seam.arm(bus, RefusalReason::SeamUnresolved);
    bus.add_observer([&native_refusals](const BusEvent& ev) {
        if (ev.kind == EventKind::Refused && ev.refusal.reason == RefusalReason::NoSuchTarget) {
            ++native_refusals;
        }
    });

    // A native weave reaching for the same unheld role with a shape it holds
    // typed: no registry resolution is involved, so it refuses at DELIVERY, as
    // NoSuchTarget — the loud failure Night Lab witnessed in the control arm.
    Registered native = register_probe(bus, {ping_schema()});
    bus.send_as_to_role(native.id, "nobody.home", Message(ping(7)));
    bus.pump();

    CHECK(native_refusals == 1);
    // The native path is a DIFFERENT refusal for a different reason, and this fix
    // does not reclassify it: the seam was never involved.
    CHECK(seam.count == 0);
}

TEST_CASE("R2E-0/P-011: an ordinary successful dynamic emission produces NO seam refusal — the "
          "diagnostic fires on rejection only") {
    Switchboard bus;
    Kernel kernel(bus);
    RefusalTap seam;
    seam.arm(bus, RefusalReason::SeamUnresolved);

    Registered listener = register_probe(bus, {pong_schema()});
    LoadResult dyn = kernel.load("dyn", ZEN_SO_WEAVE);
    REQUIRE_MESSAGE(dyn.ok, dyn.error);

    // The plain fixture replies Pong to reply_to — a shape the listener's
    // accept-set registered, so the seam resolves it and the delivery lands.
    bus.send(dyn.id, Message(ping(3), WeaveId{}, listener.id, 0));
    bus.pump();

    CHECK(seam.count == 0);
    CHECK(listener.weave->count == 1);
}

} // TEST_SUITE
