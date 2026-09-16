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
// SHUTDOWN HAS TWO MEANINGS AND THEY ARE DIFFERENT COMMANDS.
//   `stop <name>`  ends one APPLICATION: its weave leaves the bus, its library closes,
//                  the host keeps running and the console stays up.
//   `quit`         ends the HOST: the console loop returns and everything comes down in
//                  reverse construction order.
// A boot failure is neither. It leaves the console up, which is the only state from
// which a person can find out what went wrong and fix it.

#include "authority.hpp"
#include "boot_plan.hpp"
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
  --no-boot           come up with the console only, starting nothing. The way back in
                      when a boot plan is what is broken.
  --check             read and validate both files, print what they say, and exit
                      without starting anything or opening a console.
  --help, --version

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
        control_ = loom::mount_control(*kernel_, bus_);
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

    /// Walk the plan in the person's order. Never throws a row away and never stops the
    /// host: a row that fails is a row with a state and a reason, and the console comes
    /// up either way.
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
        bus_.drain_until_idle();
    }

    /// Load one artifact and, if it lands, put it under the warden's administration and
    /// apply whatever the person has already decided about its speech.
    ///
    /// THE BOOT WALK USES THE KERNEL DIRECTLY AND THE CONSOLE USES THE MANAGER, on
    /// purpose. They are two different doors and both must be governed, so both are
    /// exercised: this one is the host's own C++ call through the POLICY-mediated
    /// `load` (not the four-argument one — the host does not name grants for the
    /// person's artifacts), and `start` at the console goes out as a `zen.LoadWeave`
    /// through the steward and the control door. Same policy, same refusals, same
    /// words.
    void start_row(const loom::host::BootEntry& e, loom::host::BootOutcome* row) {
#if ZEN_HOST_HAS_KERNEL
        const loom::LoadResult r = kernel_->load(e.name, e.path, e.role);
        if (!r.ok) {
            // "refused" and "failed" send a person to different places — the console or
            // the filesystem — so the report distinguishes them by who said no.
            row->state = r.error.find("admission refused") != std::string::npos
                             ? loom::host::BootState::Refused
                             : loom::host::BootState::Failed;
            row->detail = r.error;
            return;
        }
        row->state = loom::host::BootState::Started;
        row->weave = r.id.value;
        adopt(e.name, r.id);
#else
        (void)e;
        row->state = loom::host::BootState::Failed;
        row->detail = no_kernel_note();
#endif
    }

    /// Put a freshly-loaded artifact under administration and sync it to the store.
    ///
    /// THE CEILING IS FULL, AND THAT IS NOT A CONTROL BEING SKIPPED. A ceiling bounds a
    /// DELEGATE — it is how a host limits a Weaver acting on somebody else's behalf. The
    /// delegate here is the host's own warden, obeying the person at this console
    /// directly, so the thing that actually bounds what gets installed is the file the
    /// person wrote. Naming a narrower ceiling would look like a second control while
    /// being nothing but a cap on what the person could later approve without a
    /// restart. It is stated full, and the file is the policy.
    void adopt(const std::string& artifact, loom::WeaveId id) {
        warden_->govern(artifact,
                        loom::host_grant_authority(
                            bus_, id, loom::LiveAuthority{}.allow_any().allow_observe_any()));
        (void)sync(artifact);
    }

    /// Make the bus agree with the store for one artifact — the only route by which
    /// delegated authority changes, and it is a message, so it shows on the tap.
    std::string sync(const std::string& artifact) {
        if (!warden_->governs(artifact)) {
            return "nothing loaded under '" + artifact + "' to administer";
        }
        std::string trouble;
        auto reply = ask(warden_id_, loom::host::HostAuthoritySync::zen_name,
                         loom::host::HostAuthoritySync::zen_version, {{"artifact", artifact}},
                         &trouble);
        if (!reply) {
            return trouble.empty() ? std::string("the warden did not answer") : trouble;
        }
        return describe_reply(*reply);
    }

    /// Compose a message from the operator seat, deliver it, and hand back the reply.
    /// One helper, so every operator-initiated conversation in this file is the same
    /// public path: the console's own weave speaks, as itself, under its own grant.
    /// `trouble`, when given, receives why there is no reply. It exists because the two
    /// silences are completely different problems and looked identical once: a shape
    /// this console could not even compose (a wrong name — the bug this parameter was
    /// added after) reads exactly like a weave that chose not to answer.
    std::optional<loom::BufferEntry> ask(loom::WeaveId target, const char* shape,
                                         std::uint32_t version,
                                         const std::map<std::string, loom::FieldValue>& fields,
                                         std::string* trouble = nullptr) {
        const std::uint64_t before = console_->evicted().buffer + console_->buffer_size();
        std::string error;
        const loom::Ticket t = console_->submit(target, shape, version, fields, &error);
        if (!error.empty()) {
            if (trouble != nullptr) {
                *trouble = std::string("could not compose ") + shape + ": " + error;
            }
            return std::nullopt;
        }
        console_->pump();
        const loom::SendOutcome o = console_->outcome(t);
        if (o.refused && trouble != nullptr) {
            *trouble = std::string("the send was refused: ") + o.reason;
        }
        const std::uint64_t after = console_->evicted().buffer + console_->buffer_size();
        if (after <= before) {
            if (trouble != nullptr && trouble->empty()) {
                *trouble = "no answer came back";
            }
            return std::nullopt;
        }
        return console_->buffer_at(static_cast<std::size_t>(after));
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
    void release(const std::string& artifact) { warden_->release(artifact); }

private:
    loom::host::AuthorityStore& store_;
    const Options& opt_;
    loom::Switchboard bus_;
    std::unique_ptr<loom::ConsoleEngine> console_;
    loom::host::HostWarden* warden_ = nullptr; ///< owned by the bus; non-owning here
    loom::WeaveId warden_id_{};
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

/// What the policy did that nobody asked about. Printed after every boot and from
/// `status`, because an admission the person did not have to answer is still something
/// they are entitled to know happened — above all that an artifact came up on REBUILT
/// code under authority they granted for an earlier build.
void print_notes(const loom::host::AuthorityStore& store) {
    for (const std::string& n : store.notes()) {
        std::cout << "  note: " << n << '\n';
    }
}

void cmd_status(HostSession& s, const loom::host::BootPlan& plan,
                const loom::host::AuthorityStore& store) {
    std::cout << "  " << kVersion << "   weave ABI v" << ZEN_ABI_VERSION << '\n';
    std::cout << "  containment: " << HostSession::containment_note() << '\n';
    std::cout << "  boot plan:   " << (plan.source.empty() ? "(none)" : plan.source) << '\n';
    std::cout << "  authority:   " << (store.path().empty() ? "(none)" : store.path()) << '\n';
    if (!s.can_host_weaves()) {
        std::cout << "  NOTE: " << HostSession::no_kernel_note() << '\n';
    }
    std::cout << "  boot:\n";
    for (const std::string& line : s.report().render()) {
        std::cout << line << '\n';
    }
    print_notes(store);
    if (!store.pending().empty()) {
        std::cout << "  " << store.pending().size()
                  << " decision(s) waiting on you — 'authority pending'\n";
    }
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
        // by the operator seat, exactly as anything else here is.
        std::string trouble;
        auto reply = s.ask(s.warden_id(), loom::host::HostDescribeAuthority::zen_name,
                           loom::host::HostDescribeAuthority::zen_version, {{"artifact", name}},
                           &trouble);
        if (!reply) {
            std::cout << "    (could not ask: " << trouble << ")\n";
            return;
        }
        if (reply->name != loom::AuthorityDescription::zen_name) {
            std::cout << "    " << HostSession::describe_reply(*reply) << '\n';
            return;
        }
        std::cout << "    LIVE, weave " << reply->value.get("subject")->as_int() << ":\n";
        const auto print_list = [](const loom::Cell* list, const char* label) {
            if (list == nullptr || list->as_list().empty()) {
                std::cout << "      " << label << ": nothing\n";
                return;
            }
            for (const loom::Cell& c : list->as_list()) {
                std::cout << "      " << label << ": " << c.as_text() << '\n';
            }
        };
        print_list(reply->value.get("base"), "baseline (frozen at admission)");
        print_list(reply->value.get("delegated"), "delegated (revocable)");
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
            std::cout << "  " << error << '\n';
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
            std::cout << "  " << error << '\n';
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
        if (text.rfind("observe", 0) == 0) {
            rule.observe.push_back(text);
        } else {
            rule.send.push_back(text);
        }
        if (!store.put(std::move(rule), &error)) {
            std::cout << "  " << error << '\n';
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
        if (tok.size() == 3) {
            rule.send.clear();
            rule.observe.clear();
        } else {
            std::string text;
            for (std::size_t i = 3; i < tok.size(); ++i) {
                text += (i == 3 ? "" : " ") + tok[i].text;
            }
            const auto drop = [&text](std::vector<std::string>& v) {
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (*it == text) {
                        v.erase(it);
                        return true;
                    }
                }
                return false;
            };
            if (!drop(rule.send) && !drop(rule.observe)) {
                std::cout << "  '" << text << "' is not one of its rules (see 'authority show "
                          << name << "')\n";
                return;
            }
        }
        if (!store.put(std::move(rule), &error)) {
            std::cout << "  " << error << '\n';
            return;
        }
        // BOTH HALVES, from one word. The file is the memory and the bus is the effect;
        // a revoke that changed only one of them would be a promise half kept.
        std::cout << "  revoked, for this run and the next. " << s.sync(name) << '\n';
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

#if ZEN_HOST_HAS_KERNEL
void cmd_lifecycle(HostSession& s, const std::vector<Token>& tok) {
    const std::string& cmd = tok[0].text;
    if (cmd == "start") {
        if (tok.size() < 3) {
            std::cout << "  usage: start <name> <path> [role]\n";
            return;
        }
        const std::string name = tok[1].text;
        std::string trouble;
        auto reply = s.ask(s.manager(), loom::LoadWeave::zen_name, loom::LoadWeave::zen_version,
                           {{"name", name},
                            {"path", tok[2].text},
                            {"role", tok.size() > 3 ? tok[3].text : std::string{}}},
                           &trouble);
        if (!reply) {
            std::cout << "  " << trouble << '\n';
            return;
        }
        std::cout << "  " << HostSession::describe_reply(*reply) << '\n';
        if (reply->name == loom::Result::zen_name) {
            std::uint64_t id = 0;
            if (parse_u64(reply->value.get("value")->as_text(), id)) {
                s.adopt(name, loom::WeaveId{id});
                std::cout << "  now under administration; " << s.sync(name) << '\n';
            }
        }
        return;
    }
    if (cmd == "reload") {
        if (tok.size() < 3) {
            std::cout << "  usage: reload <name> <path>\n";
            return;
        }
        auto reply =
            s.ask(s.manager(), loom::ReloadWeave::zen_name, loom::ReloadWeave::zen_version,
                  {{"name", tok[1].text}, {"path", tok[2].text}});
        std::cout << "  " << (reply ? HostSession::describe_reply(*reply)
                                    : std::string("the steward did not answer"))
                  << '\n';
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
        std::string trouble;
        auto reply = s.ask(s.control(), loom::UnloadLibrary::zen_name,
                           loom::UnloadLibrary::zen_version, {{"name", name}}, &trouble);
        if (!reply) {
            std::cout << "  the kernel door did not answer\n";
            return;
        }
        if (reply->name == loom::Refused::zen_name) {
            std::cout << "  " << HostSession::describe_reply(*reply) << '\n';
            return;
        }
        // Its authority went with it. Dropping the capability is not a revocation —
        // there is no longer a subject to revoke anything from — and the person's
        // standing decision in the file is untouched, so starting it again restores
        // exactly what they approved.
        s.release(name);
        std::cout << "  '" << name << "' stopped. This host is still running.\n";
        return;
    }
    if (cmd == "list") {
        auto reply =
            s.ask(s.manager(), loom::ListLoaded::zen_name, loom::ListLoaded::zen_version, {});
        std::cout << "  " << (reply ? HostSession::describe_reply(*reply)
                                    : std::string("the steward did not answer"))
                  << '\n';
        return;
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse_options(argc, argv);
    if (opt.done) {
        return 0;
    }
    if (!opt.ok) {
        return 2;
    }

    // THE FILES ARE READ BEFORE ANYTHING IS BUILT, and a malformed one is fatal here
    // rather than half-applied later. A person who wrote a file is owed the parse error
    // at the moment they started the host.
    std::string error;
    loom::host::AuthorityStore store;
    if (!store.open(opt.authority, &error)) {
        std::cerr << "loom-host: " << error << '\n';
        return 3;
    }
    loom::host::BootPlan plan;
    if (!loom::host::read_boot_plan(opt.boot_plan, &plan, &error)) {
        std::cerr << "loom-host: " << error << '\n';
        return 3;
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
    session.boot(plan);
    for (const std::string& line : session.report().render()) {
        std::cout << line << '\n';
    }
    print_notes(store);
    if (!session.report().complete()) {
        // The console is the recovery surface, so say that it is one. A host that
        // exited here would leave a person with nothing to diagnose from.
        std::cout << "  the boot did not complete. The console is up: 'status' for the table, "
                     "'authority pending' for what is waiting on you.\n";
    }
    std::cout << "type 'help' for commands, 'quit' to shut this host down.\n";

    std::string line;
    while (std::cout << "loom> " && std::getline(std::cin, line)) {
        const std::vector<Token> tok = tokenize(line);
        if (tok.empty()) {
            continue;
        }
        const std::string& cmd = tok[0].text;
        if (cmd == "quit" || cmd == "exit") {
            break; // HOST shutdown; see the file header
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "status") {
            cmd_status(session, plan, store);
        } else if (cmd == "authority") {
            cmd_authority(session, store, tok);
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
                continue;
            }
            auto d = session.console().describe(tok[1].text, static_cast<std::uint32_t>(v));
            if (!d) {
                std::cout << "  no such registered shape\n";
                continue;
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
                continue;
            }
            std::vector<loom::Arg> args;
            for (std::size_t i = 4; i < tok.size(); ++i) {
                args.push_back(lex_arg(tok[i]));
            }
            const std::uint64_t before =
                session.console().evicted().buffer + session.console().buffer_size();
            const loom::Composed c = session.console().compose(
                loom::WeaveId{id}, tok[2].text, static_cast<std::uint32_t>(v), args);
            if (c.status == loom::Composed::Status::Error) {
                std::cout << "  compose error: " << c.error << '\n';
                continue;
            }
            if (c.status == loom::Composed::Status::NeedsInput) {
                std::cout << "  needs input — name the fields with field=value:\n";
                for (const loom::FieldDesc& f : c.open_fields) {
                    std::cout << "    " << f.name << " : " << f.type << '\n';
                }
                continue;
            }
            session.console().pump();
            const loom::SendOutcome o = session.console().outcome(c.ticket);
            if (o.refused) {
                std::cout << "  refused: " << o.reason << '\n';
            } else {
                std::cout << "  sent.";
                const std::uint64_t after =
                    session.console().evicted().buffer + session.console().buffer_size();
                if (after > before) {
                    std::cout << "  reply -> m" << after;
                }
                std::cout << '\n';
            }
        } else if (cmd == "buffer") {
            const std::size_t n = session.console().buffer_size();
            const std::uint64_t gone = session.console().evicted().buffer;
            for (std::uint64_t label = gone + 1; label <= gone + n; ++label) {
                if (auto e = session.console().buffer_at(static_cast<std::size_t>(label))) {
                    std::cout << "  " << e->label << " : " << e->name << " v" << e->version << "  "
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
                continue;
            }
            auto e = session.console().buffer_at(static_cast<std::size_t>(n));
            if (!e) {
                std::cout << "  no such reply (it may have been evicted)\n";
                continue;
            }
            std::cout << "  " << e->label << " : " << e->name << " v" << e->version << "  "
                      << HostSession::describe_reply(*e) << '\n';
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
    }
    return 0;
}
