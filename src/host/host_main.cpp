// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// loom-host — THE SUPPLIED HOST.
//
// Install Loom and you get a program. That sentence is the whole point of this file:
// before it, every host in this tree was a fixed-cast demo in the build directory, and
// the only route to a running Loom was to write C++ first.
//
// WHAT IT IS. A boot walk the person authors, an operator console, and the two files
// that carry their decisions across restarts:
//
//   the boot plan       what to start and in what order          host/boot_plan.hpp
//   the authority store what may run, and what it may say        host/authority.hpp
//
// WHAT IT IS NOT. It is not a framework and it is not privileged. Everything it mounts
// is an ordinary participant: the console is `loom::ConsoleEngine` (a deliberately
// trusted host lens, and labelled as one), the lifecycle steward is
// `loom::WeaveManager`, the kernel door is `loom::ControlWeave`, and the authority hand
// is `loom::host::HostWarden`. Every one of them is replaceable by something a person
// writes, because none of them is a kernel primitive — being the default that ships is
// not the same as being the only thing that fits.
//
// ---- THE THREE RULES THIS HOST IS BUILT AROUND ------------------------------
//
// They are written here because each one replaced a defect that a green test lane could
// not see, and each one is easy to lose again by writing the obvious thing.
//
// 1. AN ANSWER IS SOMETHING LOOM ATTRIBUTED, NEVER THE NEWEST THING IN THE WINDOW.
//    Every operator-initiated conversation goes through `HostSession::ask`, which opens
//    it in the console's own `loom::AskBook` and settles it only on the pair
//    (correlation this console minted, bus-stamped sender). The console is registered
//    `AcceptMode::AnyRegistered` and every admitted artifact may send `zen.Result` under
//    the ordinary poke-answer baseline, so "the newest buffer entry" is a value any
//    loaded weave can author. It once decided which weave this host administered.
//
// 2. A LOADED ARTIFACT IS ADOPTED INSIDE THE LOAD, NOT AFTER IT. `LifecycleAdoption`
//    (zen/kernel/control.hpp) hands this host the one moment between "the incarnation is
//    committed" and "the incarnation has been told it is live". Everything after that is
//    queue order and the asker hears last, so a host that adopted on the answer installed
//    the person's approved authority strictly after the weave's first breath. Because the
//    door owns that moment, EVERY route through it — the boot walk, `start`, and an
//    ordinary `zen.LoadWeave` a weave sends to the Manager itself — produces the same
//    governed participant. There is no adoption anywhere else in this file.
//
// 3. THE BUS AND THE PERSON BOTH GET SERVED, EVERY TURN. The loop is
//    `pump_pending()` (bounded at the backlog it found — MSG-09) plus a line read with a
//    deadline (host/line_input.hpp). It is never `drain_until_idle()`, which is unbounded
//    BY CONTRACT: one approved self-addressed message from a loaded weave used to make
//    `stop` and `quit` unreadable forever. A conversation that has not settled is
//    reported as PENDING and stays open; the host never invents a completion to get its
//    prompt back.
//
// SHUTDOWN HAS TWO MEANINGS AND THEY ARE DIFFERENT COMMANDS.
//   `stop <name>`  ends one APPLICATION: its weave leaves the bus, its library closes,
//                  the host keeps running and the console stays up.
//   `quit`         ends the HOST: the console loop returns and everything comes down in
//                  reverse construction order.
// A boot failure is neither. It leaves the console up, which is the only state from
// which a person can find out what went wrong and fix it.

#include "authority.hpp"
#include "boot_plan.hpp"
#include "line_input.hpp"
#include "store_lock.hpp"
#include "warden.hpp"

#include <zen/console/console.hpp>
#include <zen/host/grant_wiring.hpp>
#include <zen/kernel/abi.h> // ZEN_ABI_VERSION — text, present on every platform
#include <zen/switchboard.hpp>
#include <zen/terminal/input_lex.hpp>
#include <zen/weave/poke.hpp>
#include <zen/zen.hpp>

#if ZEN_HOST_HAS_KERNEL
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/kernel/manager.hpp>
#endif

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using loom::lex_arg;
using loom::parse_u64;
using loom::Token;
using loom::tokenize;

constexpr const char* kVersion = "loom-host 0.1.0";

/// HOW LONG THE HOST WAITS BEFORE CALLING A CONVERSATION PENDING, in bounded turns.
///
/// It is a number of TURNS, not a clock and not a message budget: each turn is one
/// `pump_pending()`, which services exactly the backlog it found, so a weave that
/// re-arms itself inside a turn cannot extend it. A load conversation is four turns
/// (ask -> Manager -> door -> activation + answer -> relay), so sixteen is roomy for
/// everything this host composes and still finite for everything it does not.
///
/// EXCEEDING IT IS NOT A FAILURE AND NOT A TIMEOUT. Nothing is cancelled, nothing at the
/// far end is told anything, and the conversation stays open in the console's book: the
/// command says PENDING, the loop keeps turning, and the answer prints when it arrives.
constexpr int kSettleTurns = 16;

/// How long an idle host waits on the person before turning the bus again. Only reached
/// when the bus has nothing to do, so it costs a busy host nothing; short enough that a
/// weave woken by something outside this loop is still serviced promptly.
constexpr int kIdleWaitMs = 100;

// ---- launch parameters ------------------------------------------------------
//
// Small on purpose. Everything that selects BEHAVIOUR is in the two files; the command
// line selects WHICH files, plus the two things a person needs when a file is the
// problem (`--no-boot`) or when they want to know what the host read without running it
// (`--check`).

struct Options {
    std::string boot_plan = "loom-boot.json";
    std::string authority = "loom-authority.json";
    bool no_boot = false;
    bool check_only = false;
    bool ok = true;
    bool done = false; // --help / --version: print and exit 0
};

void print_usage() {
    std::cout <<
        R"(loom-host — the supplied Loom host: boot what you chose, then operate it.

usage: loom-host [options]

  --boot <file>       the boot plan: what to start, in the order you wrote it.
                      default: ./loom-boot.json   (absent = start nothing)
  --authority <file>  your standing decisions: what may run, and what it may say.
                      default: ./loom-authority.json  (absent = nothing may run)
                      One host owns one of these at a time; a second host naming the
                      same file is refused rather than allowed to overwrite it.
  --no-boot           come up with the console only, starting nothing. The way back in
                      when a boot plan is what is broken — including when it does not
                      parse, which is reported and NOT run.
  --check             read and validate both files, print what they say, and exit
                      without starting anything or opening a console. A file that does
                      not parse is an error here, which is what --check is for.
  --help, --version

exit codes: 0 ok   2 bad command line   3 a file would not parse   4 another host owns
the decision store

The two files are yours: written here at the console, readable and editable in any
editor. Neither is required to start — a host with no files comes up empty and says so,
which is where everyone starts.
)";
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "loom-host: " << a << " needs a " << what << '\n';
                o.ok = false;
                return {};
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            print_usage();
            o.done = true;
            return o;
        } else if (a == "--version") {
            std::cout << kVersion << "  (weave ABI v" << ZEN_ABI_VERSION << ")\n";
            o.done = true;
            return o;
        } else if (a == "--boot") {
            o.boot_plan = value("file");
        } else if (a == "--authority") {
            o.authority = value("file");
        } else if (a == "--no-boot") {
            o.no_boot = true;
        } else if (a == "--check") {
            o.check_only = true;
        } else {
            std::cerr << "loom-host: unknown option '" << a << "' (try --help)\n";
            o.ok = false;
        }
    }
    return o;
}

// ---- the session ------------------------------------------------------------

/// WHAT AN OPERATOR-INITIATED CONVERSATION CAME TO.
///
/// `Pending` is a real outcome and the reason this is not an `optional<BufferEntry>`: a
/// weave is entitled to take longer than this host's patience, and the honest report is
/// "still waiting, ask number N" — not a fabricated completion and not a lost question.
struct Answer {
    enum class State {
        Settled,    ///< an arrival Loom attributed to exactly this conversation
        Pending,    ///< no attributed answer yet; the conversation is still open
        Refused,    ///< the SEND was refused by the gate; nobody was ever asked
        Unsendable, ///< it could not even be composed (a wrong shape name, say)
        /// It WAS sent, and this console has no room left to track its answer. A distinct
        /// state because "it never went" and "it went and cannot be attributed" are
        /// different facts, and only one of them is fixed by typing the command again.
        Untracked,
    };
    State state = State::Unsendable;
    std::uint64_t ask = 0;
    std::optional<loom::BufferEntry> reply;
    std::string trouble; ///< why, for every state but Settled

    bool settled() const { return state == State::Settled && reply.has_value(); }
};

/// Everything the host holds, in construction order — which is also the order the
/// dependencies run, and the reverse of the order it comes down. The Kernel must die
/// before the Switchboard (its artifacts are participants) and the store must outlive
/// the Kernel (the admission policy consults it), so the declaration order below is
/// load-bearing and not alphabetical.
class HostSession {
public:
    HostSession(loom::host::AuthorityStore& store, const Options& opt)
        : store_(store), opt_(opt) {
        // The console first: the warden needs its id as the operator seat, and the
        // seat has to exist before anything that obeys it.
        //
        // AND IT DECLARES ITS VOCABULARY, which is not a formality. A console's
        // wildcard accept means "any shape the REGISTRY can resolve", and the registry
        // learns a shape from some weave's ACCEPT-set — so a shape that only ever
        // travels TO the operator has nobody to declare it and is refused
        // `SeamUnresolved` before it reaches any door. `zen.AuthorityDescription` is
        // exactly such a shape here: the warden emits it, nothing accepts it, and
        // without this line `authority show` silently got no answer. Found by using it.
        console_ = std::make_unique<loom::ConsoleEngine>(
            bus_, std::vector<std::shared_ptr<const loom::Schema>>{
                      loom::schema_of<loom::AuthorityDescription>()});
        auto warden = std::make_unique<loom::host::HostWarden>(store_, console_->console_id());
        warden_ = warden.get();
        warden_id_ = bus_.register_weave(std::move(warden),
                                         loom::host::warden_capability(console_->console_id()));
        warden_->zen_set_self(warden_id_);
#if ZEN_HOST_HAS_KERNEL
        kernel_ = std::make_unique<loom::Kernel>(bus_, store_.policy());
        // THE ADOPTION SEAM, AND THE WHOLE OF THIS HOST'S LIFECYCLE OWNERSHIP.
        //
        // The door calls these from inside its own delivery — the one window between an
        // incarnation being committed and being told it is live (see `LifecycleAdoption`
        // and rule 2 in the file header). Every route that can load or unload anything in
        // this process goes through that door, so putting the host's bookkeeping here is
        // what makes the boot walk, the `start` command and a `zen.LoadWeave` a weave
        // sends of its own accord produce the SAME governed participant. Nothing else in
        // this file governs, releases, or mints a `GrantAuthority`.
        loom::LifecycleAdoption adoption;
        adoption.admitted = [this](loom::Mail& mail, const std::string& name, loom::WeaveId id) {
            // THE CEILING IS FULL, AND THAT IS NOT A CONTROL BEING SKIPPED. A ceiling
            // bounds a DELEGATE — it is how a host limits a Weaver acting on somebody
            // else's behalf. The delegate here is the host's own warden, obeying the
            // person at this console directly, so the thing that actually bounds what
            // gets installed is the file the person wrote. Naming a narrower ceiling
            // would look like a second control while being nothing but a cap on what the
            // person could later approve without a restart.
            //
            // `id` is the KERNEL'S fact about what it just registered. It is never a
            // message payload, which is the distinction that failed before: a host that
            // read the subject off a `zen.Result` administered whichever weave authored
            // the newest one.
            const loom::host::Installed done = warden_->adopt(
                mail, name,
                loom::host_grant_authority(
                    bus_, id, loom::LiveAuthority{}.allow_any().allow_observe_any()));
            adoption_log_.push_back((done.ok ? "  " : "  could not administer '" + name + "': ") +
                                    done.report);
        };
        adoption.retired = [this](loom::Mail&, const std::string& name) {
            // Its authority went with it. Dropping the capability is not a revocation —
            // there is no longer a subject to revoke anything from — and the person's
            // standing decision in the file is untouched, so starting it again restores
            // exactly what they approved.
            warden_->release(name);
        };
        control_ = loom::mount_control(*kernel_, bus_, std::move(adoption));
        manager_ = loom::mount_manager(control_, bus_);
#endif
    }

    ~HostSession() {
#if ZEN_HOST_HAS_KERNEL
        kernel_.reset(); // artifacts leave the bus before the bus does
#endif
    }

    loom::ConsoleEngine& console() { return *console_; }
    loom::Switchboard& bus() { return bus_; }
    const loom::host::BootReport& report() const { return report_; }

    bool can_host_weaves() const {
#if ZEN_HOST_HAS_KERNEL
        return true;
#else
        return false;
#endif
    }

    /// Why not, in a sentence a person can act on. The Windows default build carries no
    /// kernel because the Windows backend has no isolation and is therefore an explicit
    /// opt-in (CMakeLists.txt, `LOOM_ENABLE_WINDOWS_KERNEL`). Saying "cannot load
    /// weaves" without saying that would send a person looking for a bug.
    static const char* no_kernel_note() {
        return "this build has no weave kernel, so it can host nothing loadable. On Windows the "
               "kernel backend is an opt-in because it applies no sandbox: rebuild Loom with "
               "-DLOOM_ENABLE_WINDOWS_KERNEL=ON to host weaves in-process.";
    }

    /// The containment this host actually imposes, in the Kernel's own words — never a
    /// stronger sentence written here.
    static const char* containment_note() {
#if ZEN_HOST_HAS_KERNEL
        return loom::Kernel::containment_note();
#else
        return "no kernel in this build; nothing loadable is hosted";
#endif
    }

    // ---- turning the world ---------------------------------------------------

    /// ONE BOUNDED HOST TURN. `pump_pending` services exactly the backlog it found and
    /// hands control back, so work a handler queues during the turn waits for the next
    /// one — which is precisely what a self-re-arming producer cannot get around, and
    /// why this host no longer has a call that can fail to return.
    std::size_t turn() { return bus_.pump_pending(); }

    bool busy() const { return bus_.pending() != 0; }

    /// Turn the bus until `ask` settles or `turns` turns have been spent. Returns
    /// whether it settled; NOT settling is an ordinary outcome (see `Answer`).
    bool service(std::uint64_t ask, int turns) {
        for (int i = 0; i < turns; ++i) {
            if (ask != 0 && console_->settled(ask)) {
                return true;
            }
            if (bus_.pump_pending() == 0 && !bus_.pending()) {
                break; // nothing left to do; waiting longer would change nothing
            }
        }
        return ask != 0 && console_->settled(ask).has_value();
    }

    /// COMPOSE A MESSAGE FROM THE OPERATOR SEAT, DELIVER IT, AND SETTLE ITS ANSWER.
    ///
    /// One helper, so every operator-initiated conversation in this file is the same
    /// public path: the console's own weave speaks, as itself, under its own grant, and
    /// what comes back is an arrival Loom attributed to THIS conversation — the
    /// correlation this console minted plus the bus's own stamp of who spoke. Nothing
    /// here reads the newest thing in a window.
    Answer ask(loom::WeaveId target, const char* shape, std::uint32_t version,
               const std::map<std::string, loom::FieldValue>& fields, int turns = kSettleTurns) {
        Answer a;
        std::string error;
        // TRACKED, AND THEREFORE OWED: this host holds a slot for every conversation it opens
        // and returns it by taking the answer here, by taking it in `report_late_answers`
        // when it lands later, or by forgetting it at the person's word.
        const loom::Submitted sent = console_->submit(target, shape, version, fields,
                                                      loom::ConsoleTracking::Tracked, &error);
        if (!error.empty()) {
            // A shape this console could not even compose and a weave that chose not to
            // answer looked identical once; they are completely different problems.
            a.state = Answer::State::Unsendable;
            a.trouble = std::string("could not compose ") + shape + ": " + error;
            return a;
        }
        a.ask = sent.ask;
        if (sent.ask == 0) {
            // THE MESSAGE WENT. What was refused is the bookkeeping, so this says that
            // rather than implying the send failed — a caller that retried would send a
            // second copy of something that already arrived.
            a.state = Answer::State::Untracked;
            a.trouble = "sent, but this console is already holding " +
                        std::to_string(console_->asks_held()) +
                        " conversations, which is all it tracks — so its answer cannot be "
                        "attributed. 'asks' lists them; 'asks forget <n>' makes room.";
            return a;
        }
        service(sent.ask, turns);
        const loom::SendOutcome o = console_->outcome(sent.ticket);
        if (auto reply = console_->take_settled(sent.ask)) {
            // COLLECTED, which returns the slot. A late copy of the same answer is inert: the
            // conversation closed when it settled.
            a.state = Answer::State::Settled;
            a.reply = std::move(reply);
            return a;
        }
        if (o.refused) {
            // REFUSED IS NOT PENDING. The gate answered; nobody was ever asked, so the
            // conversation is closed here rather than left open forever.
            (void)console_->forget_ask(sent.ask);
            a.state = Answer::State::Refused;
            a.trouble = "the send was refused: " + o.reason;
            return a;
        }
        a.state = Answer::State::Pending;
        a.trouble = "no attributed answer yet (ask " + std::to_string(sent.ask) +
                    "); this host keeps turning and will print it when it lands";
        return a;
    }

    /// Make the bus agree with the store for one artifact — the only route by which
    /// delegated authority changes at a person's request, and it is a message, so it
    /// shows on the tap.
    std::string sync(const std::string& artifact) {
        if (!warden_->governs(artifact)) {
            return "nothing loaded under '" + artifact + "' to administer";
        }
        const Answer a = ask(warden_id_, loom::host::HostAuthoritySync::zen_name,
                             loom::host::HostAuthoritySync::zen_version, {{"artifact", artifact}});
        return a.settled() ? describe_reply(*a.reply) : a.trouble;
    }

    /// The standard reply shapes, as one line. A `zen.Refused` is printed as a refusal
    /// and never flattened into "failed": the reason is the whole value of it.
    static std::string describe_reply(const loom::BufferEntry& e) {
        const loom::Cell* value = e.value.get("value");
        const loom::Cell* reason = e.value.get("reason");
        if (e.name == loom::Refused::zen_name && reason != nullptr) {
            return "refused: " + reason->as_text();
        }
        if (value != nullptr && value->kind() == loom::Kind::Text) {
            return value->as_text();
        }
        // A `zen.Ack` carries nothing and that IS the answer. Repeating its own name
        // back ("zen.Ack v1  zen.Ack v1", which is what this used to print in the
        // buffer) says less than one plain word.
        if (e.name == loom::Ack::zen_name) {
            return "ok";
        }
        return e.name + " v" + std::to_string(e.version);
    }

    // ---- the boot walk -------------------------------------------------------

    /// Walk the plan in the person's order. Never throws a row away and never stops the
    /// host: a row that fails is a row with a state and a reason, and the console comes
    /// up either way.
    ///
    /// EVERY ROW GOES OUT AS `zen.LoadWeave` THROUGH THE STEWARD, exactly as `start`
    /// does. It used to call `Kernel::load` directly, which is a different door: no
    /// authenticated `zen.Activated` was announced, so a participating weave was
    /// started and never told it was live (a probe reported `activations=0` after a boot
    /// this host called COMPLETE), and adoption had to be repeated in a second place.
    /// One door, one set of refusals, one set of words, one adoption.
    void boot(const loom::host::BootPlan& plan) {
        bool walking = !opt_.no_boot;
        for (const loom::host::BootEntry& e : plan.entries) {
            loom::host::BootOutcome row;
            row.entry = e;
            if (!walking) {
                row.state = opt_.no_boot ? loom::host::BootState::Skipped
                                         : loom::host::BootState::Pending;
                row.detail = opt_.no_boot ? "--no-boot" : "an earlier row stopped the walk";
                report_.rows.push_back(std::move(row));
                continue;
            }
            if (!e.enabled) {
                row.state = loom::host::BootState::Skipped;
                row.detail = "disabled in the boot plan";
                report_.rows.push_back(std::move(row));
                continue;
            }
            start_row(e, &row);
            const bool bad = row.state != loom::host::BootState::Started;
            report_.rows.push_back(std::move(row));
            if (bad && e.on_failure == loom::host::OnFailure::Stop) {
                walking = false;
            }
        }
        // One more bounded turn so an activation a row's weave answered has been
        // delivered before the console prints the report. Bounded, like everything else.
        (void)service(0, kSettleTurns);
    }

    /// One boot row, through the steward, with its outcome attributed.
    void start_row(const loom::host::BootEntry& e, loom::host::BootOutcome* row) {
#if ZEN_HOST_HAS_KERNEL
        const Answer a = ask(manager_, loom::LoadWeave::zen_name, loom::LoadWeave::zen_version,
                             {{"name", e.name}, {"path", e.path}, {"role", e.role}});
        if (a.state == Answer::State::Pending) {
            // A ROW MAY LEGITIMATELY NOT BE DONE YET, and saying so beats both waiting
            // forever and calling it started. The conversation stays open; the console
            // prints its answer when it lands.
            row->state = loom::host::BootState::Pending;
            row->detail = a.trouble;
            return;
        }
        if (!a.settled()) {
            row->state = loom::host::BootState::Failed;
            row->detail = a.trouble;
            return;
        }
        if (a.reply->name == loom::Refused::zen_name) {
            // "refused" and "failed" send a person to different places — the console or
            // the filesystem — so the report distinguishes them by who said no. The raw
            // reason, not `describe_reply`: the row already says `refused` in its state
            // column, and printing "refused: admission refused at open:" says it twice.
            const loom::Cell* reason = a.reply->value.get("reason");
            const std::string why =
                reason != nullptr ? reason->as_text() : describe_reply(*a.reply);
            row->state = why.find("admission refused") != std::string::npos
                             ? loom::host::BootState::Refused
                             : loom::host::BootState::Failed;
            row->detail = why;
            return;
        }
        // THE SUBJECT IS THE WARDEN'S FACT, not the answer's payload. The two agree
        // here, and the host says so if they ever do not (see `governed_note`).
        const loom::WeaveId id = warden_->subject_of(e.name);
        if (!id.valid()) {
            row->state = loom::host::BootState::Failed;
            row->detail = "loaded, but this host is not administering it: " + describe_reply(*a.reply);
            return;
        }
        row->state = loom::host::BootState::Started;
        row->weave = id.value;
#else
        (void)e;
        row->state = loom::host::BootState::Failed;
        row->detail = no_kernel_note();
#endif
    }

    /// What the adoption seam did since this was last asked, and it is drained by the
    /// asking so a later command does not reprint an earlier load's authority.
    std::vector<std::string> take_adoption_log() {
        std::vector<std::string> out;
        out.swap(adoption_log_);
        return out;
    }

    /// CROSS-CHECK: does the id a lifecycle answer reported match the subject this host
    /// is actually administering under that name? They are two independent facts — one
    /// came over the bus as a payload, one is the capability this host minted from the
    /// Kernel's own answer — and the whole of finding 1 was a host that had only the
    /// first. An empty string means they agree.
    std::string governed_note(const std::string& artifact, const loom::BufferEntry& reply) const {
        const loom::WeaveId governed = warden_->subject_of(artifact);
        const loom::Cell* value = reply.value.get("value");
        if (!governed.valid() || value == nullptr || value->kind() != loom::Kind::Text) {
            return {};
        }
        std::uint64_t reported = 0;
        if (!parse_u64(value->as_text(), reported) || reported == governed.value) {
            return {};
        }
        return "  WARNING: the steward reported weave " + std::to_string(reported) +
               " but this host administers weave " + std::to_string(governed.value) +
               " under '" + artifact + "'. Report this.";
    }

#if ZEN_HOST_HAS_KERNEL
    loom::WeaveId manager() const { return manager_; }
    /// The kernel's control door. The console reaches it DIRECTLY for the primitives
    /// the steward does not compose (unload), which is not a bypass: the door is gated
    /// by the load capability, the console holds it like any other participant could,
    /// and the steward's own note says a holder may drive the door with no Manager in
    /// the path.
    loom::WeaveId control() const { return control_; }
    loom::Kernel& kernel() { return *kernel_; }
#endif
    loom::host::HostWarden& warden() { return *warden_; }
    loom::WeaveId warden_id() const { return warden_id_; }
    loom::WeaveId seat() const { return console_->console_id(); }

private:
    loom::host::AuthorityStore& store_;
    const Options& opt_;
    loom::Switchboard bus_;
    std::unique_ptr<loom::ConsoleEngine> console_;
    loom::host::HostWarden* warden_ = nullptr; ///< owned by the bus; non-owning here
    loom::WeaveId warden_id_{};
    /// What the adoption seam installed, in the order it installed it. Written from
    /// inside the door's delivery, read by whichever command caused the load.
    std::vector<std::string> adoption_log_;
#if ZEN_HOST_HAS_KERNEL
    std::unique_ptr<loom::Kernel> kernel_;
    loom::WeaveId control_{};
    loom::WeaveId manager_{};
#endif
    loom::host::BootReport report_;
};

// ---- the console commands ---------------------------------------------------

void print_help() {
    std::cout <<
        R"(  status                        the boot report, what is loaded, what this host contains
  weaves                        who is on the bus, and what each accepts
  describe <Shape> <v>          a shape's fields
  send <id> <Shape> <v> [args]  compose and send; args: value | field=value | $mN.field
  buffer | show <mN>            replies, by their stable labels
  asks | asks forget <n>        conversations still open; stop waiting on one
  tap [n]                       the last n bus events, refusals included

  start <name> <path> [role]    load an artifact (through the steward — policy decides)
  reload <name> <path>          reload it in place, same id, state carried
  stop <name>                   unload it: that application ends, this host does not
  list                          what the steward says is loaded, and under which offices

  authority                     your standing decisions
  authority show <name>         one decision, and what the bus actually permits now
  authority pending             what the policy refused and is waiting on
  authority trust <name> [--rebuilds|--ask-again]
                                it may run, pinned to this build; --rebuilds accepts any
                                rebuild of it, --ask-again turns that back off
  authority deny <name>         it may not run
  authority allow <name> <rule> it may say one more thing, e.g.  Greet v1 -> any target
  authority revoke <name> [rule]   take one back, or all of them; live and remembered
  authority forget <name>       drop the decision entirely

  quit                          shut this host down
)";
}

/// What the policy did that nobody asked about. Printed after every boot, from `status`,
/// and as it happens (`print_new_notes`), because an admission the person did not have to
/// answer is still something they are entitled to know happened — above all that an
/// artifact came up on REBUILT code under authority they granted for an earlier build.
void print_notes(const loom::host::AuthorityStore& store) {
    for (const std::string& n : store.notes()) {
        std::cout << "  note: " << n << '\n';
    }
}

/// ...AND SAID WHEN IT HAPPENS, not only when somebody thinks to type `status`. A `start`
/// that came up on rebuilt code, or whose build could not be recorded, used to print only
/// the steward's answer; the note waited for a `status` the person had no reason to ask
/// for. `*shown` is how many notes this console has already printed.
void print_new_notes(const loom::host::AuthorityStore& store, std::size_t* shown) {
    const std::vector<std::string>& all = store.notes();
    for (std::size_t i = *shown; i < all.size(); ++i) {
        std::cout << "  note: " << all[i] << '\n';
    }
    *shown = all.size();
}

/// A LINE REFUSED FOR ITS LENGTH, said at the point in the input where it stood. None of it
/// ran: not a shortened command, and not its remainder as a second one (host/line_input.hpp).
/// The next line is read as usual, so the way back is to send the command again, shorter —
/// and the line number and its first bytes are how a person or a script's author finds it.
void print_too_long(const loom::host::TooLongLine& refused) {
    std::string shown;
    for (const char c : refused.beginning) {
        const auto u = static_cast<unsigned char>(c);
        shown.push_back(u >= 0x20 && u < 0x7f ? c : '?');
    }
    std::cout << "  refused: input line " << refused.line << " is longer than "
              << loom::host::kMaxCommandBytes
              << " bytes, the longest command this host reads, so none of it was run. It began: "
              << shown << "...\n"
              << "  send it again, shorter; the lines after it are read as usual.\n";
}

/// The boot plan's own state, for `status`. A plan that did not parse is not the same
/// thing as a plan with no rows, and `--no-boot` now comes up over the first one.
struct PlanSource {
    std::string path;
    std::string parse_error; ///< empty when the plan was read
};

void cmd_status(HostSession& s, const loom::host::BootPlan& plan, const PlanSource& source,
                const loom::host::AuthorityStore& store, const loom::host::StoreLock& lock) {
    std::cout << "  " << kVersion << "   weave ABI v" << ZEN_ABI_VERSION << '\n';
    std::cout << "  containment: " << HostSession::containment_note() << '\n';
    if (source.parse_error.empty()) {
        std::cout << "  boot plan:   " << (plan.source.empty() ? "(none)" : plan.source) << '\n';
    } else {
        std::cout << "  boot plan:   " << source.path << "  COULD NOT BE READ, NOT RUN\n";
        std::cout << "               " << source.parse_error << '\n';
    }
    std::cout << "  authority:   " << (store.path().empty() ? "(none)" : store.path())
              << (lock.held() ? "   (owned by this host)" : "   (in memory only)") << '\n';
    if (!s.can_host_weaves()) {
        std::cout << "  NOTE: " << HostSession::no_kernel_note() << '\n';
    }
    std::cout << "  boot:\n";
    if (source.parse_error.empty()) {
        for (const std::string& line : s.report().render()) {
            std::cout << line << '\n';
        }
    } else {
        // NOT "nothing requested". The person requested plenty; the file could not be
        // read, which is a different sentence and sends them somewhere else.
        std::cout << "    the plan was not read, so no row was attempted\n";
    }
    print_notes(store);
    if (!store.pending().empty()) {
        std::cout << "  " << store.pending().size()
                  << " decision(s) waiting on you — 'authority pending'\n";
    }
    const std::vector<loom::PendingAsk> open = s.console().open_asks();
    if (!open.empty()) {
        std::cout << "  " << open.size() << " conversation(s) still open — 'asks'\n";
    }
}

/// Is `text` still permitted by what `rule` leaves behind? Asked of the AUTHORITY, not
/// of the list of strings, so `revoke Spin v1 -> role probe` under a standing
/// `any shape -> any target` says the honest thing instead of implying the permission is
/// gone. The two lists are different vocabularies for the same wall.
bool still_permitted(const loom::host::AuthorityRule& rule, const std::string& text) {
    loom::LiveAuthority remaining;
    loom::LiveAuthority revoked;
    std::string ignored;
    if (!loom::host::apply_rule(text, &revoked, &ignored)) {
        return false;
    }
    if (!loom::host::to_live_authority(rule, &remaining, &ignored)) {
        return false;
    }
    return remaining.contains(revoked);
}

void cmd_authority(HostSession& s, loom::host::AuthorityStore& store,
                   const std::vector<Token>& tok) {
    const std::string sub = tok.size() > 1 ? tok[1].text : "";

    if (sub.empty()) {
        const std::vector<loom::host::AuthorityRule> all = store.rules();
        if (all.empty()) {
            std::cout << "  no standing decisions yet. Nothing may run.\n"
                         "  approve something with:  authority trust <name>\n";
            return;
        }
        for (const loom::host::AuthorityRule& r : all) {
            std::cout << "  " << (r.may_run ? "run " : "DENY") << "  " << r.artifact;
            if (!r.content_id.empty()) {
                std::cout << "  build " << r.content_id.substr(0, 8);
            }
            if (r.trust_rebuilds) {
                std::cout << "  (+rebuilds)";
            }
            std::cout << "  " << r.send.size() << " send, " << r.observe.size() << " observe\n";
        }
        return;
    }

    if (sub == "pending") {
        if (store.pending().empty()) {
            std::cout << "  nothing is waiting on you.\n";
            return;
        }
        for (const loom::host::PendingDecision& p : store.pending()) {
            std::cout << "  " << p.artifact << "  " << p.path << '\n';
            std::cout << "    " << p.why << '\n';
            std::cout << "    build presented: " << (p.content_id.empty() ? "(unreadable)"
                                                                          : p.content_id)
                      << '\n';
            if (!p.pinned.empty()) {
                std::cout << "    build approved:  " << p.pinned << '\n';
            }
        }
        return;
    }

    if (tok.size() < 3) {
        std::cout << "  usage: authority show|trust|deny|allow|revoke|forget <name> [...]\n";
        return;
    }
    const std::string name = tok[2].text;
    const loom::host::AuthorityRule* existing = store.find(name);
    std::string error;

    if (sub == "show") {
        if (existing == nullptr) {
            std::cout << "  no standing decision for '" << name << "'\n";
        } else {
            std::cout << "  " << name << (existing->may_run ? ": may run" : ": may NOT run")
                      << '\n';
            std::cout << "    build pinned: "
                      << (existing->content_id.empty() ? "(not yet)" : existing->content_id)
                      << (existing->trust_rebuilds ? "   (any rebuild is accepted)" : "") << '\n';
            for (const std::string& r : existing->send) {
                std::cout << "    may say:  " << r << '\n';
            }
            for (const std::string& r : existing->observe) {
                std::cout << "    may read: " << r << '\n';
            }
            if (!existing->note.empty()) {
                std::cout << "    note: " << existing->note << '\n';
            }
        }
        // AND WHAT THE BUS ACTUALLY THINKS, which is the line that matters: asked of
        // the live authority through the warden, so a file that disagrees with the
        // running system shows up here instead of being believed. Asked as a MESSAGE,
        // by the operator seat, exactly as anything else here is — and settled on this
        // host's own conversation, so it is this artifact's authority and not whatever
        // description happened to arrive most recently.
        const Answer a = s.ask(s.warden_id(), loom::host::HostDescribeAuthority::zen_name,
                               loom::host::HostDescribeAuthority::zen_version,
                               {{"artifact", name}});
        if (!a.settled()) {
            std::cout << "    (could not ask: " << a.trouble << ")\n";
            return;
        }
        if (a.reply->name != loom::AuthorityDescription::zen_name) {
            std::cout << "    " << HostSession::describe_reply(*a.reply) << '\n';
            return;
        }
        std::cout << "    LIVE, weave " << a.reply->value.get("subject")->as_int() << ":\n";
        const auto print_list = [](const loom::Cell* list, const char* label) {
            if (list == nullptr || list->as_list().empty()) {
                std::cout << "      " << label << ": nothing\n";
                return;
            }
            for (const loom::Cell& c : list->as_list()) {
                std::cout << "      " << label << ": " << c.as_text() << '\n';
            }
        };
        print_list(a.reply->value.get("base"), "baseline (frozen at admission)");
        print_list(a.reply->value.get("delegated"), "delegated (revocable)");
        return;
    }

    loom::host::AuthorityRule rule =
        (existing != nullptr) ? *existing : loom::host::AuthorityRule{};
    rule.artifact = name;

    if (sub == "trust") {
        rule.may_run = true;
        for (std::size_t i = 3; i < tok.size(); ++i) {
            if (tok[i].text == "--rebuilds") {
                rule.trust_rebuilds = true;
            } else if (tok[i].text == "--ask-again") {
                // THE WAY BACK. Without it, turning `--rebuilds` off meant hand-editing
                // the file — a poor answer for the one switch here that lets new code run
                // without asking.
                rule.trust_rebuilds = false;
            } else {
                std::cout << "  unknown option '" << tok[i].text << "'\n";
                return;
            }
        }
        // Re-pin from whatever the policy last saw refused, if anything, so "trust"
        // after a changed-file refusal approves THE BUILD THAT WAS REFUSED and not some
        // other one. Otherwise clear the pin and let the next load record it.
        rule.content_id.clear();
        for (const loom::host::PendingDecision& p : store.pending()) {
            if (p.artifact == name && !p.content_id.empty()) {
                rule.content_id = p.content_id;
            }
        }
        const bool trusts_rebuilds = rule.trust_rebuilds;
        if (!store.put(std::move(rule), &error)) {
            // NOTHING CHANGED, AND THAT IS WHY THE SENTENCE CAN SAY SO. The store writes
            // the file before it changes what this host permits, so a failed command is
            // a command that did not happen — rather than an approval good for this
            // session that the next boot has never heard of.
            std::cout << "  " << error << "\n  '" << name
                      << "' is unchanged: it still may " << (existing != nullptr && existing->may_run
                                                                 ? "run"
                                                                 : "NOT run")
                      << ", here and after a restart.\n";
            return;
        }
        // BOTH HALVES OF WHAT WAS JUST DECIDED, because a person who typed one word is
        // owed the other: this grants the right to RUN and nothing else, and whether a
        // later rebuild will ask again is exactly what the flag changed.
        std::cout << "  '" << name << "' may run"
                  << (trusts_rebuilds ? ", and so will any rebuild of it"
                                      : "; a rebuild of it will ask again")
                  << ". It may still say nothing until you allow something.\n";
        return;
    }

    if (sub == "deny") {
        rule.may_run = false;
        if (!store.put(std::move(rule), &error)) {
            std::cout << "  " << error << "\n  '" << name << "' is unchanged.\n";
            return;
        }
        std::cout << "  '" << name << "' may not run. A copy already loaded keeps running — "
                     "end it with 'stop " << name << "'.\n";
        return;
    }

    if (sub == "allow") {
        if (tok.size() < 4) {
            std::cout << "  usage: authority allow <name> <rule>\n"
                         "  rules read exactly as 'authority show' prints them:\n"
                         "    Greet v1 -> any target     Tick v1 -> role clock\n"
                         "    any shape -> any target    observe Tick v1\n";
            return;
        }
        std::string text;
        for (std::size_t i = 3; i < tok.size(); ++i) {
            text += (i == 3 ? "" : " ") + tok[i].text;
        }
        loom::LiveAuthority probe;
        if (!loom::host::apply_rule(text, &probe, &error)) {
            std::cout << "  " << error << '\n';
            return;
        }
        const bool observe = text.rfind("observe", 0) == 0;
        const std::string canonical = loom::host::canonical_rule(text, observe);
        // ALREADY GRANTED IS SAID, NOT STORED TWICE. A repeated approval used to append a
        // second copy of the same permission, and one `revoke` then removed one copy,
        // reported a revocation, and left the permission installed. A person retrying a
        // command must not be able to make their own decision unrevokable.
        std::vector<std::string>& into = observe ? rule.observe : rule.send;
        for (const std::string& r : into) {
            if (loom::host::canonical_rule(r, observe) == canonical) {
                std::cout << "  '" << name << "' is already allowed '" << canonical
                          << "' — nothing to add. One 'authority revoke " << name << " "
                          << canonical << "' takes it back.\n";
                return;
            }
        }
        into.push_back(canonical);
        if (!store.put(std::move(rule), &error)) {
            std::cout << "  " << error << "\n  nothing was allowed; '" << name
                      << "' permits exactly what it permitted before.\n";
            return;
        }
        std::cout << "  remembered. " << s.sync(name) << '\n';
        return;
    }

    if (sub == "revoke") {
        if (existing == nullptr) {
            std::cout << "  no standing decision for '" << name << "'\n";
            return;
        }
        std::string text;
        const bool all = tok.size() == 3;
        if (all) {
            rule.send.clear();
            rule.observe.clear();
        } else {
            for (std::size_t i = 3; i < tok.size(); ++i) {
                text += (i == 3 ? "" : " ") + tok[i].text;
            }
            // EVERY COPY, not the first one. The store collapses duplicates now, so
            // there should never be a second — but a file a person hand-edited between
            // two runs is exactly the case where "should" is not a guarantee, and a
            // revoke that leaves one behind is the defect this loop exists to prevent.
            std::size_t removed = 0;
            const auto drop = [&removed](std::vector<std::string>& v, const std::string& want,
                                         bool observe) {
                for (auto it = v.begin(); it != v.end();) {
                    if (loom::host::canonical_rule(*it, observe) == want) {
                        it = v.erase(it);
                        ++removed;
                    } else {
                        ++it;
                    }
                }
            };
            drop(rule.send, loom::host::canonical_rule(text, /*observe=*/false), false);
            drop(rule.observe, loom::host::canonical_rule(text, /*observe=*/true), true);
            if (removed == 0) {
                std::cout << "  '" << text << "' is not one of its rules (see 'authority show "
                          << name << "')\n";
                return;
            }
        }
        const loom::host::AuthorityRule after = rule; // for the honest effective-authority line
        if (!store.put(std::move(rule), &error)) {
            std::cout << "  " << error << "\n  nothing was revoked; '" << name
                      << "' permits exactly what it permitted before.\n";
            return;
        }
        // BOTH HALVES, from one word. The file is the memory and the bus is the effect;
        // a revoke that changed only one of them would be a promise half kept.
        std::cout << "  revoked, for this run and the next. " << s.sync(name) << '\n';
        // AND THE HONEST REMAINDER. A narrow rule taken back under a broad one that is
        // still standing has changed the list and changed nothing about what the weave
        // may do, and a person reading "revoked" deserves to be told which they got.
        if (!all && still_permitted(after, text)) {
            std::cout << "  NOTE: '" << text << "' is STILL PERMITTED by another rule you have "
                         "granted '" << name << "'. See 'authority show " << name << "'.\n";
        }
        return;
    }

    if (sub == "forget") {
        if (!store.forget(name, &error)) {
            std::cout << "  " << error << '\n';
            return;
        }
        std::cout << "  forgotten. " << s.sync(name) << '\n';
        return;
    }

    std::cout << "  unknown: authority " << sub << " (try 'help')\n";
}

void cmd_asks(HostSession& s, const std::vector<Token>& tok) {
    if (tok.size() >= 3 && tok[1].text == "forget") {
        std::uint64_t n = 0;
        const bool was_open = parse_u64(tok[2].text, n) && s.console().awaiting(n);
        if (n == 0 || !s.console().forget_ask(n)) {
            std::cout << "  no open conversation numbered '" << tok[2].text << "' (see 'asks')\n";
            return;
        }
        if (!was_open) {
            std::cout << "  dropped the unread answer to " << n << ".\n";
            return;
        }
        // Named `forget` rather than `cancel` because the shorter word would be the lie:
        // Loom has no cancellation vocabulary, so nothing at the far end is told
        // anything. What changes is only that this console stops recognizing the answer.
        std::cout << "  no longer waiting on " << n
                  << ". Nothing was cancelled — a late answer will simply not be recognized.\n";
        return;
    }
    const std::vector<loom::PendingAsk> open = s.console().open_asks();
    const std::vector<std::uint64_t> answered = s.console().answered_asks();
    if (open.empty() && answered.empty()) {
        std::cout << "  no open conversations (" << s.console().ask_capacity()
                  << " trackable at once).\n";
        return;
    }
    // An answer the loop has not printed yet holds its slot exactly as an open conversation
    // does, so it is listed with them. The loop prints and releases it at its next turn.
    for (std::uint64_t id : answered) {
        std::cout << "  " << id << "  answered; printed at the next prompt\n";
    }
    for (const loom::PendingAsk& p : open) {
        std::cout << "  " << p.id << "  " << (p.shape.empty() ? "(unnamed shape)" : p.shape)
                  << " v" << p.version << "  waiting on ";
        if (p.to_role()) {
            std::cout << "whoever holds role '" << p.role << "'";
        } else {
            std::cout << "weave " << p.respondent.value;
        }
        std::cout << '\n';
    }
    std::cout << "  " << s.console().asks_held() << " of " << s.console().ask_capacity()
              << " tracked.\n";
}

#if ZEN_HOST_HAS_KERNEL
void cmd_lifecycle(HostSession& s, const std::vector<Token>& tok) {
    const std::string& cmd = tok[0].text;
    const auto print_adoption = [&s]() {
        for (const std::string& line : s.take_adoption_log()) {
            std::cout << line << '\n';
        }
    };
    if (cmd == "start") {
        if (tok.size() < 3) {
            std::cout << "  usage: start <name> <path> [role]\n";
            return;
        }
        const std::string name = tok[1].text;
        const Answer a = s.ask(s.manager(), loom::LoadWeave::zen_name, loom::LoadWeave::zen_version,
                               {{"name", name},
                                {"path", tok[2].text},
                                {"role", tok.size() > 3 ? tok[3].text : std::string{}}});
        if (!a.settled()) {
            std::cout << "  " << a.trouble << '\n';
            print_adoption();
            return;
        }
        std::cout << "  " << HostSession::describe_reply(*a.reply) << '\n';
        // THE HOST DOES NOT ADOPT HERE, and that absence is the repair. Adoption happened
        // inside the load, before the weave was told it was live (see rule 2 in the file
        // header); what is printed below is what the warden actually installed, not
        // something this command decided from a payload.
        print_adoption();
        const std::string mismatch = s.governed_note(name, *a.reply);
        if (!mismatch.empty()) {
            std::cout << mismatch << '\n';
        }
        return;
    }
    if (cmd == "reload") {
        if (tok.size() < 3) {
            std::cout << "  usage: reload <name> <path>\n";
            return;
        }
        const Answer a =
            s.ask(s.manager(), loom::ReloadWeave::zen_name, loom::ReloadWeave::zen_version,
                  {{"name", tok[1].text}, {"path", tok[2].text}});
        std::cout << "  " << (a.settled() ? HostSession::describe_reply(*a.reply) : a.trouble)
                  << '\n';
        print_adoption(); // a reload is new code behind a stable id: it is adopted again
        return;
    }
    if (cmd == "stop") {
        if (tok.size() < 2) {
            std::cout << "  usage: stop <name>\n";
            return;
        }
        // THIS ENDS AN APPLICATION, NOT THE HOST — the one place a person might
        // reasonably expect otherwise, so the answer says which ended.
        const std::string name = tok[1].text;
        // The control door's primitives are ZEN_SHAPE-named and therefore carry NO
        // `zen.` prefix — `UnloadLibrary`, not `zen.UnloadLibrary`. Spelling one of these
        // as a literal is how this file first shipped a command that silently did
        // nothing, so they are all the types' own constants now and the compiler checks
        // them.
        const Answer a = s.ask(s.control(), loom::UnloadLibrary::zen_name,
                               loom::UnloadLibrary::zen_version, {{"name", name}});
        if (!a.settled()) {
            std::cout << "  " << a.trouble << '\n';
            return;
        }
        if (a.reply->name == loom::Refused::zen_name) {
            std::cout << "  " << HostSession::describe_reply(*a.reply) << '\n';
            return;
        }
        // The capability went with the weave, retired by the same door that unloaded it
        // — so `authority show` stops describing a subject that no longer exists, by
        // whichever route the unload arrived.
        std::cout << "  '" << name << "' stopped. This host is still running.\n";
        return;
    }
    if (cmd == "list") {
        const Answer a =
            s.ask(s.manager(), loom::ListLoaded::zen_name, loom::ListLoaded::zen_version, {});
        std::cout << "  " << (a.settled() ? HostSession::describe_reply(*a.reply) : a.trouble)
                  << '\n';
        return;
    }
}
#endif

/// Run one typed line. Returns false when the person asked this host to stop.
bool dispatch(const std::string& line, HostSession& session, const loom::host::BootPlan& plan,
              const PlanSource& plan_source, loom::host::AuthorityStore& store,
              const loom::host::StoreLock& lock) {
    const std::vector<Token> tok = tokenize(line);
    if (tok.empty()) {
        return true;
    }
    const std::string& cmd = tok[0].text;
    if (cmd == "quit" || cmd == "exit") {
        return false; // HOST shutdown; see the file header
    } else if (cmd == "help") {
        print_help();
    } else if (cmd == "status") {
        cmd_status(session, plan, plan_source, store, lock);
    } else if (cmd == "authority") {
        cmd_authority(session, store, tok);
    } else if (cmd == "asks") {
        cmd_asks(session, tok);
    } else if (cmd == "weaves") {
        for (const loom::WeaveInfo& w : session.console().weaves()) {
            std::cout << "  weave " << w.id.value << "  accepts:";
            for (const loom::ShapeRef& a : w.accepts) {
                std::cout << ' ' << a.name << " v" << a.version;
            }
            if (w.accepts.empty()) {
                std::cout << " (none)";
            }
            std::cout << '\n';
        }
    } else if (cmd == "describe") {
        std::uint64_t v = 0;
        if (tok.size() != 3 || !parse_u64(tok[2].text, v)) {
            std::cout << "  usage: describe <Shape> <version>\n";
            return true;
        }
        auto d = session.console().describe(tok[1].text, static_cast<std::uint32_t>(v));
        if (!d) {
            std::cout << "  no such registered shape\n";
            return true;
        }
        for (const loom::FieldDesc& f : d->fields) {
            std::cout << "    " << f.name << " : " << f.type
                      << (f.required ? " (required)" : " (optional)") << '\n';
        }
    } else if (cmd == "send") {
        std::uint64_t id = 0;
        std::uint64_t v = 0;
        if (tok.size() < 4 || !parse_u64(tok[1].text, id) || !parse_u64(tok[3].text, v)) {
            std::cout << "  usage: send <id> <Shape> <version> [args ...]\n";
            return true;
        }
        std::vector<loom::Arg> args;
        for (std::size_t i = 4; i < tok.size(); ++i) {
            args.push_back(lex_arg(tok[i]));
        }
        // TRACKED: this command reports the attributed answer, so it holds the conversation
        // and owes it back — taken below, or taken when it lands later, or forgotten.
        const loom::Composed c =
            session.console().compose(loom::WeaveId{id}, tok[2].text,
                                      static_cast<std::uint32_t>(v), args,
                                      loom::ConsoleTracking::Tracked);
        if (c.status == loom::Composed::Status::Error) {
            std::cout << "  compose error: " << c.error << '\n';
            return true;
        }
        if (c.status == loom::Composed::Status::NeedsInput) {
            std::cout << "  needs input — name the fields with field=value:\n";
            for (const loom::FieldDesc& f : c.open_fields) {
                std::cout << "    " << f.name << " : " << f.type << '\n';
            }
            return true;
        }
        // BOUNDED, like everything else here. A weave whose handler queues its own next
        // message keeps the bus busy for as long as it likes; this host spends a fixed
        // number of turns on the operator's send and then goes back to serving both.
        (void)session.service(c.ask, kSettleTurns);
        const loom::SendOutcome o = session.console().outcome(c.ticket);
        if (o.refused) {
            std::cout << "  refused: " << o.reason << '\n';
            (void)session.console().forget_ask(c.ask);
            return true;
        }
        std::cout << "  sent.";
        if (c.ask == 0) {
            std::cout << "  (untracked: this console is holding "
                      << session.console().asks_held()
                      << " conversations already, so this one's answer cannot be attributed)";
        } else if (auto reply = session.console().take_settled(c.ask)) {
            // ATTRIBUTED, not "whatever arrived last". The label names the reply to THIS
            // send, from the weave this send named — and taking it returns the slot.
            std::cout << "  reply -> " << reply->label << "  "
                      << HostSession::describe_reply(*reply);
        } else {
            std::cout << "  no answer yet (ask " << c.ask << "; 'asks' to see it)";
        }
        std::cout << '\n';
    } else if (cmd == "buffer") {
        const std::size_t n = session.console().buffer_size();
        const std::uint64_t gone = session.console().evicted().buffer;
        for (std::uint64_t label = gone + 1; label <= gone + n; ++label) {
            if (auto e = session.console().buffer_at(static_cast<std::size_t>(label))) {
                std::cout << "  " << e->label << " : " << e->name << " v" << e->version
                          << "  from weave " << e->sender.value << "  "
                          << HostSession::describe_reply(*e) << '\n';
            }
        }
        if (n == 0) {
            std::cout << "  (no replies)\n";
        }
    } else if (cmd == "show") {
        std::uint64_t n = 0;
        if (tok.size() != 2 || tok[1].text.empty() || tok[1].text[0] != 'm' ||
            !parse_u64(tok[1].text.substr(1), n)) {
            std::cout << "  usage: show <mN>\n";
            return true;
        }
        auto e = session.console().buffer_at(static_cast<std::size_t>(n));
        if (!e) {
            std::cout << "  no such reply (it may have been evicted)\n";
            return true;
        }
        // WHO SENT IT AND WHICH CONVERSATION IT NAMED are printed because they are what
        // makes an entry mean anything: a `zen.Result` in this window may be an answer
        // this console earned or an unsolicited message any loaded weave may author.
        std::cout << "  " << e->label << " : " << e->name << " v" << e->version << "  "
                  << HostSession::describe_reply(*e) << '\n';
        std::cout << "    from weave " << e->sender.value << ", conversation "
                  << e->correlation << (e->answers_ask ? "  (Loom attests: an answer to an ask "
                                                         "this console sent)"
                                                       : "") << '\n';
    } else if (cmd == "tap") {
        std::uint64_t want = 20;
        if (tok.size() == 2) {
            (void)parse_u64(tok[1].text, want);
        }
        const std::vector<loom::TapEvent> ev = session.console().tap();
        const std::size_t from = ev.size() > want ? ev.size() - want : 0;
        for (std::size_t i = from; i < ev.size(); ++i) {
            std::cout << "  " << ev[i].kind << ' ' << ev[i].schema << "  " << ev[i].sender.value
                      << " -> " << ev[i].target.value;
            if (!ev[i].refusal.empty()) {
                std::cout << "  [" << ev[i].refusal << ']';
            }
            std::cout << '\n';
        }
        if (ev.empty()) {
            std::cout << "  (no bus events yet)\n";
        }
#if ZEN_HOST_HAS_KERNEL
    } else if (cmd == "start" || cmd == "reload" || cmd == "stop" || cmd == "list") {
        cmd_lifecycle(session, tok);
#else
    } else if (cmd == "start" || cmd == "reload" || cmd == "stop" || cmd == "list") {
        std::cout << "  " << HostSession::no_kernel_note() << '\n';
#endif
    } else {
        std::cout << "  unknown command: " << cmd << " (try 'help')\n";
    }
    return true;
}

/// Print, and COLLECT, every conversation that settled between commands. This is what makes
/// PENDING an honest answer rather than a dropped one: a load, a send or a sync that took
/// longer than the host's patience still reports, in the order the conversations were opened.
///
/// Every command takes the answer it can take before it returns, so an answer still held
/// when the loop turns is by definition a late one, and this host's to report — including a
/// boot row's, left pending before anybody typed anything. Taking it returns its slot, which
/// is why a host that has served a million commands holds exactly what is still open.
void report_late_answers(HostSession& session, bool& prompt_shown) {
    for (std::uint64_t id : session.console().answered_asks()) {
        std::optional<loom::BufferEntry> reply = session.console().take_settled(id);
        if (!reply) {
            continue;
        }
        if (prompt_shown) {
            std::cout << '\n'; // do not write over the prompt the person is typing at
            prompt_shown = false;
        }
        std::cout << "  [ask " << id << " settled] " << reply->name << " v" << reply->version
                  << "  from weave " << reply->sender.value << "  "
                  << HostSession::describe_reply(*reply) << '\n';
        for (const std::string& line : session.take_adoption_log()) {
            std::cout << line << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse_options(argc, argv);
    if (opt.done) {
        return 0;
    }
    if (!opt.ok) {
        return 2;
    }

    // ONE HOST OWNS ONE DECISION STORE. Claimed before the store is even read, because
    // the danger is not two readers but two whole-file writers: the second one to write
    // restores whatever the first one revoked. `--check` takes no claim — it reads,
    // prints and exits, and refusing it would make a diagnostic unavailable exactly when
    // a person has a host running and wants to know what it read.
    loom::host::StoreLock lock;
    if (!opt.check_only) {
        std::string why;
        bool taken = false;
        if (!lock.claim(opt.authority, &why, &taken)) {
            std::cerr << "loom-host: " << why << '\n';
            if (taken) {
                std::cerr << "loom-host: decisions are written whole, so a second host sharing "
                             "this file would put back what the first one revoked. Either quit "
                             "the other host, or give this one its own file:\n"
                             "             loom-host --authority <other-file>\n";
            }
            return 4;
        }
    }

    // THE FILES ARE READ BEFORE ANYTHING IS BUILT. A malformed AUTHORITY store is fatal
    // either way: it is the record of what a person decided, and a host that came up
    // ignoring it would be a host running under decisions nobody made.
    std::string error;
    loom::host::AuthorityStore store;
    if (!store.open(opt.authority, &error)) {
        std::cerr << "loom-host: " << error << '\n';
        return 3;
    }
    // A malformed BOOT PLAN is a different thing, and `--no-boot` is documented as the
    // way back in when the plan is what is broken. It used to parse the plan before
    // applying `--no-boot`, so the advertised recovery route exited 3 without ever
    // opening a console — the one state from which a person could have fixed the file.
    // The plan is still not RUN, and `--check` still fails: validating is what it is for.
    PlanSource plan_source;
    plan_source.path = opt.boot_plan;
    loom::host::BootPlan plan;
    if (!loom::host::read_boot_plan(opt.boot_plan, &plan, &error)) {
        if (opt.check_only || !opt.no_boot) {
            std::cerr << "loom-host: " << error << '\n';
            if (!opt.check_only) {
                std::cerr << "loom-host: start with --no-boot to get a console without running "
                             "this plan.\n";
            }
            return 3;
        }
        plan_source.parse_error = error;
        plan = loom::host::BootPlan{}; // nothing to walk; every row was in the file that failed
    }

    if (opt.check_only) {
        std::cout << "boot plan: " << (plan.source.empty() ? "(none found)" : plan.source) << '\n';
        for (const loom::host::BootEntry& e : plan.entries) {
            std::cout << "  " << e.name << "  " << e.path
                      << (e.role.empty() ? "" : ("  @" + e.role))
                      << (e.enabled ? "" : "  (disabled)") << '\n';
        }
        std::cout << "authority: " << (store.path().empty() ? "(none)" : store.path()) << '\n';
        for (const loom::host::AuthorityRule& r : store.rules()) {
            std::cout << "  " << (r.may_run ? "run " : "DENY") << "  " << r.artifact << '\n';
        }
        return 0;
    }

    HostSession session(store, opt);
    std::cout << kVersion << "   containment: " << HostSession::containment_note() << '\n';
    if (!session.can_host_weaves()) {
        std::cout << "NOTE: " << HostSession::no_kernel_note() << '\n';
    }
    if (!plan_source.parse_error.empty()) {
        std::cout << "the boot plan was NOT run: " << plan_source.parse_error << '\n';
        std::cout << "  nothing from it was started. Fix " << plan_source.path
                  << " and restart, or start things by hand here.\n";
    }
    session.boot(plan);
    if (plan_source.parse_error.empty()) {
        for (const std::string& line : session.report().render()) {
            std::cout << line << '\n';
        }
    }
    for (const std::string& line : session.take_adoption_log()) {
        std::cout << line << '\n';
    }
    print_notes(store);
    std::size_t notes_shown = store.notes().size();
    if (!session.report().complete() || !plan_source.parse_error.empty()) {
        // The console is the recovery surface, so say that it is one. A host that
        // exited here would leave a person with nothing to diagnose from.
        std::cout << "  the boot did not complete. The console is up: 'status' for the table, "
                     "'authority pending' for what is waiting on you.\n";
    }
    std::cout << "type 'help' for commands, 'quit' to shut this host down.\n";

    // ---- THE HOST LOOP ------------------------------------------------------
    //
    // Both parties get served, every pass: one bounded bus turn, then the person, with a
    // wait only when there is nothing else to do. There is no call in here that can fail
    // to return because a participant decided to keep talking — and the wait itself is a
    // real deadline even while the person has typed half a command (host/line_input.hpp).
    loom::host::LineInput input;
    bool prompt_shown = false;
    bool running = true;
    while (running) {
        session.turn();
        report_late_answers(session, prompt_shown);
        if (store.notes().size() != notes_shown) {
            if (prompt_shown) {
                std::cout << '\n';
                prompt_shown = false;
            }
            print_new_notes(store, &notes_shown);
        }

        if (!prompt_shown) {
            std::cout << "loom> " << std::flush;
            prompt_shown = true;
        }
        std::string line;
        // Poll while the bus has work; wait only when it does not. An idle host costs
        // nothing and a busy one never stops reading.
        const int wait = session.busy() ? 0 : kIdleWaitMs;
        const loom::host::LineInput::Status got = input.read(&line, wait);
        if (got == loom::host::LineInput::Status::Closed) {
            break;
        }
        if (got == loom::host::LineInput::Status::Idle) {
            continue;
        }
        prompt_shown = false;
        if (got == loom::host::LineInput::Status::TooLong) {
            print_too_long(input.too_long());
            continue;
        }
        running = dispatch(line, session, plan, plan_source, store, lock);
        // What the policy decided during the command, said with the command's own output.
        print_new_notes(store, &notes_shown);
    }
    return 0;
}
