// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE RUN MANAGER AND ITS CATALOG, WITHOUT PYTHON (src/runs/). The catalog's strictness, the
// package revision and snapshot, approval and input checking are pure functions; the manager is
// registered natively on a bus beside a real session door and answers a native client. A worker
// that fails to start is a real process: `cmake` itself, handed the worker's arguments it does not
// understand, stands in for an interpreter that exits at once. The full journey with real Python
// workers is tests/session/journey.py.

#include <doctest.h>

#include "run_manager.hpp"
#include "session_door.hpp"

#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#endif

using namespace loom;
using namespace loom::runs;

namespace {

namespace fs = std::filesystem;

struct Scratch {
    fs::path path;
    explicit Scratch(const std::string& name) {
        path = fs::temp_directory_path() /
               ("zen-runs-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

const char* const kManifest = R"({
  "package": "demo", "version": "7", "summary": "a demo",
  "tools": [ { "name": "hello", "script": "hello.py", "summary": "says hello",
               "inputs": [ {"name": "who", "type": "text", "default": "world", "help": "whom"},
                           {"name": "times", "type": "int", "required": true, "help": "how often"},
                           {"name": "loud", "type": "bool", "default": false} ],
               "outputs": [ {"name": "hello.txt", "help": "the greeting"} ],
               "asks": [], "terms": ["greeting"] } ] })";

fs::path demo_package(const fs::path& root, const std::string& manifest = kManifest) {
    const fs::path dir = root / "demo";
    write(dir / "loom-tool.json", manifest);
    write(dir / "hello.py", "def run(ctx):\n    return 'hello'\n");
    return dir;
}

bool until(const std::function<bool()>& step, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (step()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_SUITE("runs") {

TEST_CASE("catalog: both files are read strictly -- an unknown key, a bad type, a bad approval is a problem, never ignored") {
    Scratch s("strict");
    const fs::path pkg = demo_package(s.path);
    write(s.path / "loom-tools.json",
          "{\"packages\": [{\"path\": \"demo\", \"approve\": \"any-revision\"}]}");
    Catalog c = read_catalog((s.path / "loom-tools.json").string());
    REQUIRE(c.problems.empty());
    REQUIRE(c.packages.size() == 1);
    CHECK(c.packages[0].tools[0].inputs.size() == 3);
    CHECK(c.packages[0].revision.size() == 64);

    write(s.path / "loom-tools.json",
          "{\"packages\": [{\"path\": \"demo\", \"approve\": \"yes please\"}], \"extra\": 1}");
    c = read_catalog((s.path / "loom-tools.json").string());
    REQUIRE_FALSE(c.problems.empty());
    CHECK(c.problems[0].find("'extra' is not a key") != std::string::npos);

    write(s.path / "loom-tools.json", "{\"packages\": [{\"path\": \"demo\", \"approve\": \"yes\"}]}");
    c = read_catalog((s.path / "loom-tools.json").string());
    REQUIRE(c.packages.size() == 1);
    CHECK(c.packages[0].approve.empty()); // a malformed approval approves nothing
    CHECK_FALSE(c.problems.empty());

    std::string bad = kManifest;
    bad.replace(bad.find("\"type\": \"int\""), 13, "\"type\": \"date\"");
    demo_package(s.path, bad);
    write(s.path / "loom-tools.json", "{\"packages\": [{\"path\": \"demo\"}]}");
    c = read_catalog((s.path / "loom-tools.json").string());
    REQUIRE(c.packages.size() == 1);
    CHECK(c.packages[0].problem.find("the types are text, int, bool and number") != std::string::npos);
    (void)pkg;
}

TEST_CASE("catalog: the revision covers every file but Python's caches; an edit is a new revision; a snapshot reproduces it") {
    Scratch s("revision");
    const fs::path pkg = demo_package(s.path);
    std::string why;
    const std::string r1 = package_revision(pkg, &why);
    REQUIRE(r1.size() == 64);
    write(pkg / "__pycache__" / "hello.cpython-313.pyc", "bytecode");
    write(pkg / "stale.pyc", "bytecode");
    CHECK(package_revision(pkg, &why) == r1);
    write(pkg / "hello.py", "def run(ctx):\n    return 'HELLO'\n");
    const std::string r2 = package_revision(pkg, &why);
    CHECK(r2 != r1);
    REQUIRE(snapshot_package(pkg, s.path / "snap", &why));
    CHECK(package_revision(s.path / "snap", &why) == r2);
    CHECK_FALSE(fs::exists(s.path / "snap" / "__pycache__"));
    CHECK_FALSE(snapshot_package(pkg, s.path / "snap", &why)); // never written over
    CHECK(why.find("never written over") != std::string::npos);
}

TEST_CASE("catalog: approval is the operator's word -- a pinned revision, any revision, or nothing, with the revision to approve named") {
    Package p;
    p.name = "demo";
    std::string why;
    CHECK_FALSE(approved(p, std::string(64, 'a'), &why));
    CHECK(why.find(std::string(64, 'a')) != std::string::npos);
    p.approve = std::string(64, 'a');
    CHECK(approved(p, std::string(64, 'a'), &why));
    CHECK_FALSE(approved(p, std::string(64, 'b'), &why));
    CHECK(why.find("changed since it was approved") != std::string::npos);
    p.approve = "any-revision";
    CHECK(approved(p, std::string(64, 'b'), &why));
}

TEST_CASE("inputs: unknown names and wrong types are refused; defaults fill in the declared order") {
    Scratch s("inputs");
    demo_package(s.path);
    write(s.path / "loom-tools.json", "{\"packages\": [{\"path\": \"demo\"}]}");
    const Catalog c = read_catalog((s.path / "loom-tools.json").string());
    const ToolSpec& t = c.packages[0].tools[0];
    std::string why;
    CHECK(check_inputs(t, "{\"times\": 2}", &why) == "{\"who\":\"world\",\"times\":2,\"loud\":false}");
    CHECK(check_inputs(t, "{}", &why).empty());
    CHECK(why.find("'times' is required") != std::string::npos);
    CHECK(check_inputs(t, "{\"times\": \"2\"}", &why).empty());
    CHECK(why.find("must be int") != std::string::npos);
    CHECK(check_inputs(t, "{\"times\": 2.5}", &why).empty());
    CHECK(check_inputs(t, "{\"times\": 1, \"when\": 3}", &why).empty());
    CHECK(why.find("'when' is not an input") != std::string::npos);
    CHECK(check_inputs(t, "[1]", &why).empty());
}

// ---- the manager, on a bus beside a real door --------------------------------------------------

namespace {

/// A native client: whatever the manager answers, kept by shape.
class RunsClient final
    : public WeaveBase<RunsClient, RunManagerState,
                       Accept<Tools, ToolDescription, Run, RunList, Refused, Ack>, Emit<>> {
public:
    void on(const Tools& t, Mail&) { tools = t; ++answers; }
    void on(const ToolDescription& d, Mail&) { description = d; ++answers; }
    void on(const Run& r, Mail&) { run = r; ++answers; }
    void on(const RunList& l, Mail&) { list = l; ++answers; }
    void on(const Refused& r, Mail&) { refused = r.reason; ++answers; }
    void on(const Ack&, Mail&) { ++acks; ++answers; }
    Tools tools;
    ToolDescription description;
    Run run;
    RunList list;
    std::string refused;
    int answers = 0;
    int acks = 0;
};

struct RunsHost {
    Scratch dir{"manager"};
    host::AuthorityStore store;
    Switchboard bus;
    host::HostWarden* warden = nullptr;
    host::SessionDoor* door = nullptr;
    RunManager* manager = nullptr;
    WeaveId manager_id{};
    RunsClient* client = nullptr;
    WeaveId client_id{};
    std::string lifetime = "0011223344556677889900aabbccddee";

    explicit RunsHost(bool serving = true, const std::string& python = ZEN_TEST_CMAKE) {
        std::string why;
        REQUIRE(store.open((dir.path / "loom-authority.json").string(), &why));
        auto w = std::make_unique<host::HostWarden>(store, WeaveId{999999});
        warden = w.get();
        warden->zen_set_self(bus.register_weave(std::move(w), host::warden_capability(WeaveId{999999})));
        host::SessionFacts facts;
        facts.lifetime = lifetime;
        auto d = std::make_unique<host::SessionDoor>(facts, std::string(64, 'k'), store, *warden);
        door = d.get();
        door->zen_set_self(bus.register_weave(std::move(d), Grant{}.allow_any(),
                                              std::string(session::kSessionRole)));
        REQUIRE(door->listen(bus, 0, &why));
        demo_package(dir.path);
        write(dir.path / "loom-tools.json",
              "{\"python\": \"" + fs::path(python).generic_string() +
                  "\", \"packages\": [{\"path\": \"demo\", \"approve\": \"any-revision\"}]}");
        if (serving) {
            write(dir.path / "session.json",
                  "{\"lifetime\": \"" + lifetime + "\", \"endpoint\": \"" + door->endpoint() + "\"}");
        }
        RunManagerConfig cfg;
        cfg.catalog = (dir.path / "loom-tools.json").string();
        cfg.session_file = (dir.path / "session.json").string();
        cfg.runs_dir = (dir.path / "runs").string();
        cfg.runtime = ZEN_TEST_PYTHON_RUNTIME;
        auto m = std::make_unique<RunManager>(cfg);
        manager = m.get();
        Grant g;
        g.allow_to_role(session::ExpectRun::zen_name, 1, session::kSessionRole);
        g.allow_to_role(session::ForgetRun::zen_name, 1, session::kSessionRole);
        for (const char* shape : {Tools::zen_name, ToolDescription::zen_name, Run::zen_name,
                                  RunList::zen_name, Directive::zen_name, Ack::zen_name,
                                  Refused::zen_name}) {
            g.allow_to_any(shape, 1);
        }
        manager_id = bus.register_weave(std::move(m), g, std::string(kRunsRole));
        manager->zen_set_self(manager_id);
        host::AuthorityRule rule;
        rule.artifact = "runs";
        rule.may_run = true;
        REQUIRE(store.put(rule, &why));
        warden->govern("runs", host_grant_authority(bus, manager_id, LiveAuthority{}.allow_any()));
        auto c = std::make_unique<RunsClient>();
        client = c.get();
        Grant cg;
        for (const std::string& shape : client_request_shapes()) {
            cg.allow_to_role(shape, 1, kRunsRole);
        }
        client_id = bus.register_weave(std::move(c), cg);
        client->zen_set_self(client_id);
    }
    void turn() {
        door->service();
        bus.pump_pending();
    }
    /// Ask the manager, as the client, and turn until it has answered.
    template <class T>
    void ask(const T& request) {
        const int before = client->answers;
        (void)bus.send_as_to_role(client_id, kRunsRole,
                                  Message(to_value(request), client_id, client_id, ++corr_));
        REQUIRE(until([&] { turn(); return client->answers > before; }));
    }

private:
    std::uint64_t corr_ = 0;
};

} // namespace

TEST_CASE("manager: with no session record here, a Start is refused and says what a session is") {
    RunsHost h(/*serving=*/false);
    h.ask(Start{"demo/hello", "a", "{\"times\": 1}"});
    CHECK(h.client->refused.find("not serving a session") != std::string::npos);
    CHECK_FALSE(fs::exists(h.dir.path / "runs"));
}

TEST_CASE("manager: an unknown tool, an unapproved package and bad inputs are refused before anything exists") {
    RunsHost h;
    h.ask(Start{"demo/nope", "a", "{}"});
    CHECK(h.client->refused.find("no tool 'demo/nope'") != std::string::npos);
    h.ask(Start{"demo/hello", "b", "{\"times\": \"x\"}"});
    CHECK(h.client->refused.find("must be int") != std::string::npos);
    h.ask(Start{"demo/hello", "bad name!", "{\"times\": 1}"});
    CHECK(h.client->refused.find("run name") != std::string::npos);
    write(h.dir.path / "loom-tools.json", "{\"packages\": [{\"path\": \"demo\"}]}");
    h.ask(Start{"demo/hello", "c", "{\"times\": 1}"});
    CHECK(h.client->refused.find("not approved to run") != std::string::npos);
    std::error_code ec;
    CHECK((!fs::exists(h.dir.path / "runs") || fs::is_empty(h.dir.path / "runs", ec)));
    h.ask(List{});
    CHECK(h.client->list.rows.empty());
}

TEST_CASE("manager: reading the catalog runs nothing, and describes a tool from its manifest") {
    RunsHost h;
    h.ask(ListTools{"greet"});
    REQUIRE(h.client->tools.rows.size() == 1);
    CHECK(h.client->tools.rows[0].id == "demo/hello");
    CHECK(h.client->tools.rows[0].approved);
    h.ask(ListTools{"no such words"});
    CHECK(h.client->tools.rows.empty());
    h.ask(DescribeTool{"demo/hello"});
    CHECK(h.client->description.inputs.size() == 3);
    CHECK(h.client->description.outputs[0].name == "hello.txt");
    CHECK(h.client->description.version == "7");
    CHECK_FALSE(fs::exists(h.dir.path / "runs"));
}

TEST_CASE("manager: a worker that ends without a verdict is CRASHED with its exit code; the same request by name is the same run") {
    RunsHost h;
    h.ask(Start{"demo/hello", "one", "{\"times\": 1}"});
    REQUIRE(h.client->refused.empty());
    CHECK(h.client->run.state == "starting");
    CHECK(h.client->run.revision.size() == 64);
    const fs::path dir = h.client->run.directory;
    CHECK(fs::exists(dir / "package" / "hello.py")); // the snapshot the worker would execute
    CHECK(fs::exists(dir / "run.json"));
    REQUIRE(until([&] {
        h.ask(Get{h.lifetime, "one"});
        return h.client->run.state == "crashed";
    }));
    CHECK(h.client->run.process == "exited");
    CHECK(h.client->run.exit_code != 0);
    CHECK(h.client->run.failure.find("without a verdict") != std::string::npos);
    CHECK(h.door->expectations() == 0); // the door forgot the connection it never saw
    // ...and the record on disk reads back through the gate.
    const std::optional<Run> on_disk = RunManager::read_record(dir / "run.json");
    REQUIRE(on_disk.has_value());
    CHECK(on_disk->state == "crashed");
    // A retried Start of the same request is the same run, not a second one.
    h.ask(Start{"demo/hello", "one", "{\"times\": 1}"});
    CHECK(h.client->run.directory == dir.string());
    h.ask(Start{"demo/hello", "one", "{\"times\": 2}"});
    CHECK(h.client->refused.find("already used") != std::string::npos);
    h.ask(List{});
    CHECK(h.client->list.rows.size() == 1);
}

TEST_CASE("manager: another lifetime's handle is refused by name; a stranger's report is refused and moves nothing") {
    RunsHost h;
    h.ask(Get{"ffffffffffffffffffffffffffffffff", "one"});
    CHECK(h.client->refused.find("another host lifetime") != std::string::npos);
    // A participant that is no run's worker, reporting a verdict: refused, counted, nothing moves.
    auto stranger = std::make_unique<RunsClient>();
    RunsClient* raw = stranger.get();
    Grant g;
    g.allow_to_role(Finished::zen_name, 1, kRunsRole);
    const WeaveId sid = h.bus.register_weave(std::move(stranger), g);
    raw->zen_set_self(sid);
    (void)h.bus.send_as_to_role(sid, kRunsRole, Message(to_value(Finished{"passed", "forged", ""}), sid, sid, 5));
    REQUIRE(until([&] { h.turn(); return raw->answers > 0; }));
    CHECK(raw->refused.find("not the worker of any run") != std::string::npos);
}

// ---- what this manager owns after the leader is gone, and what it never aims at ---------------

TEST_CASE("process: a live child is alive and stoppable; one that ended by itself owns nothing, "
          "so nothing later can be aimed at its number") {
    Scratch s{"process"};
    ChildProcess sleeper;
    SpawnSpec spec;
    spec.program = ZEN_TEST_CMAKE;
    spec.args = {"-E", "sleep", "30"};
    spec.cwd = s.path.string();
    spec.output = (s.path / "sleeper.log").string();
    std::string why;
    REQUIRE(sleeper.spawn(spec, &why));
    // An unrelated process of its own: nothing done to the first may reach the second.
    ChildProcess bystander;
    SpawnSpec other = spec;
    other.output = (s.path / "bystander.log").string();
    REQUIRE(bystander.spawn(other, &why));

    CHECK(sleeper.alive());
    CHECK_FALSE(sleeper.ended());
    CHECK(sleeper.terminate());
    // Waiting is this harness's own decision; that the execution ENDED is the claim, and it is
    // read from the operating system through both doors -- the group, and the leader.
    REQUIRE(until([&] { return !sleeper.alive() && sleeper.ended(); }));
    // Nothing is left to end -- and saying so is the point: a `true` here would mean this object
    // had signalled a number it no longer owns.
    CHECK_FALSE(sleeper.terminate());
    CHECK(bystander.alive());  // the other process was never this one's to touch

    ChildProcess brief;
    SpawnSpec quick = spec;
    quick.args = {"-E", "true"};
    quick.output = (s.path / "brief.log").string();
    REQUIRE(brief.spawn(quick, &why));
    REQUIRE(until([&] { return brief.ended(); }));
    CHECK_FALSE(brief.alive());      // it ended and left nothing behind
    CHECK_FALSE(brief.terminate());  // ...so there is nothing to stop, and nothing is signalled
    CHECK(bystander.alive());
    CHECK(bystander.terminate());
}

TEST_CASE("process: an exit code is an OBSERVATION -- unknown until one is read, and waited for "
          "within a bound when a final claim depends on it") {
    Scratch s{"observe"};
    ChildProcess sleeper;
    SpawnSpec spec;
    spec.program = ZEN_TEST_CMAKE;
    spec.args = {"-E", "sleep", "30"};
    spec.cwd = s.path.string();
    spec.output = (s.path / "sleeper.log").string();
    std::string why;
    REQUIRE(sleeper.spawn(spec, &why));
    // WHILE IT RUNS there is nothing to report: `exit_code()` is not a result, and a caller
    // about to write one down is told so rather than handed the -1 it holds.
    CHECK_FALSE(sleeper.ended());
    CHECK_FALSE(sleeper.exit_code_known());
    CHECK_FALSE(sleeper.wait_for_end(0));   // zero is one look and no wait
    CHECK(sleeper.alive());

    REQUIRE(sleeper.terminate());
    // ...and now a BOUNDED wait, which is the one thing this class waits for: a final claim
    // needs an observation, and this is where it is made.
    CHECK(sleeper.wait_for_end(5000));
    CHECK(sleeper.ended());
    REQUIRE(sleeper.exit_code_known());
#ifdef _WIN32
    CHECK(sleeper.exit_code() == 1);         // the job object's own termination code
#else
    CHECK(sleeper.exit_code() == 128 + 9);   // SIGKILL, spelled the way this class spells signals
#endif
    const int once = sleeper.exit_code();
    CHECK(sleeper.wait_for_end(0));          // it stays observed, and stays the same
    CHECK(sleeper.exit_code() == once);

    // A process that ends BY ITSELF is observed just the same, with its own code.
    ChildProcess brief;
    SpawnSpec quick = spec;
    quick.args = {"-E", "true"};
    quick.output = (s.path / "brief.log").string();
    REQUIRE(brief.spawn(quick, &why));
    CHECK(brief.wait_for_end(10000));
    CHECK(brief.exit_code_known());
    CHECK(brief.exit_code() == 0);

    // ...and one that was never started has nothing to observe and says so.
    ChildProcess never;
    CHECK_FALSE(never.wait_for_end(1000));
    CHECK_FALSE(never.exit_code_known());

#ifndef _WIN32
    // THE ONE CASE WHERE "IT ENDED" IS NOT "ITS CODE IS KNOWN": somebody else reaps the leader
    // first, so its status is gone before this object could read it. The end is a fact; the
    // code is not, and saying so is what keeps the -1 this object holds out of a record.
    ChildProcess stolen;
    SpawnSpec stolen_spec = spec;
    stolen_spec.args = {"-E", "true"};
    stolen_spec.output = (s.path / "stolen.log").string();
    REQUIRE(stolen.spawn(stolen_spec, &why));
    int status = 0;
    REQUIRE(::waitpid(static_cast<pid_t>(stolen.pid()), &status, 0) > 0);
    CHECK(stolen.ended());
    CHECK_FALSE(stolen.exit_code_known());
    CHECK_FALSE(stolen.alive());
#endif
}

// ---- the record: a save that fails is not a verdict, and an older record is still evidence ----

TEST_CASE("record: a save that cannot happen leaves the LAST VALID record exactly where it was, "
          "says why, and works again once the fault is gone") {
    Scratch s{"record"};
    const fs::path file = s.path / "run.json";
    const fs::path tmp = s.path / "run.json.tmp";
    Run first;
    first.lifetime = "0011223344556677889900aabbccddee";
    first.name = "one";
    first.state = "running";
    first.step = "the first record";
    first.record = "saved";
    std::string why;
    REQUIRE(RunManager::save_record(file, tmp, first, &why));
    const std::string valid = [&] {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }();
    REQUIRE_FALSE(valid.empty());
    CHECK_FALSE(fs::exists(tmp)); // the temporary does not outlive the write that used it

    // THE FAULT: the temporary's name is taken by something this manager did not put there.
    fs::create_directory(tmp);
    REQUIRE(fs::is_directory(tmp));
    Run second = first;
    second.state = "passed";
    second.step = "the second record";
    why.clear();
    CHECK_FALSE(RunManager::save_record(file, tmp, second, &why));
    CHECK(why.find(tmp.string()) != std::string::npos);
    CHECK(fs::is_directory(tmp)); // what somebody else put there is still theirs
    const std::optional<Run> kept = RunManager::read_record(file);
    REQUIRE(kept.has_value());
    CHECK(kept->step == "the first record"); // the last valid record, whole and readable
    CHECK(kept->state == "running");

    fs::remove(tmp);
    why.clear();
    CHECK(RunManager::save_record(file, tmp, second, &why));
    const std::optional<Run> now = RunManager::read_record(file);
    REQUIRE(now.has_value());
    CHECK(now->step == "the second record");
    CHECK(now->state == "passed");

    // AND THE WAY IS NEVER CLEARED FIRST. Something that is not this manager's stands where the
    // record would go: the replacement fails and leaves it exactly as it was. Removing the
    // destination before the new record is in place would both take what was there and leave
    // nothing behind when the replacement then failed.
    const fs::path taken = s.path / "taken.json";
    const fs::path taken_tmp = s.path / "taken.json.tmp";
    fs::create_directory(taken);
    why.clear();
    CHECK_FALSE(RunManager::save_record(taken, taken_tmp, second, &why));
    CHECK(fs::is_directory(taken));
    CHECK_FALSE(fs::exists(taken_tmp)); // nor is a temporary left lying beside it
    CHECK(why.find(taken.string()) != std::string::npos);
}

TEST_CASE("record: one written before this manager had a word for its own saving is still read, "
          "with that word's documented default -- and one short of anything else is refused") {
    Scratch s{"forward"};
    const fs::path file = s.path / "run.json";
    const fs::path tmp = s.path / "run.json.tmp";
    Run view;
    view.lifetime = "0011223344556677889900aabbccddee";
    view.name = "old";
    view.state = "passed";
    view.summary = "a run from an earlier manager";
    view.record = "saved";
    view.record_error = "";
    view.record_ms = 1234;
    std::string why;
    REQUIRE(RunManager::save_record(file, tmp, view, &why));
    std::string text = [&] {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }();
    // AN OLDER RECORD, exactly: the three fields this manager added are simply not in it, and
    // its content id names the shape it was written against.
    for (const char* gone : {"\"record\":\"saved\",", "\"record_error\":\"\",",
                             "\"record_ms\":\"1234\","}) {
        const std::size_t at = text.find(gone);
        REQUIRE(at != std::string::npos);
        text.erase(at, std::strlen(gone));
    }
    write(file, text);
    // The gate refuses it by identity, as it should; the manager reads it forward and admits it.
    CHECK_FALSE(admit(compat::parse(text), schema_of<Run>()).ok());
    const std::optional<Run> old = RunManager::read_record(file);
    REQUIRE(old.has_value());
    CHECK(old->state == "passed");
    CHECK(old->summary == "a run from an earlier manager");
    CHECK(old->record == "unknown"); // no claim is made about evidence it never spoke of
    CHECK(old->record_error.empty());
    CHECK(old->record_ms == 0);

    // A record short of a field that is not one of those is not read forward: it is refused.
    std::string broken = text;
    const std::size_t at = broken.find("\"summary\":");
    REQUIRE(at != std::string::npos);
    const std::size_t end = broken.find(',', broken.find(':', at + 10));
    REQUIRE(end != std::string::npos);
    broken.erase(at, end - at + 1);
    write(file, broken);
    CHECK_FALSE(RunManager::read_record(file).has_value());
}

TEST_CASE("manager: a finished run is forgotten on release, and its directory removed when asked") {
    RunsHost h;
    h.ask(Start{"demo/hello", "gone", "{\"times\": 1}"});
    const fs::path dir = h.client->run.directory;
    REQUIRE(until([&] {
        h.ask(Get{h.lifetime, "gone"});
        return h.client->run.state == "crashed";
    }));
    h.ask(Release{h.lifetime, "gone", true});
    CHECK(h.client->acks == 1);
    CHECK_FALSE(fs::exists(dir));
    h.ask(Get{h.lifetime, "gone"});
    CHECK(h.client->refused.find("no run named 'gone'") != std::string::npos);
}

} // TEST_SUITE("runs")
