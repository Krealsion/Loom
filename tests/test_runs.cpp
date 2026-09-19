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
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

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
