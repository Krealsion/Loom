// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A real Weave, woven as a clean C++ loom::Weave subclass and shipped as a
// .so with a single ZEN_EXPORT_WEAVE line. No senses, no std::any — the same
// Weave one would compile in. Compile-time switches produce adversarial variants
// for the kernel's harness and the isolation host's harness:
//   ZEN_WEAVE_MALFORMED_SNAPSHOT  — emit a snapshot missing a required field
//   ZEN_WEAVE_MALFORMED_MESSAGE   — emit a message missing a required field
//   ZEN_WEAVE_STATE_V2            — bump the state schema version (reload mismatch)
//   ZEN_WEAVE_CRASH_ON_MAGIC      — abort mid-handle on the magic seq 0xDEAD
//   ZEN_WEAVE_THROW_ON_MAGIC      — THROW mid-handle on the magic seq 0xDEAD. Not a
//                                   crash: the exception is caught at the library's own
//                                   ABI boundary (do_handle) and crosses back as a
//                                   status, which is the ONE path on which the host can
//                                   learn a loaded handler did not finish (RTH-1)
//   ZEN_WEAVE_CRASH_ON_REVIVE     — abort on revive (drives reload-then-quarantine)
//   ZEN_WEAVE_LOW_RELOADS         — max_reloads = 3 (fast crash-budget exhaustion)
//   ZEN_WEAVE_SILENT              — handle never replies (liveness: cannot stall host)
//   ZEN_WEAVE_NET_PROBE           — on handle, attempt a TCP connect to the loopback
//                                   port named by the Ping's seq — an endpoint the TEST
//                                   owns — push one byte down it, and report 0 or the
//                                   errno (B3: proves the sandbox blocks the network;
//                                   BL-VER-08: against a live endpoint the test
//                                   established, never a closed port whose behaviour
//                                   belongs to the host)
//   ZEN_WEAVE_FS_PROBE            — on handle, probe filesystem reach (read a secret,
//                                   write in/out of scratch, exec from scratch) and
//                                   report each errno (B4: proves the mount-ns view)
//   ZEN_WEAVE_FD_PROBE            — on handle, inventory every descriptor it actually
//                                   holds, try to USE the one named by the Ping's seq,
//                                   and separately attempt a fresh socket. Reports all
//                                   three (C-2: proves ambient host descriptors do not
//                                   cross execve, WITHOUT letting the network-namespace
//                                   denial stand in for that — they are different facts
//                                   and the second has been true here while the first
//                                   was false)
//   ZEN_WEAVE_ENV_PROBE           — on handle, report the child's COMPLETE environment:
//                                   how many entries exist, how many are LD_*, whether
//                                   the planted ambient secret is visible, and the NAMES
//                                   (never the values — a value could be a real token
//                                   from the developer's shell, and a name is enough to
//                                   identify a leak). C-2a: the environment the child
//                                   receives must be the one Zen authored, not the one
//                                   the host happened to hold
//   ZEN_WEAVE_MEM_BOMB            — on handle (and revive), allocate a large resident
//                                   block to trip memory.max (B5: OOM-kill containment)
//   ZEN_WEAVE_FORK_BOMB           — on handle, fork until it can't and report the count
//                                   (B5: proves pids.max bounds a fork-bomb)
//   ZEN_WEAVE_BEQUEATHS           — accepts zen.PrepareShutdown and answers with a
//                                   zen.Bequest carrying its live count as an item
//                                   (1b: the predecessor that writes a letter)
//   ZEN_WEAVE_HEIR                — a DIFFERENTLY-SHAPED successor (Counter v2) that
//                                   claims by role on first wake and folds what it
//                                   inherits into its own count (1b: the heir)
//   ZEN_WEAVE_ACTIVATES           — accepts zen.Activated and records, IN ITS OWN
//                                   PERSISTED STATE (Counter v3), how many it has
//                                   handled and the newest sequence (LIFE-01: the
//                                   activation participant, observed through the
//                                   ordinary snapshot path)
//   ZEN_WEAVE_ACTIVATES_DRIFT     — the same weave with the SAME state schema and one
//                                   EXTRA accepted shape, so a reload between the two
//                                   differs in nothing but the door contract (LIFE-01:
//                                   the accepted-schema-drift negative)
//   ZEN_WEAVE_ANSWERS             — answers its ask IMMEDIATELY through the public
//                                   answer surface, so the dynamic seam's meaning of
//                                   mail.answer() can be compared with the native one
//                                   (ANS-06)
//   ZEN_WEAVE_DEFERS              — takes an ask's answer right AWAY WITH IT, returns
//                                   without answering, and answers from a LATER
//                                   handler using only the retained capability
//                                   (ANS-02: the deferring steward, proven as a real
//                                   .so because that is the whole question)
//   ZEN_WEAVE_ACTIVATES_CONFLICT  — the drift twin whose extra door carries the SAME
//                                   (name, version) with DIFFERENT content, so loading
//                                   it meets the registry's agreement wall (LIFE-08:
//                                   makes a rejected candidate's schema admission
//                                   observable from outside the kernel)
//   ZEN_WEAVE_SEAM_EMIT           — on Ping, reaches for a role with a shape THIS
//                                   LIBRARY ALONE knows: SeamOnly v1 is built here and
//                                   declared in no accept-set, so nothing ever
//                                   registers it. The Night Lab III P-011 shape
//                                   exactly (the lamp's EnsureTimer to zengine.timer
//                                   when the sole registrar of that vocabulary was
//                                   never loaded). The emission cannot resolve at the
//                                   host seam; whether that leaves a Loom-owned fact
//                                   is the question the reproducer asks.

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
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

/// COMMIT ~200 MiB OF REAL, RESIDENT PAGES.
///
/// The previous shape — `malloc(bomb)` then `memset`, into a pointer never read
/// and never freed — is DEAD CODE, and at `-O2` GCC deletes the pair outright.
/// The Debug build kept it and passed; the Release build dropped it, so the
/// process never grew and was never OOM-killed.
///
/// Release did NOT report green — `isolation` and the aggregate `all` both
/// failed. What was silent was the CAUSE, not the lane: the failure surfaced
/// several steps downstream at the quarantine assertion, and nothing in it said
/// the allocation had been deleted. A witness that stops applying its pressure
/// still fails, just not where or why you would look.
///
/// Two properties make this version survive optimization, and both are needed:
///   - the pointer is `volatile`, so every store is an observable side effect
///     the compiler is forbidden to remove or sink out of the loop;
///   - there is one store PER PAGE, so the writes actually fault in the whole
///     range. A single volatile write would be equally un-removable and equally
///     useless: it commits one page, not 200 MiB.
///
/// The allocation is deliberately never freed — the pages must stay resident for
/// the cgroup to see them.
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
std::shared_ptr<const Schema> ping_schema() {
    static const auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
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
    // The COMPLETE environment, not a lookup of the names a test thought to ask about.
    // `count` is what makes an unknown future variable fail this on its own; the other
    // three are the named questions the environment policy asks, kept separate so a
// failure says which.
    // `names` carries NAMES ONLY: a value could be a real credential from the host
    // shell, and putting one in test output (or a CI log) to prove it should not be
    // there would be its own leak.
    static const auto s = SchemaBuilder("EnvResult", 1)
                              .field("count", Kind::Int)
                              .field("ld_count", Kind::Int)
                              .field("secret_present", Kind::Int)
                              .field("names", Kind::Text)
                              .build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> fdresult_schema() { // only the fd-probe variant
    // FOUR SEPARATE FACTS, deliberately not collapsed. The whole finding here was
    // that `fresh_connect == ENETUNREACH` was true while an inherited connected socket
    // was simultaneously usable — so a witness that reported only the namespace verdict
    // would have called that host contained. Each field answers its own question:
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
[[maybe_unused]] std::shared_ptr<const Schema> greet_schema() { // only the accepted-drift variants
#if defined(ZEN_WEAVE_ACTIVATES_CONFLICT)
    // The SAME (name, version) carrying DIFFERENT content — a cross-library
    // disagreement, which the registry's agreement wall refuses at load. Used to
    // make the kernel registry's admission of a REJECTED candidate's schemas
    // observable from outside.
    static const auto s = SchemaBuilder("Greet", 1).field("text", Kind::Text).build();
#else
    static const auto s = SchemaBuilder("Greet", 1).field("msg", Kind::Text).build();
#endif
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> sensehealth_schema() { // only the senses variant
    // The Sense this artifact declares it can claim. Declared in the manifest's
    // claim-set (v6), so the host registers it at load and a consumer can ask
    // what this artifact provides before it has claimed anything.
    static const auto s = SchemaBuilder("SenseHealth", 1).field("hp", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> senseprobe_schema() { // only the senses variant
    // "Observe this office and tell me EXACTLY what identity you were given."
    // The role travels as Text with no bound, because the whole point of the
    // case that uses it is a role name longer than any buffer the seam used to
    // impose — the test names the office, the artifact reports what it saw, and
    // the two are compared byte for byte.
    static const auto s = SchemaBuilder("SenseProbe", 1).field("role", Kind::Text).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> seamonly_schema() { // only the seam-emit variant
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
    // Counter v4 — the deferring steward's own bookkeeping. A loaded weave has no
    // window on itself but this one, so what the test needs to see (did I get a
    // retained answer right? did spending it succeed?) is persisted state like
    // anything else, rather than a back channel invented for a test.
    // `token` is deliberately persisted so a RELOAD hands the successor a
    // capability that LOOKS live. Without that, a successor would simply have an
    // empty member and fail locally, proving nothing about the board. With it, the
    // successor genuinely presents its predecessor's token and the board is the
    // thing that has to say no.
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
#else
        return {ping_schema()};
#endif
    }

#if defined(ZEN_WEAVE_SENSES)
    /// THE DECLARED CLAIM-SET (v6), from a raw `loom::Weave`. It rides the
    /// manifest, so the host registers SenseHealth at load and can answer "what
    /// Senses does this artifact provide?" before it has claimed anything.
    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return {sensehealth_schema()};
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
        // THE DYNAMIC STEWARD (ANS-02, ANS-06). An ask arrives; the answer is not known
        // yet; so it takes the answer right away with it and RETURNS WITHOUT
        // ANSWERING. Nothing here retains a Bus or a Message — only the opaque
        // capability, which is the whole point: a stored `Bus&` would be a
        // dangling reference, and a stored Message would be an ordinary value
        // with no authority.
        // `count_` counts DELIVERIES, both shapes, so a test can positively
        // observe that a handler ran — which matters most for the assertions that
        // are absences ("it got nothing", "it answered nobody"): without it, a
        // weave that never woke up would pass them all.
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
            // LIFE-04: THE FACT IS TRUSTED BECAUSE LOOM ATTESTS IT, not because the
            // shape arrived. Two questions, and both must answer yes:
            //   - did Loom authorize a lifecycle commit for THIS incarnation?
            //     (bound to the target by the bus; an ordinary weave sending the
            //      same public shape produces nothing here)
            //   - is the attested sequence the one the payload claims? (a proof
            //     minted for one activation must not authenticate another)
            // Anything else is an ordinary message wearing a lifecycle costume,
            // and is ignored entirely — no count, no lineage, no notice.
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
        // THE DYNAMIC-PARITY FIXTURE (SENSE-01; ABI v6). The same four public verbs a
        // native weave writes, from the far side of the seam — and the whole
        // question is whether they mean here what they mean natively.
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
        // THE SILENT-SEAM FIXTURE (MSG-08). Reach for a
        // service by role, carrying a shape no registry in this process knows.
        // Both halves matter: the role may well be unheld, but the emission never
        // gets far enough for that to be the reason — it is rejected at the
        // library/host seam, before any target is resolved. Fire-and-forget by
        // design (a dynamic send returns no ticket), so this weave learns nothing
        // and asserts nothing; the test watches the HOST side.
        Value want(seamonly_schema());
        want.set("want", Cell::integer(seq));
        bus.send_to_role("nobody.home", Message(std::move(want)));
#elif defined(ZEN_WEAVE_SILENT)
        (void)seq;
        (void)bus; // a deliberately silent Weave: it never replies
#elif defined(ZEN_WEAVE_MALFORMED_MESSAGE)
        (void)seq;
        bus.send(in.reply_to, Message(Value(pong_schema()))); // 'seq' deliberately absent
#elif defined(ZEN_WEAVE_NET_PROBE)
        // Instruction-level reach: open a TCP socket directly and USE it — the exact
        // move a bus grant cannot stop and only an OS sandbox can.
        //
        // The Ping's `seq` names the TCP port of an endpoint THE TEST OWNS on loopback
        // (bound to :0, so the kernel chose it). It used to be the literal port 1, on
        // the reasoning that "nothing listens there, so a reachable stack answers
        // ECONNREFUSED" — which made the positive control depend on how the HOST treats
        // a closed port. Where a closed port is black-holed instead of refused (WSL2
        // mirrored networking), that connect() burns the whole SYN retry budget and the
        // probe answers minutes late. So the endpoint is now a live listener the test
        // established, and the positive witness is success rather than a particular
        // failure (BL-VER-08; the measurement is in BL-VER-07-RB).
        //
        // `code` is 0 only if the connection opened AND one byte went down it, so a
        // handshake that cannot carry data is not reported as reach. Otherwise it is
        // the errno the OS gave — ENETUNREACH when the sandbox removed the interface.
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
        // THE DESCRIPTOR WITNESS, from inside the sandbox. Instruction-level reach:
        // no bus grant can stop any of this, and no namespace covers it either — an
        // already-open descriptor that crossed execve is simply THERE, or it is not.
        //
        // The Ping's `seq` names the fd the host parked before spawning. Writing the
        // payload to it is the exact escape this witness exists to catch; the host holds
        // the other end and asserts that nothing arrives.
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
        // Commit a large resident block to trip memory.max. Held (not freed) so RSS
        // stays high; under the cgroup cap the kernel OOM-kills us mid-handle, and
        // THE REPLY BELOW IS NEVER SENT. That silence is the witness: the test
        // asserts the recorder heard nothing, so a bomb that fails to die is caught
        // by a Pong arriving rather than by a missing one.
        //
        // When it does NOT die, the reply carries WHY as a sentinel seq instead of
        // the echo, so "the kernel refused the allocation" and "the pages were all
        // written and nothing killed us" stay distinguishable in the failure.
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
