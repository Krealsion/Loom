// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A real Weave, a plain loom::Weave subclass shipped as a .so with one ZEN_EXPORT_WEAVE line.
// tests/CMakeLists.txt builds it many times, each under one or more of the ZEN_WEAVE_* switches
// below; each switch's behaviour is described at its branch in this file.

// The kernel's harness: MALFORMED_SNAPSHOT and MALFORMED_MESSAGE drop a required field;
// STATE_V2 bumps the state schema; THROW_ON_MAGIC throws on seq 0xDEAD, caught at the
// library's own ABI boundary; BEQUEATHS writes a letter, HEIR (Counter v2) inherits it, and
// WEDGED declares the ceremony and never answers; ACTIVATES records each zen.Activated in its
// persisted state, and _DRIFT and _CONFLICT add one door (a new contract; a same-named shape).

// Answers and seams: ANSWERS answers at once and ANSWERS_TWICE tries a second; DEFERS keeps
// the answer right and answers from a later handler; SEAM_EMIT sends, and SEAM_PUBLISH
// publishes, a shape no registry knows; EMITS_GREET declares Greet v1 in its emit-set, as
// {text} under GREET_CONFLICT; SENSES claims a Sense and reads offices back verbatim.

// Nested schemas: NEST_OLD accepts Box {Part v1 {a}}, NEST_NEW Box2 {Part v1 {a, b}},
// NEST_MIXED both (one artifact, two Part v1s; _REVERSED in the other order), NEST_AGREE Box
// and Whole sharing one Part; PING_DRIFT is the guide's copied weave, Ping v1 with an extra
// field; ASKS declares a capability ask.

// The isolation host's harness: CRASH_ON_MAGIC and CRASH_ON_REVIVE abort, LOW_RELOADS sets
// max_reloads to 3, SILENT never replies; NET_PROBE, FS_PROBE, FD_PROBE and ENV_PROBE report
// what the child reaches from inside its sandbox; MEM_BOMB and FORK_BOMB trip memory.max and
// pids.max.

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ZEN_WEAVE_NET_PROBE
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef ZEN_WEAVE_FS_PROBE
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef ZEN_WEAVE_FD_PROBE
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef ZEN_WEAVE_ENV_PROBE
#include <cstring>
#include <string>
extern char** environ; // the child's OWN environment — the thing under test
#endif

#if defined(ZEN_WEAVE_MEM_BOMB)
#include <unistd.h> // sysconf(_SC_PAGESIZE) — the bomb must touch every page, not one byte
#endif

#if defined(ZEN_WEAVE_FORK_BOMB)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace loom;
using namespace loom;

namespace {

#if defined(ZEN_WEAVE_MEM_BOMB)
/// WHAT THE BOMB REPORTS WHEN IT DID NOT DIE. Both are failures of the witness,
/// and they are DIFFERENT failures: an allocation the kernel refused up front
/// never produced any memory pressure, while surviving the page walk means
/// pressure was applied and containment did not act. Collapsing them would let
/// "malloc said no" be read as "the cgroup killed it", which is the one
/// substitution this witness must never make.
constexpr std::int64_t kBombAllocRefused = -101;
constexpr std::int64_t kBombSurvived = -102;

/// COMMIT ~200 MiB OF REAL, RESIDENT PAGES. A malloc-and-memset into a pointer never read is
/// dead code GCC deletes at -O2: the Release build never grew, and `isolation` failed downstream
/// at the quarantine assertion with nothing saying why. Two properties survive optimization,
/// both needed: the pointer is `volatile` (every store is observable), and there is one store
/// PER PAGE (one volatile write commits one page). Never freed: the pages must stay resident
/// for the cgroup to see them.
std::int64_t detonate() {
    const std::size_t bomb = 200UL * 1024UL * 1024UL;
    auto* memory = static_cast<unsigned char*>(std::malloc(bomb));
    if (memory == nullptr) {
        return kBombAllocRefused; // refused BEFORE any pressure; not containment
    }
    volatile unsigned char* observable = memory;

    const long queried_page_size = ::sysconf(_SC_PAGESIZE);
    const std::size_t page_size =
        queried_page_size > 0 ? static_cast<std::size_t>(queried_page_size) : 4096UL;

    for (std::size_t offset = 0; offset < bomb; offset += page_size) {
        observable[offset] = 1;
    }
    observable[bomb - 1] = 1; // the final partial page, whatever the page size is

    // Reaching this line means every page was written and nothing killed us.
    return kBombSurvived;
}
#endif

/// `seq` IS FIXTURE-LOCAL PLUMBING, AND WHAT IT MEANS DEPENDS ON WHICH VARIANT
/// OF THIS FILE WAS COMPILED — this is one source built many times under the
/// `ZEN_WEAVE_*` macros above, not one probe. Across the variants it carries a
/// logical sequence, a TCP port on loopback, a parked descriptor number, the
/// magic crash value, or a sentinel outcome. It is never a Loom delivery seq.
/// Read the variant's own handler before assuming which one you are looking at.
[[maybe_unused]] std::shared_ptr<const Schema> ping_schema() { // unused by the nested variants
#if defined(ZEN_WEAVE_PING_DRIFT)
    // THE COPIED WEAVE THAT KEPT A SHAPE'S NAME (docs/guides/writing-a-weave.md): the
    // same `Ping v1`, one field richer. Loaded beside the plain fixture it must be
    // refused at load with the registry's sentence, never admitted to disagree later.
    static const auto s = SchemaBuilder("Ping", 1)
                              .field("seq", Kind::Int)
                              .field("extra", Kind::Int)
                              .build();
#else
    static const auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
#endif
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> pong_schema() { // unused by the silent variant
    static const auto s = SchemaBuilder("Pong", 1).field("seq", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> netresult_schema() { // only the net-probe variant
    static const auto s = SchemaBuilder("NetResult", 1).field("code", Kind::Int).build();
    return s;
}
/// The one byte the net probe pushes down a connection it managed to open, so the
/// test's own listener can confirm a DATA PATH rather than a completed handshake.
/// Mirrored in `tests/test_isolation.cpp` rather than shared through a header,
/// exactly as `kBombAllocRefused` is: this fixture is a separate artifact built
/// into its own `.so`, and the two ends asserting the same literal is the point.
[[maybe_unused]] constexpr char kNetProbeToken = 'Z';
[[maybe_unused]] std::shared_ptr<const Schema> fsresult_schema() { // only the fs-probe variant
    static const auto s = SchemaBuilder("FsResult", 1)
                              .field("secret_read", Kind::Int)
                              .field("scratch_write", Kind::Int)
                              .field("outside_write", Kind::Int)
                              .field("noexec_exec", Kind::Int)
                              .build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> forkresult_schema() { // only the fork-bomb variant
    static const auto s = SchemaBuilder("ForkResult", 1).field("forked", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> envresult_schema() { // only the env-probe variant
    // The COMPLETE environment: `count` makes an unknown future variable fail on its own; the
    // other three are the named questions, kept separate so a failure says which. `names`
    // carries NAMES ONLY: a value could be a real credential from the host shell, and printing
    // one in test output would be its own leak.
    static const auto s = SchemaBuilder("EnvResult", 1)
                              .field("count", Kind::Int)
                              .field("ld_count", Kind::Int)
                              .field("secret_present", Kind::Int)
                              .field("names", Kind::Text)
                              .build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> fdresult_schema() { // only the fd-probe variant
    // FOUR SEPARATE FACTS: `fresh_connect == ENETUNREACH` can hold while an inherited connected
    // socket is usable, so the namespace verdict alone would call that host contained.
    //   open_low      WHICH descriptors exist at all, as a bitmap of fds 0..62
    //   open_high     whether any survived by living at a high number instead
    //   parked_write  whether the one this Ping names can still MOVE BYTES
    //   fresh_connect whether the network namespace is still genuinely imposed
    static const auto s = SchemaBuilder("FdResult", 1)
                              .field("open_low", Kind::Int)
                              .field("open_high", Kind::Int)
                              .field("parked_write", Kind::Int)
                              .field("fresh_connect", Kind::Int)
                              .build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> greet_schema() { // the drift and emit variants
#if defined(ZEN_WEAVE_ACTIVATES_CONFLICT) || defined(ZEN_WEAVE_GREET_CONFLICT)
    // The SAME (name, version) carrying DIFFERENT content — a cross-library
    // disagreement, which the registry's agreement wall refuses at load. Used to
    // make the kernel registry's admission of a REJECTED candidate's schemas
    // observable from outside, and (ZEN_WEAVE_GREET_CONFLICT) as the shape a
    // divergent EMITTER declares.
    static const auto s = SchemaBuilder("Greet", 1).field("text", Kind::Text).build();
#else
    static const auto s = SchemaBuilder("Greet", 1).field("msg", Kind::Text).build();
#endif
    return s;
}
#if defined(ZEN_WEAVE_NEST_OLD) || defined(ZEN_WEAVE_NEST_NEW) || defined(ZEN_WEAVE_NEST_MIXED)
// THE NESTED-COMPONENT SHAPES. `part_old()` and `part_new()` both say `Part v1`, ON PURPOSE:
// a shape that gained a field without a new version. NEST_MIXED declares a door over each, so
// one manifest carries the contradiction; the single-door variants play the two sides of a
// cross-artifact disagreement whose outer names differ (`Box`, `Box2`), which only the
// component closure can see.
std::shared_ptr<const Schema> part_old() {
    static const auto s = SchemaBuilder("Part", 1).field("a", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> part_new() {
    static const auto s =
        SchemaBuilder("Part", 1).field("a", Kind::Int).field("b", Kind::Bool).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> box_schema() { // nests Part v1 {a}
    static const auto s = SchemaBuilder("Box", 1).message("part", part_old()).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> box2_schema() { // nests Part v1 {a, b}
    static const auto s = SchemaBuilder("Box2", 1).message("part", part_new()).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> whole_schema() { // List<Part v1 {a}>: agrees with Box
    static const auto s =
        SchemaBuilder("Whole", 1).list("parts", type_message(part_old())).build();
    return s;
}
#endif
[[maybe_unused]] std::shared_ptr<const Schema> sensehealth_schema() { // only the senses variant
    // The Sense this artifact declares it can claim. Declared in the manifest's claim-set, so
    // the host registers it at load and a consumer can ask what this artifact provides before
    // it has claimed anything.
    static const auto s = SchemaBuilder("SenseHealth", 1).field("hp", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> senseprobe_schema() { // only the senses variant
    // "Observe this office and tell me EXACTLY what identity you were given." The role travels
    // as Text with no bound, because the case that uses it names an office longer than any
    // fixed buffer, and compares what this artifact saw with it byte for byte.
    static const auto s = SchemaBuilder("SenseProbe", 1).field("role", Kind::Text).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> seamonly_schema() { // the two seam variants
    // DECLARED NOWHERE ELSE. Not in accepted_schemas(), not in any other library,
    // not by the host — so no registry in the process has ever heard of it. That
    // is the whole fixture: an emission the host seam cannot resolve.
    static const auto s = SchemaBuilder("SeamOnly", 1).field("want", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> counter_schema() {
#if defined(ZEN_WEAVE_SENSES)
    // Counter v6 — the Sense fixture's own window. A loaded weave has no window on
    // itself but its snapshot, so what the test needs to see (did my claim take?
    // at what revision? what did I read back, and with what authorship?) is
    // persisted state like anything else, rather than a back channel.
    static const auto s = SchemaBuilder("Counter", 6)
                              .field("count", Kind::Int)
                              .field("claimed", Kind::Int)     // 1 = the personal claim took
                              .field("revision", Kind::Int)    // its revision under the key
                              .field("office_denied", Kind::Int) // 1 = office claim refused
                              .field("read_hp", Kind::Int)     // what it read back, -1 = nothing
                              .field("read_author", Kind::Int) // whom the reading named
                              .field("read_personal", Kind::Int) // 1 = the reading carried no office
                              // The OFFICE IDENTITY an office reading carried, verbatim. Text
                              // rather than a length or a digest because the claim under test
                              // is `observed == authored` byte for byte: anything summarised
                              // could agree while the identity itself differed.
                              .field("read_office", Kind::Text, /*required=*/false)
                              // ...and the two generation facts the reading reported, which a
                              // truncating or deriving seam would get wrong independently.
                              .field("read_life_current", Kind::Int, /*required=*/false)
                              .field("read_inc_current", Kind::Int, /*required=*/false)
                              .build();
#elif defined(ZEN_WEAVE_ANSWERS)
    // Counter v5 — the immediate-answer fixture's own window: did the board accept
    // its answer, and did it refuse a second one?
    static const auto s = SchemaBuilder("Counter", 5)
                              .field("count", Kind::Int)
                              .field("answered", Kind::Int)
                              .field("second", Kind::Int)
                              .build();
#elif defined(ZEN_WEAVE_ACTIVATES)
    // Counter v3 — the activation participant's own bookkeeping, persisted like
    // any other state so a reload TRANSPLANTS it. That is what makes "the state
    // crossed the reload before the new activation arrived" observable rather
    // than asserted: a fresh instance would read 0 activations.
    static const auto s = SchemaBuilder("Counter", 3)
                              .field("count", Kind::Int)
                              .field("activations", Kind::Int)
                              .field("last_activation", Kind::Int)
                              .build();
#elif defined(ZEN_WEAVE_DEFERS)
    // Counter v4, the deferring steward's own window: did it get a retained answer right, and
    // did spending it succeed? `token` is persisted so a RELOAD hands the successor a capability
    // that LOOKS live: the successor genuinely presents its predecessor's token, and the board
    // is the thing that has to say no.
    static const auto s = SchemaBuilder("Counter", 4)
                              .field("count", Kind::Int)
                              .field("deferred", Kind::Int)
                              .field("spent", Kind::Int)
                              .field("token", Kind::Int)
                              .build();
#elif defined(ZEN_WEAVE_STATE_V2) || defined(ZEN_WEAVE_HEIR)
    static const auto s = SchemaBuilder("Counter", 2)
                              .field("count", Kind::Int)
                              .field("note", Kind::Text, /*required=*/false)
                              .build();
#else
    static const auto s = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
#endif
    return s;
}

// Accepts Ping, replies Pong, and counts what it has handled as its state.
class TestWeave : public Weave {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
#if defined(ZEN_WEAVE_BEQUEATHS) || defined(ZEN_WEAVE_WEDGED)
        // Declaring zen.PrepareShutdown IS the opt-in: it is what the steward
        // reads (via the kernel's manifest) to decide whether to hold a ceremony.
        return {ping_schema(), schema_of<loom::PrepareShutdown>()};
#elif defined(ZEN_WEAVE_HEIR)
        return {ping_schema(), schema_of<loom::Bequest>(), schema_of<loom::Refused>()};
#elif defined(ZEN_WEAVE_ACTIVATES_DRIFT) || defined(ZEN_WEAVE_ACTIVATES_CONFLICT)
        // Declaring zen.Activated IS the opt-in, exactly as PrepareShutdown is
        // for the letter — plus ONE extra door. Same state schema, different
        // contract: the only thing a reload from the plain variant changes.
        // (The _CONFLICT twin differs only in what its Greet v1 SAYS.)
        return {ping_schema(), schema_of<loom::Activated>(), greet_schema()};
#elif defined(ZEN_WEAVE_DEFERS)
        // The dynamic steward: an ASK (Ping) it answers LATER, and the COMPLETION
        // (Greet) that tells it the answer is ready.
        return {ping_schema(), greet_schema()};
#elif defined(ZEN_WEAVE_ACTIVATES)
        return {ping_schema(), schema_of<loom::Activated>()};
#elif defined(ZEN_WEAVE_SENSES)
        // Ping drives the claim/read-back parity case; SenseProbe asks this
        // artifact to observe a named OFFICE and report the identity verbatim.
        return {ping_schema(), senseprobe_schema()};
#elif defined(ZEN_WEAVE_NEST_MIXED_REVERSED)
        return {box2_schema(), box_schema()}; // the contradiction, the other way round
#elif defined(ZEN_WEAVE_NEST_MIXED)
        return {box_schema(), box2_schema()}; // ONE declaration, two Part v1 definitions
#elif defined(ZEN_WEAVE_NEST_AGREE)
        return {box_schema(), whole_schema()}; // two doors, one shared Part v1
#elif defined(ZEN_WEAVE_NEST_OLD)
        return {box_schema()};
#elif defined(ZEN_WEAVE_NEST_NEW)
        return {box2_schema()};
#else
        return {ping_schema()};
#endif
    }

#if defined(ZEN_WEAVE_SENSES)
    /// THE DECLARED CLAIM-SET, from a raw `loom::Weave`. It rides the manifest, so the host
    /// registers SenseHealth at load and can answer "what Senses does this artifact provide?"
    /// before it has claimed anything.
    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return {sensehealth_schema()};
    }
#endif

#if defined(ZEN_WEAVE_EMITS_GREET)
    /// THE DECLARED EMIT-SET, from a raw `loom::Weave`. It rides the manifest's `emits`
    /// section, so the host claims this artifact's own definition of Greet v1 into the
    /// agreement wall at load, where a divergent acceptor meets it.
    std::vector<std::shared_ptr<const Schema>> emitted_schemas() const override {
        return {greet_schema()};
    }
#endif

    void handle(const Message& in, Bus& bus) override {
#if defined(ZEN_WEAVE_ANSWERS)
        // THE PARITY FIXTURE. One line, the public one, and the whole question is
        // whether it means here what it means natively.
        ++count_;
        Value reply(pong_schema());
        reply.set("seq", Cell::integer(in.payload.get("seq")->as_int()));
        answered_ = bus.answer(Message(reply)).valid();
#if defined(ZEN_WEAVE_ANSWERS_TWICE)
        // ...and a second attempt from the same delivery must fail, exactly as it
        // does natively: one delivered request, one answer.
        second_answer_ = bus.answer(Message(reply)).valid();
#endif
        return;
#endif
#if defined(ZEN_WEAVE_DEFERS)
        // THE DYNAMIC STEWARD (ANS-02, ANS-06): the answer is not known yet, so it takes the
        // answer right away with it and RETURNS WITHOUT ANSWERING, retaining only the opaque
        // capability (a stored `Bus&` would dangle; a stored Message carries no authority).
        // `count_` counts DELIVERIES, so the absence assertions ("it answered nobody") cannot
        // pass on a weave that never woke up.
        ++count_;
        if (in.payload.schema().name() == std::string_view("Ping")) {
            pending_ = bus.make_deferred_answer();
            deferred_ok_ = pending_.valid();
            return; // no answer. The handler ends here.
        }
        // ...and later, on an entirely separate delivery, it answers the ORIGINAL
        // request using the capability it kept and THIS handler's bus. Every Greet
        // tries; the COUNT of accepted spends is what makes "spendable once" a
        // fact observed through the dynamic path rather than one asserted natively.
        if (in.payload.schema().name() == std::string_view("Greet")) {
            Value answer(pong_schema());
            answer.set("seq", Cell::integer(count_));
            if (bus.spend_deferred(pending_, Message(answer)).valid()) {
                ++spends_ok_;
            }
            return;
        }
        return;
#endif
#if defined(ZEN_WEAVE_BEQUEATHS)
        if (in.payload.schema().name() == loom::PrepareShutdown::zen_name) {
            // The letter: what this weave wants its heir to know, said in its own
            // vocabulary. It has no idea what shape its successor is — only what
            // it itself has to say.
            loom::Bequest letter;
            letter.role = "spawner";
            Value carried(ping_schema());
            carried.set("seq", Cell::integer(count_));
            letter.items.push_back(loom::bequeath_item_value(carried));
            bus.send(in.sender, Message(loom::to_value(letter), WeaveId{}, WeaveId{},
                                        in.correlation));
            return;
        }
#elif defined(ZEN_WEAVE_WEDGED)
        if (in.payload.schema().name() == loom::PrepareShutdown::zen_name) {
            (void)bus;
            return; // declared the ceremony, then says nothing — the honest wedge
        }
#elif defined(ZEN_WEAVE_HEIR)
        if (in.payload.schema().name() == loom::Bequest::zen_name) {
            const loom::Bequest letter = loom::from_value<loom::Bequest>(in.payload);
            for (const loom::Bytes& item : letter.items) {
                // Every item is re-admitted through the real gate before a field
                // is touched: inherited mail is untrusted input like any other.
                Unverified u = parse(std::string_view(
                    reinterpret_cast<const char*>(item.data()), item.size()));
                Admission a = admit(u, ping_schema());
                if (a.ok()) {
                    count_ += a.value().get("seq")->as_int();
                }
            }
            return;
        }
        if (in.payload.schema().name() == loom::Refused::zen_name) {
            return; // nothing was left for us; start fresh, which is already true
        }
        if (!claimed_) {
            // The heir asks when it WAKES — not when it was born, and with no
            // idea how long the letter has been waiting. It reaches the steward
            // by role because that is the only address it can know.
            claimed_ = true;
            bus.send_to_role(loom::kManagerRole,
                             Message(loom::to_value(loom::ClaimBequest{"spawner"})));
        }
#elif defined(ZEN_WEAVE_ACTIVATES)
        if (in.payload.schema().name() == loom::Activated::zen_name) {
            (void)bus;
            // LIFE-04: TRUSTED BECAUSE LOOM ATTESTS IT, not because the shape arrived: Loom
            // must have authorized a lifecycle commit for THIS incarnation, and the attested
            // sequence must be the one the payload claims. Anything else is an ordinary message
            // wearing a lifecycle costume, ignored entirely: no count, no lineage, no notice.
            const std::int64_t claimed = in.payload.get("sequence")->as_int();
            if (!in.provenance.lifecycle_activation() ||
                in.provenance.attested_sequence() != claimed) {
                return;
            }
            // The whole participation: note that it happened and which one it
            // was. Deliberately nothing else — no loop is started, no prior work
            // repeated, nothing announced. Activation is a fact, not an order.
            ++activations_;
            last_activation_ = claimed;
            return; // never falls through to the Ping path below ('seq' is absent)
        }
#elif defined(ZEN_WEAVE_SENSES)
        // THE EXACT-OFFICE PROBE (SENSE-03). Observe the office this message names
        // and record the identity VERBATIM. Nothing here shortens, hashes or
        // normalises it: the test compares what came back against what it
        // authored, so a seam that truncates is caught by the comparison rather
        // than by a bound this artifact would have to know about.
        if (in.payload.schema().name() == "SenseProbe") {
            const std::string role(in.payload.get("role")->as_text());
            const SenseReading r = bus.observe_office(role, sensehealth_schema());
            if (r) {
                read_office_ = r.by.office;
                read_hp_ = r.value->get("hp")->as_int();
                read_author_ = static_cast<std::int64_t>(r.by.author.value);
                read_personal_ = r.by.office.empty();
                read_life_current_ = r.by.author_life_is_current;
                read_inc_current_ = r.by.author_incarnation_is_current;
            } else {
                read_hp_ = -1;
                read_office_.clear();
            }
            return; // never falls through to the Ping path below ('seq' is absent)
        }
#endif
#if defined(ZEN_WEAVE_NEST_OLD) || defined(ZEN_WEAVE_NEST_NEW) || defined(ZEN_WEAVE_NEST_MIXED)
        // The nested-shape variants hear Box/Box2/Whole, none of which carries `seq`.
        // What they are for is decided at LOAD, by the agreement wall; a delivery
        // here is counted and nothing more.
        (void)bus;
        ++count_;
        return;
#endif
        const std::int64_t seq = in.payload.get("seq")->as_int();
#ifdef ZEN_WEAVE_CRASH_ON_MAGIC
        if (seq == 0xDEAD) {
            std::abort(); // crash mid-handle; the isolation host must contain this
        }
#endif
#ifdef ZEN_WEAVE_THROW_ON_MAGIC
        if (seq == 0xDEAD) {
            // Deliberately an exception and not an abort: it never leaves this
            // library (the seam catches everything), so the only way the host can
            // know is the status the seam returns.
            throw std::runtime_error("loaded handler failure");
        }
#endif
        ++count_;
#if defined(ZEN_WEAVE_SENSES)
        // THE DYNAMIC-PARITY FIXTURE (SENSE-01). The same four public verbs a native weave
        // writes, from the far side of the seam, and the whole question is whether they mean
        // here what they mean natively.
        Value obs(sensehealth_schema());
        obs.set("hp", Cell::integer(seq));
        const SenseClaimResult claim = bus.claim(std::move(obs));
        sense_claimed_ = claim.accepted;
        revision_ = static_cast<std::int64_t>(claim.revision);

        // Asking to claim as an office this artifact does not hold must refuse
        // precisely — never a silent downgrade to a personal claim.
        Value forged(sensehealth_schema());
        forged.set("hp", Cell::integer(9000));
        const SenseClaimResult denied = bus.office_claim("nobody.holds.this", std::move(forged));
        office_denied_ = !denied.accepted && denied.why == SenseRefusal::OfficeNotHeld;

        // ...and read a claim back, synchronously, with the authorship the HOST
        // computed. `reply_to` is where the test points it — an address handed in
        // by the sender, which is exactly what that field is for. The test points
        // it at this weave itself, so the read-back is of its own claim.
        const SenseReading r = bus.observe(in.reply_to, sensehealth_schema());
        if (r) {
            read_hp_ = r.value->get("hp")->as_int();
            read_author_ = static_cast<std::int64_t>(r.by.author.value);
            read_personal_ = r.by.office.empty();
        } else {
            read_hp_ = -1;
        }
        return;
#elif defined(ZEN_WEAVE_SEAM_EMIT)
        // THE SILENT-SEAM FIXTURE (MSG-08): reach for a service by role with a shape no
        // registry in this process knows. The role may be unheld, but the emission is rejected
        // at the library/host seam before any target is resolved. A dynamic send returns no
        // ticket, so this weave learns nothing; the test watches the HOST side.
        Value want(seamonly_schema());
        want.set("want", Cell::integer(seq));
        bus.send_to_role("nobody.home", Message(std::move(want)));
#elif defined(ZEN_WEAVE_SEAM_PUBLISH)
        // THE SAME UNRESOLVABLE SHAPE, SPOKEN TO NOBODY: the seam-emit twin addresses a slot,
        // this one publishes, so only the door differs. Then a SECOND publication, of a shape
        // the host knows, with 'seq' absent: its shape resolves, so an accepter would have been
        // handed these bytes, a real failed delivery that keeps the first half from being mere
        // suppression.
        {
            Value want(seamonly_schema());
            want.set("want", Cell::integer(seq));
            bus.publish(Message(std::move(want)));
        }
        bus.publish(Message(Value(pong_schema()))); // 'seq' deliberately absent
#elif defined(ZEN_WEAVE_SILENT)
        (void)seq;
        (void)bus; // a deliberately silent Weave: it never replies
#elif defined(ZEN_WEAVE_MALFORMED_MESSAGE)
        (void)seq;
        bus.send(in.reply_to, Message(Value(pong_schema()))); // 'seq' deliberately absent
#elif defined(ZEN_WEAVE_NET_PROBE)
        // Instruction-level reach: open a TCP socket directly and USE it, the move a bus grant
        // cannot stop and only an OS sandbox can. The Ping's `seq` names the port of a listener
        // THE TEST OWNS on loopback, so the positive witness is success, never a closed port's
        // host-dependent errno. `code` is 0 only if the connection opened AND one byte went down
        // it; otherwise the errno (ENETUNREACH when the sandbox removed the interface).
        std::int64_t code = 0;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            code = errno != 0 ? errno : -1;
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<std::uint16_t>(seq));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            errno = 0;
            const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (rc != 0) {
                code = errno != 0 ? errno : -1;
            } else {
                errno = 0;
                const ssize_t wrote = ::write(fd, &kNetProbeToken, 1);
                code = (wrote == 1) ? 0 : (errno != 0 ? errno : -1);
            }
            ::close(fd);
        }
        Value result(netresult_schema());
        result.set("code", Cell::integer(code));
        bus.send(in.reply_to, Message(std::move(result)));
#elif defined(ZEN_WEAVE_FS_PROBE)
        (void)seq;
        // Instruction-level filesystem reach: read a secret outside the view, write
        // inside scratch, write outside it, and execute from scratch. Each reports its
        // errno (0 = succeeded) so the test reads the OS verdict off the bus — the
        // failures are the sandbox, not the grant.
        const auto try_read = [](const char* p) -> std::int64_t {
            const int fd = ::open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return errno;
            }
            ::close(fd);
            return 0;
        };
        const auto try_write = [](const char* p) -> std::int64_t {
            const int fd = ::open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) {
                return errno;
            }
            ::close(fd);
            return 0;
        };
        const std::int64_t secret = try_read("/tmp/zen_b4_secret.txt");
        const std::int64_t scratch = try_write("/scratch/probe.txt");
        const std::int64_t outside = try_write("/zen_outside.txt");
        std::int64_t noexec = 0;
        {
            const int fd = ::open("/scratch/prog", O_WRONLY | O_CREAT | O_CLOEXEC, 0755);
            if (fd < 0) {
                noexec = errno; // no scratch at all (None/ReadOnly) → report why
            } else {
                const char* script = "#!/bin/sh\nexit 0\n";
                (void)!::write(fd, script, std::strlen(script));
                ::close(fd);
                (void)::chmod("/scratch/prog", 0755);
                const pid_t gp = ::fork();
                if (gp == 0) {
                    ::execl("/scratch/prog", "prog", static_cast<char*>(nullptr));
                    ::_exit(errno); // exec refused (e.g. noexec → EACCES) → carry it out
                }
                int st = 0;
                (void)::waitpid(gp, &st, 0);
                noexec = WIFEXITED(st) ? WEXITSTATUS(st) : -1; // 0 = ran; EACCES = noexec
            }
        }
        Value result(fsresult_schema());
        result.set("secret_read", Cell::integer(secret));
        result.set("scratch_write", Cell::integer(scratch));
        result.set("outside_write", Cell::integer(outside));
        result.set("noexec_exec", Cell::integer(noexec));
        bus.send(in.reply_to, Message(std::move(result)));
#elif defined(ZEN_WEAVE_FD_PROBE)
        // THE DESCRIPTOR WITNESS, from inside the sandbox: an already-open descriptor that
        // crossed execve is simply THERE or not, whatever the grant or namespace. The Ping's
        // `seq` names the fd the host parked before spawning; writing the payload to it is the
        // escape this witness catches, and the host, holding the other end, asserts nothing
        // arrives.
        {
            // (1) What do we actually hold? A bitmap of the low numbers, plus a count of
            //     anything hiding higher up — so "the leak just moved to another fd" is
            //     not mistakable for "the leak is gone".
            std::int64_t open_low = 0;
            for (int fd = 0; fd < 63; ++fd) {
                if (::fcntl(fd, F_GETFD) != -1) {
                    open_low |= (static_cast<std::int64_t>(1) << fd);
                }
            }
            std::int64_t open_high = 0;
            for (int fd = 63; fd < 65536; ++fd) {
                if (::fcntl(fd, F_GETFD) != -1) {
                    ++open_high;
                }
            }

            // (2) Can the parked descriptor still be USED? Presence and usability are
            //     asked separately because a closed number and a live socket both answer
            //     "an int" — only a write says which.
            const char payload[] = "COLD2-ESCAPE-PAYLOAD";
            errno = 0;
            const ssize_t wrote = ::write(static_cast<int>(seq), payload, sizeof(payload) - 1);
            const std::int64_t parked_write = wrote > 0 ? 0 : (errno != 0 ? errno : -1);

            // (3) Is the network namespace still real? The control that keeps this test
            //     honest in the other direction: if a future change removed the netns,
            //     descriptor hygiene alone must not be read as containment.
            std::int64_t fresh_connect = 0;
            const int fresh = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fresh < 0) {
                fresh_connect = errno != 0 ? errno : -1;
            } else {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(1);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                errno = 0;
                const int rc = ::connect(fresh, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                fresh_connect = rc == 0 ? 0 : errno;
                ::close(fresh);
            }

            Value result(fdresult_schema());
            result.set("open_low", Cell::integer(open_low));
            result.set("open_high", Cell::integer(open_high));
            result.set("parked_write", Cell::integer(parked_write));
            result.set("fresh_connect", Cell::integer(fresh_connect));
            bus.send(in.reply_to, Message(std::move(result)));
        }
#elif defined(ZEN_WEAVE_ENV_PROBE)
        (void)seq;
        // THE ENVIRONMENT WITNESS, from inside the sandbox. It reads its OWN `environ` — the
        // thing execve actually installed — rather than asking about names a test
        // remembered to name. `count` is therefore the load-bearing field: a variable
        // some future embedding host introduces raises it without anyone editing this.
        {
            std::int64_t count = 0;
            std::int64_t ld_count = 0;
            std::string names;
            for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
                ++count;
                const char* eq = std::strchr(*e, '=');
                const std::string name =
                    eq != nullptr ? std::string(*e, static_cast<std::size_t>(eq - *e))
                                  : std::string(*e); // a nameless entry cannot happen, but say so
                if (name.rfind("LD_", 0) == 0) {
                    ++ld_count;
                }
                if (names.size() < 4000) { // bounded: this rides an ordinary framed message
                    names += name;
                    names += "\n";
                }
            }
            Value result(envresult_schema());
            result.set("count", Cell::integer(count));
            result.set("ld_count", Cell::integer(ld_count));
            result.set("secret_present",
                       Cell::integer(std::getenv("ZEN_C2A_AMBIENT_SECRET") != nullptr ? 1 : 0));
            result.set("names", Cell::text(names));
            bus.send(in.reply_to, Message(std::move(result)));
        }
#elif defined(ZEN_WEAVE_MEM_BOMB)
        // Commit a large resident block to trip memory.max, held so RSS stays high: under the
        // cgroup cap the kernel OOM-kills us mid-handle and THE REPLY BELOW IS NEVER SENT, so a
        // bomb that fails to die is caught by a Pong arriving. When it does not die, the reply
        // carries WHY as a sentinel seq: "the kernel refused the allocation" and "every page was
        // written and nothing killed us" stay distinguishable.
        {
            const std::int64_t outcome = detonate();
            Value pong(pong_schema());
            pong.set("seq", Cell::integer(outcome));
            bus.send(in.reply_to, Message(std::move(pong)));
        }
        (void)seq;
#elif defined(ZEN_WEAVE_FORK_BOMB)
        (void)seq;
        // Fork until the kernel refuses (pids.max), counting successes, then clean up.
        // Bounded ⇒ pids.max held; unbounded would fork the whole loop.
        {
            std::int64_t forked = 0;
            std::vector<pid_t> kids;
            for (int i = 0; i < 4000; ++i) {
                const pid_t k = ::fork();
                if (k == 0) {
                    ::pause(); // child: stay alive (counts against pids.max) until killed
                    ::_exit(0);
                }
                if (k < 0) {
                    break; // EAGAIN: hit pids.max
                }
                kids.push_back(k);
                ++forked;
            }
            for (pid_t k : kids) {
                ::kill(k, SIGKILL);
            }
            for (pid_t k : kids) {
                (void)::waitpid(k, nullptr, 0);
            }
            Value res(forkresult_schema());
            res.set("forked", Cell::integer(forked));
            bus.send(in.reply_to, Message(std::move(res)));
        }
#elif defined(ZEN_WEAVE_EMITS_GREET)
        // The declared emitter SAYS what it declared: a Greet built against its own
        // definition (`msg`, or `text` under GREET_CONFLICT). Whether that reaches a
        // door is the host's business; that its definition was agreed at load is the
        // fixture's whole point.
        {
            Value greet(greet_schema());
#if defined(ZEN_WEAVE_GREET_CONFLICT)
            greet.set("text", Cell::text("greet " + std::to_string(seq)));
#else
            greet.set("msg", Cell::text("greet " + std::to_string(seq)));
#endif
            bus.send(in.reply_to, Message(std::move(greet)));
        }
#else
        Value pong(pong_schema());
        pong.set("seq", Cell::integer(seq));
        bus.send(in.reply_to, Message(std::move(pong)));
#endif
    }

    Value snapshot() const override {
        Value v(counter_schema());
#ifndef ZEN_WEAVE_MALFORMED_SNAPSHOT
        v.set("count", Cell::integer(count_));
#endif
#if defined(ZEN_WEAVE_ACTIVATES)
        v.set("activations", Cell::integer(activations_));
        v.set("last_activation", Cell::integer(last_activation_));
#endif
#if defined(ZEN_WEAVE_ANSWERS)
        v.set("answered", Cell::integer(answered_ ? 1 : 0));
        v.set("second", Cell::integer(second_answer_ ? 1 : 0));
#endif
#if defined(ZEN_WEAVE_DEFERS)
        v.set("deferred", Cell::integer(deferred_ok_ ? 1 : 0));
        v.set("spent", Cell::integer(spends_ok_));
        v.set("token", Cell::integer(static_cast<std::int64_t>(pending_.opaque_token())));
#endif
#if defined(ZEN_WEAVE_SENSES)
        v.set("claimed", Cell::integer(sense_claimed_ ? 1 : 0));
        v.set("revision", Cell::integer(revision_));
        v.set("office_denied", Cell::integer(office_denied_ ? 1 : 0));
        v.set("read_hp", Cell::integer(read_hp_));
        v.set("read_author", Cell::integer(read_author_));
        v.set("read_personal", Cell::integer(read_personal_ ? 1 : 0));
        v.set("read_office", Cell::text(read_office_));
        v.set("read_life_current", Cell::integer(read_life_current_ ? 1 : 0));
        v.set("read_inc_current", Cell::integer(read_inc_current_ ? 1 : 0));
#endif
        return v;
    }

    Value policy() const override {
        Value v(lifecycle_policy_schema());
#ifdef ZEN_WEAVE_LOW_RELOADS
        v.set("max_reloads", Cell::integer(3));
#else
        v.set("max_reloads", Cell::integer(8));
#endif
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }

#ifdef ZEN_WEAVE_ASKS
    // ZEN_WEAVE_ASKS — a variant that DECLARES a capability ask, so the admission suite
    // can pin that the in-process kernel carries the declaration to the host's policy,
    // and that the policy grants nothing because of it. `ZEN_ASK` belongs to the
    // WeaveBase layer and this fixture is a raw `loom::Weave`, so the hook the exporter
    // actually looks for (`zen_requested_capabilities`, kernel/export.hpp) is written
    // out by hand. Not an override: the exporter finds it by `requires`, not by vtable.
    std::optional<loom::CapabilityAsk> zen_requested_capabilities() const {
        loom::CapabilityAsk a;
        a.network = true;
        a.filesystem = "write-scoped";
        a.roles = {"storage"};
        return a;
    }
#endif

    void revive(const Value& state) override {
#ifdef ZEN_WEAVE_CRASH_ON_REVIVE
        (void)state;
        std::abort(); // crash on revive; drives bounded reload-then-quarantine
#elif defined(ZEN_WEAVE_MEM_BOMB)
        (void)state;
        // RE-OOM ON REVIVE, so the bomb exhausts its reload budget and quarantines.
        // This site matters as much as the handle() one: quarantine is reached by
        // dying repeatedly until max_reloads runs out, so a revive that survived
        // would leave the artifact alive and the witness would never conclude.
        // Same volatile page walk, same reason — see detonate().
        (void)detonate();
        count_ = 0;
#else
        count_ = state.get("count")->as_int();
#if defined(ZEN_WEAVE_ACTIVATES)
        activations_ = state.get("activations")->as_int();
        last_activation_ = state.get("last_activation")->as_int();
#endif
#if defined(ZEN_WEAVE_DEFERS)
        // The successor inherits the NUMBER, rebuilds a capability from it, and
        // believes it holds one. It is entitled to nothing, and the board — not
        // this fixture — is what must refuse it.
        pending_ = loom::DeferredAnswer::from_host_token(
            static_cast<std::uint64_t>(state.get("token")->as_int()));
        deferred_ok_ = pending_.valid();
        spends_ok_ = 0;
#endif
#endif
    }

private:
    std::int64_t count_ = 0;
#if defined(ZEN_WEAVE_HEIR)
    bool claimed_ = false; // transient: waking asks once, and only once
#endif
#if defined(ZEN_WEAVE_ANSWERS)
    bool answered_ = false;      // did the board accept the immediate answer?
    bool second_answer_ = false; // ...and did a second one from the same delivery?
#endif
#if defined(ZEN_WEAVE_DEFERS)
    loom::DeferredAnswer pending_{}; // the retained answer right; move-only, opaque
    bool deferred_ok_ = false;       // did this delivery HAVE an answer right to retain?
    std::int64_t spends_ok_ = 0;     // how many spends the board accepted
#endif
#if defined(ZEN_WEAVE_ACTIVATES)
    std::int64_t activations_ = 0;      // how many zen.Activated this incarnation has handled
    std::int64_t last_activation_ = 0;  // the newest sequence it was told
#endif
#if defined(ZEN_WEAVE_SENSES)
    bool sense_claimed_ = false;   // did the host accept the personal claim?
    std::int64_t revision_ = 0;    // ...at what revision under its key
    bool office_denied_ = false;   // did the forged office claim refuse precisely?
    std::int64_t read_hp_ = -1;    // what the synchronous read-back saw (-1 = nothing)
    std::int64_t read_author_ = 0; // whom the reading named as author
    bool read_personal_ = false;   // did the reading carry NO office (a personal claim)?
    std::string read_office_;      // the office identity a reading carried, VERBATIM
    bool read_life_current_ = false; // was the author's life still current?
    bool read_inc_current_ = false;  // ...and its incarnation? (a separate question)
#endif
};

} // namespace

ZEN_EXPORT_WEAVE(TestWeave)
