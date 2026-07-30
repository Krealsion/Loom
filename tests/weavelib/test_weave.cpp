// A real Weave, woven as a clean C++ loom::Weave subclass and shipped as a
// .so with a single ZEN_EXPORT_WEAVE line. No senses, no std::any — the same
// Weave one would compile in. Compile-time switches produce adversarial variants
// for the kernel's harness and the isolation host's harness:
//   ZEN_WEAVE_MALFORMED_SNAPSHOT  — emit a snapshot missing a required field
//   ZEN_WEAVE_MALFORMED_MESSAGE   — emit a message missing a required field
//   ZEN_WEAVE_STATE_V2            — bump the state schema version (reload mismatch)
//   ZEN_WEAVE_CRASH_ON_MAGIC      — abort mid-handle on the magic seq 0xDEAD
//   ZEN_WEAVE_CRASH_ON_REVIVE     — abort on revive (drives reload-then-quarantine)
//   ZEN_WEAVE_LOW_RELOADS         — max_reloads = 3 (fast crash-budget exhaustion)
//   ZEN_WEAVE_SILENT              — handle never replies (liveness: cannot stall host)
//   ZEN_WEAVE_NET_PROBE           — on handle, attempt a TCP connect and report the
//                                   errno (B3: proves the sandbox blocks the network)
//   ZEN_WEAVE_FS_PROBE            — on handle, probe filesystem reach (read a secret,
//                                   write in/out of scratch, exec from scratch) and
//                                   report each errno (B4: proves the mount-ns view)
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
//                                   handled and the newest sequence (R2A-1: the
//                                   activation participant, observed through the
//                                   ordinary snapshot path)
//   ZEN_WEAVE_ACTIVATES_DRIFT     — the same weave with the SAME state schema and one
//                                   EXTRA accepted shape, so a reload between the two
//                                   differs in nothing but the door contract (R2A-1:
//                                   the accepted-schema-drift negative)
//   ZEN_WEAVE_DEFERS              — takes an ask's answer right AWAY WITH IT, returns
//                                   without answering, and answers from a LATER
//                                   handler using only the retained capability
//                                   (R2B-2: the deferring steward, proven as a real
//                                   .so because that is the whole question)
//   ZEN_WEAVE_ACTIVATES_CONFLICT  — the drift twin whose extra door carries the SAME
//                                   (name, version) with DIFFERENT content, so loading
//                                   it meets the registry's agreement wall (R2A-1a:
//                                   makes a rejected candidate's schema admission
//                                   observable from outside the kernel)

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
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

#if defined(ZEN_WEAVE_MEM_BOMB)
#include <cstring>
#endif

#if defined(ZEN_WEAVE_FORK_BOMB)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace loom;
using namespace loom;

namespace {

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
std::shared_ptr<const Schema> counter_schema() {
#if defined(ZEN_WEAVE_ACTIVATES)
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
#else
        return {ping_schema()};
#endif
    }

    void handle(const Message& in, Bus& bus) override {
#if defined(ZEN_WEAVE_DEFERS)
        // THE DYNAMIC STEWARD (R2B-2). An ask arrives; the answer is not known
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
            // R2B-1: THE FACT IS TRUSTED BECAUSE LOOM ATTESTS IT, not because the
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
#endif
        const std::int64_t seq = in.payload.get("seq")->as_int();
#ifdef ZEN_WEAVE_CRASH_ON_MAGIC
        if (seq == 0xDEAD) {
            std::abort(); // crash mid-handle; the isolation host must contain this
        }
#endif
        ++count_;
#if defined(ZEN_WEAVE_SILENT)
        (void)seq;
        (void)bus; // a deliberately silent Weave: it never replies
#elif defined(ZEN_WEAVE_MALFORMED_MESSAGE)
        (void)seq;
        bus.send(in.reply_to, Message(Value(pong_schema()))); // 'seq' deliberately absent
#elif defined(ZEN_WEAVE_NET_PROBE)
        (void)seq;
        // Instruction-level reach: open a TCP socket directly — the exact move a bus
        // grant cannot stop and only an OS sandbox can. Report the errno class so the
        // test distinguishes ENETUNREACH (no interface) from ECONNREFUSED (reachable).
        std::int64_t code = 0;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            code = errno != 0 ? errno : -1;
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(1); // a port nothing listens on → ECONNREFUSED when reachable
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            code = rc == 0 ? 0 : errno;
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
#elif defined(ZEN_WEAVE_MEM_BOMB)
        // Allocate a large resident block to trip memory.max. Held (not freed) so RSS
        // stays high; below the cgroup cap the kernel OOM-kills us mid-handle (the Pong
        // below is only reached if we survived — proving the kill is the cap).
        {
            const std::size_t bomb = 200UL * 1024 * 1024;
            char* p = static_cast<char*>(std::malloc(bomb));
            if (p != nullptr) {
                std::memset(p, 1, bomb);
            }
        }
        {
            Value pong(pong_schema());
            pong.set("seq", Cell::integer(seq));
            bus.send(in.reply_to, Message(std::move(pong)));
        }
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
#if defined(ZEN_WEAVE_DEFERS)
        v.set("deferred", Cell::integer(deferred_ok_ ? 1 : 0));
        v.set("spent", Cell::integer(spends_ok_));
        v.set("token", Cell::integer(static_cast<std::int64_t>(pending_.opaque_token())));
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
        // Re-OOM on revive so a memory bomb exhausts its reload budget and quarantines.
        const std::size_t bomb = 200UL * 1024 * 1024;
        char* p = static_cast<char*>(std::malloc(bomb));
        if (p != nullptr) {
            std::memset(p, 1, bomb);
        }
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
#if defined(ZEN_WEAVE_DEFERS)
    loom::DeferredAnswer pending_{}; // the retained answer right; move-only, opaque
    bool deferred_ok_ = false;       // did this delivery HAVE an answer right to retain?
    std::int64_t spends_ok_ = 0;     // how many spends the board accepted
#endif
#if defined(ZEN_WEAVE_ACTIVATES)
    std::int64_t activations_ = 0;      // how many zen.Activated this incarnation has handled
    std::int64_t last_activation_ = 0;  // the newest sequence it was told
#endif
};

} // namespace

ZEN_EXPORT_WEAVE(TestWeave)
