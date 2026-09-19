// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_RUN_MANAGER_HPP
#define ZEN_RUNS_RUN_MANAGER_HPP

// THE RUN MANAGER: named runs of editable tools, owned while the host that loaded it lives.
//
// An ordinary loadable weave (`loom-runs`, exported by runs_weave.cpp). A session host boots it
// like any artifact, the operator approves it like any artifact, and it holds the office
// `loom.runs` (zen/runs/vocabulary.hpp says what it answers). It is task policy, not host
// authority: it cannot admit anybody to the bus. For each run it asks the session door to expect
// one worker connection -- the credential's SHA-256 and the rules to grant, which the door
// attenuates to this manager's OWN approved authority -- and only once the door has agreed does it
// start the worker process. The worker is then its own session, the actor of its own asks.
//
// WHAT IT OWNS, AND WHERE IT KEEPS IT:
//   in memory   every run of this lifetime: its view (`Run`), its process, its standing control
//               question, the Start answer it still owes a client
//   on disk     each run's directory, `runs/<lifetime prefix>-<name>/`: the package SNAPSHOT the
//               worker executes, the request, the worker's output, `out/` for its artifacts, and
//               `run.json` -- the run's record, rewritten at every change and read back through
//               the gate by `Past`. A record is evidence of what happened, never live state.
//
// WHAT IT NEVER DOES: resume anything across a host restart, resend anything a worker asked, or
// believe a report it cannot attribute. A report counts only from the session the door admitted
// for that run (the bus stamps the sender); a run's name in a payload is data. A connection
// notice counts only from the door that answered this manager's own registration.
//
// LIFETIMES. The host lifetime comes from the door's own published record (`session.json`,
// written before anything booted); every handle names it, and a handle from another lifetime is
// refused by name, never looked up as if it were this one's.

#include "catalog.hpp"
#include "process.hpp"

#include "../detail/json.hpp"
#include "../host/secure_random.hpp"

#include <zen/runs/vocabulary.hpp>
#include <zen/serialize.hpp>
#include <zen/session/vocabulary.hpp>
#include <zen/weave.hpp>
#include <zen/weave/ask_book.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace loom::runs {

struct RunManagerState {
    std::int64_t started = 0;   ///< runs whose worker process started
    std::int64_t refused = 0;   ///< Start requests refused
    std::int64_t finished = 0;  ///< runs that reached a final state
    std::int64_t ignored = 0;   ///< arrivals that moved nothing: unattributable reports, stray words
    ZEN_SHAPE(RunManagerState, 1, ZEN_FIELD(started), ZEN_FIELD(refused), ZEN_FIELD(finished),
              ZEN_FIELD(ignored));
};

struct RunManagerConfig {
    std::string catalog = "loom-tools.json";
    std::string session_file = "session.json";
    std::string runs_dir = "runs";
    std::string runtime;      ///< where `loom_session` lives; empty: the catalog's, else beside
                              ///< this artifact (`<its directory>/python`)
    std::string image_dir;    ///< this artifact's directory, when known
};

class RunManager final
    : public WeaveBase<RunManager, RunManagerState,
                       Accept<ListTools, DescribeTool, Start, List, Get, Cancel, Release, Past,
                              WorkerStarted, Progress, AskReport, Produced, Finished, Control,
                              session::RunExpected, session::RunConnection, Ack, Refused>,
                       Emit<Tools, ToolDescription, Run, RunList, Directive, session::ExpectRun,
                            session::ForgetRun, Ack, Refused>> {
public:
    static constexpr std::size_t kMaxRuns = 64;      ///< runs held in this lifetime, finished or not
    static constexpr std::size_t kMaxActive = 8;     ///< runs not yet final
    static constexpr std::size_t kMaxNotes = 32;     ///< notes per run; older ones are counted, dropped
    static constexpr std::size_t kMaxAsks = 128;     ///< asks per run the record keeps
    static constexpr std::size_t kMaxArtifacts = 32; ///< artifacts per run
    static constexpr std::size_t kMaxPast = 64;      ///< past records one answer carries

    RunManager() : book_(kMaxActive * 4) {}
    explicit RunManager(RunManagerConfig config) : config_(std::move(config)), book_(kMaxActive * 4) {}

    /// THE HOST IS ENDING (or this manager is being unloaded): every run still active ends with
    /// it, and its record says so. Nothing is resent and nothing is claimed about far work.
    ~RunManager() {
        for (auto& r : runs_) {
            if (final_state(r->view.state)) {
                continue;
            }
            const bool killed = r->process.terminate();
            const std::string was = r->view.state;
            r->view.state = "interrupted";
            r->view.failure = "the session host ended while this run was " + was +
                              "; what its worker had asked far away is not known to have finished";
            r->view.process = killed ? "killed" : (r->process.started() ? "exited" : "not-started");
            r->view.ended_ms = now_ms();
            write_record(*r);
        }
    }

    // ---- the catalog ---------------------------------------------------------------------------

    void on(const ListTools& q, Mail& mail) {
        const Catalog c = read_catalog(config_.catalog);
        Tools out;
        out.catalog = c.file;
        out.problems = c.problems;
        const std::vector<std::string> words = split_words(q.query);
        for (const Package& p : c.packages) {
            if (!p.problem.empty()) {
                continue;
            }
            for (const ToolSpec& t : p.tools) {
                const std::string id = p.name + "/" + t.name;
                std::string hay = lower(id + " " + t.summary + " " + t.description + " " + p.summary);
                for (const std::string& term : t.terms) {
                    hay += " " + lower(term);
                }
                bool all = true;
                for (const std::string& w : words) {
                    all = all && hay.find(w) != std::string::npos;
                }
                if (!all) {
                    continue;
                }
                Tool row;
                row.id = id;
                row.package = p.name;
                row.name = t.name;
                row.summary = t.summary;
                std::string why;
                row.approved = approved(p, p.revision, &why);
                row.approval = approval_text(p, row.approved, why);
                row.revision = p.revision;
                out.rows.push_back(std::move(row));
            }
        }
        (void)mail.answer(out);
    }

    void on(const DescribeTool& q, Mail& mail) {
        const Catalog c = read_catalog(config_.catalog);
        const ToolSpec* t = nullptr;
        const Package* p = c.find(q.tool, &t);
        if (p == nullptr) {
            (void)mail.answer(Refused{"no tool '" + q.tool + "' in the catalog " + c.file +
                                      " (ListTools lists them)"});
            return;
        }
        ToolDescription d;
        d.id = p->name + "/" + t->name;
        d.package = p->name;
        d.name = t->name;
        d.summary = t->summary;
        d.description = t->description;
        d.version = p->version;
        d.revision = p->revision;
        d.package_dir = p->dir;
        d.source = (std::filesystem::path(p->dir) / t->script).string();
        std::string why;
        d.approved = approved(*p, p->revision, &why);
        d.approval = approval_text(*p, d.approved, why);
        for (const InputSpec& in : t->inputs) {
            d.inputs.push_back(ToolInput{in.name, in.type, in.required, in.default_json, in.help});
        }
        for (const OutputSpec& o : t->outputs) {
            d.outputs.push_back(ToolOutput{o.name, o.help});
        }
        d.requires_ = t->requires_;
        d.asks = t->asks;
        d.vocabulary = t->vocabulary;
        d.example = t->example;
        d.recovery = t->recovery;
        (void)mail.answer(d);
    }

    // ---- starting a run ------------------------------------------------------------------------

    void on(const Start& s, Mail& mail) {
        reap_all(mail);
        std::string why;
        if (!know_session(&why)) {
            refuse(mail, why);
            return;
        }
        if (!valid_name(s.name, &why)) {
            refuse(mail, why);
            return;
        }
        const Catalog c = read_catalog(config_.catalog);
        const ToolSpec* tool = nullptr;
        const Package* pkg = c.find(s.tool, &tool);
        if (pkg == nullptr) {
            refuse(mail, "no tool '" + s.tool + "' in the catalog " + c.file);
            return;
        }
        const std::string inputs = check_inputs(*tool, s.inputs, &why);
        if (inputs.empty()) {
            refuse(mail, "run '" + s.name + "': " + why);
            return;
        }
        // A NAME IS ONE RUN FOR THE WHOLE LIFETIME. The same request again is the same run -- a
        // client whose answer was lost asks again and is told what became of it -- and anything
        // else under that name is refused rather than silently started as a second run.
        if (RunRecord* existing = find(s.name)) {
            if (existing->view.tool == s.tool && existing->view.inputs == inputs) {
                if (existing->start_answer.valid()) {
                    refuse(mail, "run '" + s.name + "' is being started for another request; ask "
                                 "for it by name in a moment");
                    return;
                }
                (void)mail.answer(existing->view);
                return;
            }
            refuse(mail, "run name '" + s.name + "' is already used in this lifetime by " +
                             existing->view.tool + " with other inputs; choose another name");
            return;
        }
        if (active() >= kMaxActive) {
            refuse(mail, std::to_string(kMaxActive) + " runs are already active; wait for one to "
                         "finish, or cancel one");
            return;
        }
        if (runs_.size() >= kMaxRuns) {
            refuse(mail, "this manager holds " + std::to_string(kMaxRuns) +
                             " runs; release finished ones first (their directories stay)");
            return;
        }
        const std::string python = !c.python.empty() ? c.python : default_python();
        if (python.empty()) {
            refuse(mail, "no Python interpreter was found on PATH; name one in the catalog: "
                         "\"python\": \"<path to python 3.8 or newer>\"");
            return;
        }
        const std::string runtime = runtime_dir(c);
        std::error_code ec;
        if (runtime.empty() ||
            !std::filesystem::is_regular_file(std::filesystem::path(runtime) / "loom_session" /
                                                  "worker.py",
                                              ec)) {
            refuse(mail, "cannot find the loom_session runtime (looked in '" + runtime +
                             "'); name its directory in the catalog: \"runtime\": \"<dir>\"");
            return;
        }
        // THE RUN'S OWN DIRECTORY, never written over: a snapshot of the package, the request,
        // the worker's output and its outputs.
        const std::filesystem::path dir =
            std::filesystem::absolute(config_.runs_dir, ec) / (lifetime_.substr(0, 8) + "-" + s.name);
        if (std::filesystem::exists(dir, ec)) {
            refuse(mail, dir.string() + " already exists; a run never writes over another's "
                                        "directory");
            return;
        }
        std::filesystem::create_directories(dir / "out", ec);
        if (ec) {
            refuse(mail, "cannot create " + dir.string() + ": " + ec.message());
            return;
        }
        const std::filesystem::path snap = dir / "package";
        std::string revision;
        if (!snapshot_package(pkg->dir, snap, &why) ||
            (revision = package_revision(snap, &why)).empty() || !approved(*pkg, revision, &why)) {
            std::filesystem::remove_all(dir, ec);
            refuse(mail, "run '" + s.name + "': " + why);
            return;
        }
        const std::string credential = loom::host::secure_random_hex(32);
        if (credential.empty()) {
            std::filesystem::remove_all(dir, ec);
            refuse(mail, "the operating system would not supply randomness for the run's credential");
            return;
        }
        auto r = std::make_unique<RunRecord>();
        r->view.lifetime = lifetime_;
        r->view.name = s.name;
        r->view.tool = s.tool;
        r->view.revision = revision;
        r->view.snapshot = snap.string();
        r->view.inputs = inputs;
        r->view.directory = dir.string();
        r->view.state = "starting";
        r->view.process = "not-started";
        r->view.started_ms = now_ms();
        r->view.log = (dir / "worker.log").string();
        r->python = python;
        r->runtime = runtime;
        r->script = tool->script;
        if (!write_private(dir / "credential", credential, &why) ||
            !write_request(*r, *pkg, *tool, &why)) {
            std::filesystem::remove_all(dir, ec);
            refuse(mail, "run '" + s.name + "': " + why);
            return;
        }
        // THE DOOR DECIDES WHAT THE WORKER MAY SAY: its reports to this office, and what the tool
        // asks for -- each rule contained in this manager's own approved authority, or refused.
        session::ExpectRun expect;
        expect.run = s.name;
        expect.digest = loom::host::credential_digest(credential);
        for (const std::string& shape : worker_report_shapes()) {
            expect.may_say.push_back(shape + " v1 -> role " + kRunsRole);
        }
        for (const std::string& rule : tool->asks) {
            expect.may_say.push_back(rule);
        }
        const AskOpened opened = book_.open_to_role(session::kSessionRole,
                                                    session::ExpectRun::zen_name, 1);
        if (!opened.ok) {
            std::filesystem::remove_all(dir, ec);
            refuse(mail, "this manager is waiting on the door for too many runs; ask again");
            return;
        }
        r->expect_corr = opened.correlation;
        r->start_answer = mail.defer_answer();
        if (!r->start_answer.valid()) {
            (void)book_.forget(opened.id);
            std::filesystem::remove_all(dir, ec);
            refuse(mail, "this delivery cannot wait for the door's answer");
            return;
        }
        (void)mail.send_to_role(session::kSessionRole, expect, opened.correlation);
        write_record(*r);
        runs_.push_back(std::move(r));
    }

    /// THE DOOR AGREED: start the worker, and answer the client that asked.
    void on(const session::RunExpected& e, Mail& mail) {
        std::optional<PendingAsk> asked = settled(mail);
        if (!asked.has_value()) {
            return;
        }
        RunRecord* r = find_expecting(mail.correlation());
        if (r == nullptr) {
            ++state_.ignored;
            return;
        }
        r->expect_corr = 0;
        r->granted = e.granted;
        SpawnSpec spec;
        spec.program = r->python;
        spec.args = {"-X", "utf8", "-u", "-m", "loom_session.worker"};
        spec.cwd = r->view.directory;
        spec.output = r->view.log;
        std::string pythonpath = r->runtime;
        if (const std::string existing = environment_value("PYTHONPATH"); !existing.empty()) {
#ifdef _WIN32
            pythonpath += ";";
#else
            pythonpath += ":";
#endif
            pythonpath += existing;
        }
        spec.env = {{"PYTHONPATH", pythonpath},
                    {"PYTHONDONTWRITEBYTECODE", "1"},
                    {"LOOM_SESSION_ENDPOINT", endpoint_},
                    {"LOOM_SESSION_LIFETIME", lifetime_},
                    {"LOOM_RUN_NAME", r->view.name},
                    {"LOOM_RUN_DIR", r->view.directory}};
        std::string why;
        if (!r->process.spawn(spec, &why)) {
            r->view.state = "error";
            r->view.failure = "the worker could not be started: " + why;
            r->view.ended_ms = now_ms();
            ++state_.finished;
            forget_at_door(*r, mail);
        } else {
            r->view.pid = r->process.pid();
            r->view.process = "running";
            ++state_.started;
            note(*r, "worker started (pid " + std::to_string(r->view.pid) + ") with " +
                         std::to_string(e.granted.size()) + " granted rule(s)");
        }
        write_record(*r);
        spend_start(*r, mail, nullptr);
    }

    /// THE DOOR REFUSED A REGISTRATION (or answered a ForgetRun with a refusal).
    void on(const Refused& no, Mail& mail) {
        std::optional<PendingAsk> asked = settled(mail);
        if (!asked.has_value()) {
            return;
        }
        RunRecord* r = find_expecting(mail.correlation());
        if (r == nullptr) {
            return; // a ForgetRun the door did not need: nothing to undo
        }
        // NOTHING RAN: the name is not used, and the directory the attempt made is removed.
        std::error_code ec;
        std::filesystem::remove_all(r->view.directory, ec);
        const Refused answer{"the session door refused run '" + r->view.name + "': " + no.reason};
        spend_start(*r, mail, &answer);
        erase(r);
        ++state_.refused;
    }

    void on(const Ack&, Mail& mail) { (void)settled(mail); }

    /// A WORKER'S CONNECTION CAME OR WENT -- believed only from the door that answered this
    /// manager's own registrations.
    void on(const session::RunConnection& n, Mail& mail) {
        if (!door_.valid() || mail.sender() != door_) {
            ++state_.ignored;
            return;
        }
        RunRecord* r = find(n.run);
        if (r == nullptr) {
            ++state_.ignored;
            return;
        }
        if (n.state == "admitted") {
            r->session = static_cast<std::uint64_t>(n.session);
            r->view.session = n.session;
            r->view.established = std::string(session::kRunSessionPrefix) + r->view.name;
            if (r->view.state == "starting") {
                r->view.state = "running";
            }
            note(*r, "worker admitted as session " + std::to_string(n.session));
        } else if (n.state == "closed") {
            r->closed = true;
            r->control = DeferredAnswer{}; // nobody left to answer
            note(*r, "worker connection ended");
            reap(*r, mail);
        }
        write_record(*r);
    }

    // ---- what a worker reports (attributed by the bus's stamp, never by a name) ----------------

    void on(const WorkerStarted& w, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        note(*r, "worker up under " + w.python);
        if (w.revision != r->view.revision) {
            note(*r, "WORKER REPORTS REVISION " + w.revision + ", not the snapshot's " +
                         r->view.revision);
        }
        (void)mail.answer(Ack{});
        write_record(*r);
    }

    void on(const Progress& p, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        if (!p.step.empty()) {
            r->view.step = p.step;
        }
        r->view.pending = p.pending;
        if (!p.note.empty()) {
            note(*r, p.note);
        }
        (void)mail.answer(Ack{});
        write_record(*r);
    }

    void on(const AskReport& a, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        auto it = std::find_if(r->view.asks.begin(), r->view.asks.end(),
                               [&](const Asked& x) { return x.correlation == a.ask.correlation; });
        if (it != r->view.asks.end()) {
            *it = a.ask;
        } else {
            if (r->view.asks.size() >= kMaxAsks) {
                r->view.asks.erase(r->view.asks.begin());
                ++r->view.asks_dropped;
            }
            r->view.asks.push_back(a.ask);
        }
        (void)mail.answer(Ack{});
    }

    /// A FILE THE TOOL SAYS IT MADE, checked here on disk before it is listed as verified.
    void on(const Produced& p, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        Artifact a;
        a.name = p.name;
        a.path = p.path;
        a.bytes = p.bytes;
        a.sha256 = p.sha256;
        a.state = verify_artifact(*r, p);
        auto it = std::find_if(r->view.artifacts.begin(), r->view.artifacts.end(),
                               [&](const Artifact& x) { return x.name == p.name; });
        if (it != r->view.artifacts.end()) {
            *it = a;
        } else if (r->view.artifacts.size() < kMaxArtifacts) {
            r->view.artifacts.push_back(a);
        } else {
            note(*r, "artifact '" + p.name + "' not listed: this run already lists " +
                         std::to_string(kMaxArtifacts));
        }
        (void)mail.answer(Ack{});
        write_record(*r);
    }

    void on(const Finished& f, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        const std::string o = f.outcome;
        r->view.state = (o == "passed" || o == "failed" || o == "error" || o == "cancelled") ? o
                                                                                            : "error";
        r->view.summary = f.summary;
        r->view.failure = f.failure;
        r->view.pending.clear();
        r->view.ended_ms = now_ms();
        r->finished = true;
        ++state_.finished;
        (void)mail.answer(Ack{});
        if (r->control.valid()) {
            DeferredAnswer due = std::move(r->control);
            r->control = DeferredAnswer{};
            (void)answer_deferred(due, mail, Directive{"finish", "the run's verdict is recorded"});
        }
        write_record(*r);
    }

    /// THE WORKER'S STANDING QUESTION. Held, and answered once: `cancel` or `finish`.
    void on(const Control&, Mail& mail) {
        RunRecord* r = worker(mail);
        if (r == nullptr) {
            return;
        }
        if (r->view.cancel_requested) {
            (void)mail.answer(Directive{"cancel", r->view.cancel_reason});
            return;
        }
        if (r->control.valid()) {
            (void)mail.answer(Refused{"this run already has a standing control question"});
            return;
        }
        r->control = mail.defer_answer();
    }

    // ---- reading and ending runs ---------------------------------------------------------------

    void on(const List&, Mail& mail) {
        reap_all(mail);
        std::string why;
        (void)know_session(&why);
        RunList out;
        out.lifetime = lifetime_;
        out.capacity = static_cast<std::int64_t>(kMaxRuns);
        out.active = static_cast<std::int64_t>(active());
        for (const auto& r : runs_) {
            out.rows.push_back(r->view);
        }
        (void)mail.answer(out);
    }

    void on(const Get& g, Mail& mail) {
        reap_all(mail);
        RunRecord* r = by_handle(g.lifetime, g.name, mail);
        if (r != nullptr) {
            (void)mail.answer(r->view);
        }
    }

    /// A CANCELLATION IS A REQUEST. The worker is told through its standing question and may
    /// clean up; the run is `cancelled` once its process has ended. `force` ends it now.
    void on(const Cancel& c, Mail& mail) {
        reap_all(mail);
        RunRecord* r = by_handle(c.lifetime, c.name, mail);
        if (r == nullptr) {
            return;
        }
        if (!final_state(r->view.state)) {
            r->view.cancel_requested = true;
            r->view.cancel_reason = c.reason.empty() ? std::string("a client asked") : c.reason;
            note(*r, std::string(c.force ? "FORCED " : "") + "cancellation requested: " +
                         r->view.cancel_reason);
            if (c.force || !r->session) {
                if (r->process.terminate()) {
                    r->view.process = "killed";
                }
            } else if (r->control.valid()) {
                DeferredAnswer due = std::move(r->control);
                r->control = DeferredAnswer{};
                (void)answer_deferred(due, mail, Directive{"cancel", r->view.cancel_reason});
            }
            reap(*r, mail);
            write_record(*r);
        }
        (void)mail.answer(r->view);
    }

    void on(const Release& rel, Mail& mail) {
        reap_all(mail);
        RunRecord* r = by_handle(rel.lifetime, rel.name, mail);
        if (r == nullptr) {
            return;
        }
        if (!final_state(r->view.state)) {
            (void)mail.answer(Refused{"run '" + rel.name + "' is " + r->view.state +
                                      "; cancel it or let it finish before releasing it"});
            return;
        }
        forget_at_door(*r, mail);
        if (rel.remove) {
            std::error_code ec;
            const std::filesystem::path root = std::filesystem::absolute(config_.runs_dir, ec);
            const std::filesystem::path dir = std::filesystem::path(r->view.directory);
            if (dir.parent_path() == root) {
                std::filesystem::remove_all(dir, ec);
            }
        }
        erase(r);
        (void)mail.answer(Ack{});
    }

    /// RECORDS OF OTHER LIFETIMES in this session directory: evidence, never live state.
    void on(const Past&, Mail& mail) {
        std::string why;
        (void)know_session(&why);
        RunList out;
        out.lifetime = lifetime_;
        out.capacity = static_cast<std::int64_t>(kMaxPast);
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        for (std::filesystem::directory_iterator it(config_.runs_dir, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::filesystem::path record = it->path() / "run.json";
            if (std::filesystem::is_regular_file(record, ec)) {
                files.emplace_back(std::filesystem::last_write_time(record, ec), record);
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [when, record] : files) {
            (void)when;
            std::optional<Run> past = read_record(record);
            if (!past.has_value() || past->lifetime == lifetime_) {
                continue;
            }
            past->live = false;
            out.rows.push_back(std::move(*past));
            if (out.rows.size() >= kMaxPast) {
                break;
            }
        }
        (void)mail.answer(out);
    }

    /// A run's record as this manager wrote it (`run.json`), read back through the gate.
    static std::optional<Run> read_record(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        Admission a = admit(compat::parse(ss.str()), schema_of<Run>());
        if (!a.ok()) {
            return std::nullopt;
        }
        return from_value<Run>(a.value());
    }

private:
    struct RunRecord {
        Run view;
        ChildProcess process;
        std::string python;
        std::string runtime;
        std::string script;
        std::vector<std::string> granted;
        std::uint64_t expect_corr = 0; ///< the door registration still unanswered
        DeferredAnswer start_answer;   ///< the client's Start, until the door answers
        DeferredAnswer control;        ///< the worker's standing question
        std::uint64_t session = 0;     ///< the worker's session, once admitted
        bool closed = false;
        bool finished = false;
        bool forgotten = false;        ///< the door was told to forget it
    };

    static bool final_state(const std::string& s) {
        return s == "passed" || s == "failed" || s == "error" || s == "cancelled" ||
               s == "crashed" || s == "interrupted";
    }

    static std::int64_t now_ms() {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    static std::string lower(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    }

    static std::vector<std::string> split_words(const std::string& q) {
        std::vector<std::string> out;
        std::string w;
        for (const char c : lower(q) + " ") {
            if (c == ' ' || c == '\t') {
                if (!w.empty()) {
                    out.push_back(w);
                }
                w.clear();
            } else {
                w.push_back(c);
            }
        }
        return out;
    }

    static std::string approval_text(const Package& p, bool ok, const std::string& why) {
        if (!ok) {
            return "not approved: " + why;
        }
        return p.approve == "any-revision" ? std::string("any revision")
                                           : "revision " + p.approve.substr(0, 12);
    }

    static bool valid_name(const std::string& name, std::string* why) {
        if (name.empty() || name.size() > 64) {
            *why = "a run name is 1..64 characters";
            return false;
        }
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            if (!ok) {
                *why = "a run name uses only letters, digits, '-', '_' and '.'";
                return false;
            }
        }
        return true;
    }

    std::string default_python() const {
#ifdef _WIN32
        std::string p = ChildProcess::find_on_path("python");
#else
        std::string p = ChildProcess::find_on_path("python3");
        if (p.empty()) {
            p = ChildProcess::find_on_path("python");
        }
#endif
        return p;
    }

    std::string runtime_dir(const Catalog& c) const {
        if (!config_.runtime.empty()) {
            return config_.runtime;
        }
        if (!c.runtime.empty()) {
            return c.runtime;
        }
        const std::string image = !config_.image_dir.empty()
                                      ? config_.image_dir
                                      : image_directory_of(reinterpret_cast<const void*>(&now_ms));
        return image.empty() ? std::string() : (std::filesystem::path(image) / "python").string();
    }

    /// The lifetime and endpoint, from the door's own published record in this directory.
    bool know_session(std::string* why) {
        if (!lifetime_.empty()) {
            return true;
        }
        std::ifstream in(config_.session_file, std::ios::binary);
        if (!in) {
            *why = "this host is not serving a session (no " + config_.session_file +
                   " here); a run manager starts workers only in a session host "
                   "(loom-host --serve <dir>)";
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const loom::detail::JsonParse parsed = loom::detail::parse_json(ss.str(), 8);
        const loom::detail::JsonValue* lifetime =
            parsed.ok ? parsed.value.find("lifetime") : nullptr;
        const loom::detail::JsonValue* endpoint =
            parsed.ok ? parsed.value.find("endpoint") : nullptr;
        if (lifetime == nullptr || endpoint == nullptr) {
            *why = config_.session_file + " does not read as a session record";
            return false;
        }
        lifetime_ = lifetime->text;
        endpoint_ = endpoint->text;
        return true;
    }

    RunRecord* find(const std::string& name) {
        for (auto& r : runs_) {
            if (r->view.name == name) {
                return r.get();
            }
        }
        return nullptr;
    }

    RunRecord* find_expecting(std::uint64_t correlation) {
        for (auto& r : runs_) {
            if (r->expect_corr != 0 && r->expect_corr == correlation) {
                return r.get();
            }
        }
        return nullptr;
    }

    void erase(RunRecord* r) {
        runs_.erase(std::remove_if(runs_.begin(), runs_.end(),
                                   [r](const std::unique_ptr<RunRecord>& x) { return x.get() == r; }),
                    runs_.end());
    }

    std::size_t active() const {
        std::size_t n = 0;
        for (const auto& r : runs_) {
            n += final_state(r->view.state) ? 0u : 1u;
        }
        return n;
    }

    /// A handle names its lifetime; another lifetime's handle is refused by name.
    RunRecord* by_handle(const std::string& lifetime, const std::string& name, Mail& mail) {
        std::string why;
        if (!know_session(&why)) {
            (void)mail.answer(Refused{why});
            return nullptr;
        }
        if (lifetime != lifetime_) {
            (void)mail.answer(Refused{
                "run handle " + lifetime.substr(0, 12) + "/" + name +
                " belongs to another host lifetime; this session is lifetime " +
                lifetime_.substr(0, 12) + ". That lifetime has ended, and nothing here is its run: "
                "its record, if any, is listed by Past"});
            return nullptr;
        }
        RunRecord* r = find(name);
        if (r == nullptr) {
            (void)mail.answer(Refused{"no run named '" + name + "' in this lifetime"});
        }
        return r;
    }

    /// The run whose worker session sent this, or nothing (and the sender is told).
    RunRecord* worker(Mail& mail) {
        for (auto& r : runs_) {
            if (r->session != 0 && r->session == mail.sender().value) {
                return r.get();
            }
        }
        ++state_.ignored;
        (void)mail.answer(Refused{"this session is not the worker of any run this manager holds"});
        return nullptr;
    }

    /// Loom's answer to one of this manager's own conversations with the door, or nothing.
    std::optional<PendingAsk> settled(Mail& mail) {
        if (!mail.answers_ask()) {
            ++state_.ignored;
            return std::nullopt;
        }
        std::optional<PendingAsk> asked = book_.settle(mail.correlation(), mail.sender());
        if (!asked.has_value()) {
            ++state_.ignored;
            return std::nullopt;
        }
        if (!door_.valid()) {
            door_ = mail.sender(); // Loom attested this as the answer from `loom.session`
        }
        return asked;
    }

    void refuse(Mail& mail, std::string why) {
        ++state_.refused;
        (void)mail.answer(Refused{std::move(why)});
    }

    void spend_start(RunRecord& r, Mail& mail, const Refused* refusal) {
        if (!r.start_answer.valid()) {
            return;
        }
        DeferredAnswer due = std::move(r.start_answer);
        r.start_answer = DeferredAnswer{};
        if (refusal != nullptr) {
            (void)answer_deferred(due, mail, *refusal);
        } else {
            (void)answer_deferred(due, mail, r.view);
        }
    }

    void forget_at_door(RunRecord& r, Mail& mail) {
        if (r.forgotten) {
            return;
        }
        r.forgotten = true;
        const AskOpened opened = book_.open_to_role(session::kSessionRole,
                                                    session::ForgetRun::zen_name, 1);
        if (opened.ok) {
            (void)mail.send_to_role(session::kSessionRole, session::ForgetRun{r.view.name},
                                    opened.correlation);
        }
    }

    void note(RunRecord& r, const std::string& text) {
        if (r.view.notes.size() >= kMaxNotes) {
            r.view.notes.erase(r.view.notes.begin());
            ++r.view.notes_dropped;
        }
        r.view.notes.push_back(text);
    }

    /// Has the worker process ended? If so, the run's final state -- the tool's own verdict when
    /// it gave one, `cancelled` when a cancellation was asked for, `crashed` otherwise.
    void reap(RunRecord& r, Mail& mail) {
        if (!r.process.started() || !r.process.ended()) {
            return;
        }
        if (r.view.process == "running" || r.view.process == "killed") {
            r.view.process = r.view.process == "killed" ? "killed" : "exited";
            r.view.exit_code = r.process.exit_code();
        }
        if (!final_state(r.view.state)) {
            r.view.pending.clear();
            r.view.ended_ms = now_ms();
            ++state_.finished;
            if (r.view.cancel_requested) {
                r.view.state = "cancelled";
                if (r.view.failure.empty()) {
                    r.view.failure = "cancelled: " + r.view.cancel_reason;
                }
            } else {
                r.view.state = "crashed";
                r.view.failure = "the worker ended (exit code " +
                                 std::to_string(r.process.exit_code()) +
                                 ") without a verdict; its output ends: " + tail(r.view.log);
            }
        }
        r.control = DeferredAnswer{};
        forget_at_door(r, mail);
        write_record(r);
    }

    void reap_all(Mail& mail) {
        for (auto& r : runs_) {
            if (!r->forgotten || !final_state(r->view.state)) {
                reap(*r, mail);
            }
        }
    }

    static std::string tail(const std::string& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return "(no output)";
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string s = ss.str();
        if (s.size() > 600) {
            s = "..." + s.substr(s.size() - 600);
        }
        return s.empty() ? std::string("(no output)") : s;
    }

    std::string verify_artifact(const RunRecord& r, const Produced& p) {
        std::error_code ec;
        const std::filesystem::path out =
            std::filesystem::weakly_canonical(std::filesystem::path(r.view.directory) / "out", ec);
        const std::filesystem::path file = std::filesystem::weakly_canonical(p.path, ec);
        const std::string o = out.string();
        const std::string f = file.string();
        if (ec || f.size() <= o.size() || f.compare(0, o.size(), o) != 0) {
            return "outside";
        }
        if (!std::filesystem::is_regular_file(file, ec)) {
            return "missing";
        }
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec || static_cast<std::int64_t>(size) != p.bytes) {
            return "size-mismatch";
        }
        return file_sha256(file) == p.sha256 ? "verified" : "digest-mismatch";
    }

    static bool write_private(const std::filesystem::path& file, const std::string& text,
                              std::string* why) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            *why = "cannot write " + file.string();
            return false;
        }
        out << text;
        out.close();
        std::error_code ec;
        std::filesystem::permissions(file,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        return !out.fail();
    }

    bool write_request(const RunRecord& r, const Package& p, const ToolSpec& t, std::string* why) {
        std::string json = "{";
        const auto field = [&json](const char* k, const std::string& v, bool last = false) {
            loom::detail::json_quote(k, json);
            json += ":";
            loom::detail::json_quote(v, json);
            json += last ? "" : ",";
        };
        field("lifetime", r.view.lifetime);
        field("name", r.view.name);
        field("tool", r.view.tool);
        field("package", p.name);
        field("script", t.script);
        field("revision", r.view.revision);
        field("snapshot", r.view.snapshot);
        field("out", (std::filesystem::path(r.view.directory) / "out").string());
        loom::detail::json_quote("inputs", json);
        json += ":" + r.view.inputs + "}";
        std::ofstream out(std::filesystem::path(r.view.directory) / "request.json",
                          std::ios::binary | std::ios::trunc);
        if (!out) {
            *why = "cannot write the run's request";
            return false;
        }
        out << json;
        out.close();
        return !out.fail();
    }

    /// The run's record, rewritten whole by way of a temp file.
    static void write_record(const RunRecord& r) {
        const std::filesystem::path file = std::filesystem::path(r.view.directory) / "run.json";
        const std::filesystem::path tmp = file.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return;
            }
            out << compat::serialize(to_value(r.view));
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
    }

    RunManagerConfig config_;
    std::vector<std::unique_ptr<RunRecord>> runs_;
    AskBook book_;
    WeaveId door_{};
    std::string lifetime_;
    std::string endpoint_;
};

} // namespace loom::runs

#endif // ZEN_RUNS_RUN_MANAGER_HPP
