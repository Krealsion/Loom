// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE SESSION HOST'S TWO ROLES, AT THE MECHANISM ALTITUDE: the session door (who may attach, as
// whom, under what grant -- src/host/session_door.hpp) and the scoped history reader (what a
// client that is not the console may learn from the host's Recorder -- src/host/history_reader.hpp).
// Real sockets and real bridge clients; a real Recorder. The process-level journey that uses them
// together with a real run manager and real Python workers is tests/session/journey.py. The files a
// serving host leaves for its clients (src/host/session_files.hpp) are read back from disk here.

#include <doctest.h>

#include "history_reader.hpp"
#include "session_door.hpp"
#include "session_files.hpp"

#include <zen/bridge/client.hpp>
#include <zen/bridge/link.hpp>
#include <zen/history/recorder.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

using namespace loom;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() /
               ("zen-session-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool until(const std::function<bool()>& step, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (step()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

struct SessEcho {
    std::string msg;
    ZEN_SHAPE(SessEcho, 1, ZEN_FIELD(msg));
};
struct Count {
    std::int64_t n = 0;
    ZEN_SHAPE(Count, 1, ZEN_FIELD(n));
};
class Answerer final
    : public WeaveBase<Answerer, Count, Accept<SessEcho>, Emit<Result>> {
public:
    void on(const SessEcho& e, Mail& mail) {
        ++state_.n;
        (void)mail.answer(Result{"echo:" + e.msg});
    }
    std::int64_t heard() const { return state_.n; }
};

/// What a registrar is told to do, from the test's own root send.
struct Kick {
    std::string what; ///< "expect" / "forget"
    std::string run;
    std::string digest;
    std::vector<std::string> rules;
    ZEN_SHAPE(Kick, 1, ZEN_FIELD(what), ZEN_FIELD(run), ZEN_FIELD(digest), ZEN_FIELD(rules));
};

/// A run manager's side of the door, reduced to its conversations.
class Registrar final
    : public WeaveBase<Registrar, Count,
                       Accept<Kick, session::RunExpected, session::RunConnection, Refused, Ack>,
                       Emit<session::ExpectRun, session::ForgetRun>> {
public:
    void on(const Kick& k, Mail& mail) {
        ++corr_;
        if (k.what == "forget") {
            (void)mail.send_to_role(session::kSessionRole, session::ForgetRun{k.run}, corr_);
        } else {
            (void)mail.send_to_role(session::kSessionRole,
                                    session::ExpectRun{k.run, k.digest, k.rules}, corr_);
        }
    }
    void on(const session::RunExpected& e, Mail& mail) {
        if (mail.answers_ask()) {
            expected.push_back(e);
        }
    }
    void on(const session::RunConnection& n, Mail& mail) {
        notices.push_back(n);
        notice_senders.push_back(mail.sender());
    }
    void on(const Refused& r, Mail& mail) {
        if (mail.answers_ask()) {
            refused.push_back(r.reason);
        }
    }
    void on(const Ack&, Mail& mail) {
        acks += mail.answers_ask() ? 1 : 0;
    }
    std::vector<session::RunExpected> expected;
    std::vector<session::RunConnection> notices;
    std::vector<WeaveId> notice_senders;
    std::vector<std::string> refused;
    int acks = 0;

private:
    std::uint64_t corr_ = 0;
};

const char* const kKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// A bus with a warden, a store, a door listening on loopback, and a Recorder watching.
struct DoorHost {
    TempDir dir{"door"};
    host::AuthorityStore store;
    Switchboard bus;
    Recorder recorder{bus};
    host::HostWarden* warden = nullptr;
    host::SessionDoor* door = nullptr;
    WeaveId door_id{};
    Answerer* echo = nullptr;
    WeaveId echo_id{};

    DoorHost() {
        std::string why;
        REQUIRE(store.open((dir.path / "loom-authority.json").string(), &why));
        auto w = std::make_unique<host::HostWarden>(store, WeaveId{999999});
        warden = w.get();
        const WeaveId wid = bus.register_weave(std::move(w), host::warden_capability(WeaveId{999999}));
        warden->zen_set_self(wid);
        host::SessionFacts facts;
        facts.lifetime = "feedfacefeedfacefeedfacefeedface";
        facts.host = "test";
        auto d = std::make_unique<host::SessionDoor>(facts, kKey, store, *warden);
        door = d.get();
        door_id = bus.register_weave(std::move(d), Grant{}.allow_any(),
                                     std::string(session::kSessionRole));
        door->zen_set_self(door_id);
        REQUIRE_MESSAGE(door->listen(bus, 0, &why), why);
        auto e = std::make_unique<Answerer>();
        echo = e.get();
        echo_id = bus.register_weave(std::move(e), Grant{}.allow_any(), std::string("echo"));
        echo->zen_set_self(echo_id);
    }
    void turn() {
        door->service();
        bus.pump_pending();
    }

    /// A registrar the warden administers as artifact `name`, approved (in the store) to say
    /// `approved` -- its ceiling -- and itself able to talk to the door.
    Registrar* registrar(const std::string& name, const std::vector<std::string>& approved,
                         WeaveId* id_out, const std::string& role = "") {
        host::AuthorityRule rule;
        rule.artifact = name;
        rule.may_run = true;
        rule.send = approved;
        std::string why;
        REQUIRE_MESSAGE(store.put(rule, &why), why);
        Grant g;
        g.allow_to_role(session::ExpectRun::zen_name, 1, session::kSessionRole);
        g.allow_to_role(session::ForgetRun::zen_name, 1, session::kSessionRole);
        auto r = std::make_unique<Registrar>();
        Registrar* raw = r.get();
        *id_out = role.empty() ? bus.register_weave(std::move(r), g)
                               : bus.register_weave(std::move(r), g, role);
        raw->zen_set_self(*id_out);
        warden->govern(name, host_grant_authority(bus, *id_out, LiveAuthority{}.allow_any()));
        return raw;
    }

    void kick(WeaveId who, Kick k) { (void)bus.send(who, Message(to_value(k))); }
};

std::unique_ptr<BridgeClient> attach(DoorHost& h, const std::string& claimed,
                                     const std::string& credential) {
    std::string err;
    const socket_t s = bridge_connect_tcp("127.0.0.1", h.door->port(), &err);
    REQUIRE_MESSAGE(s != kInvalidSocket, err);
    auto c = std::make_unique<BridgeClient>(s);
    REQUIRE(c->hello(claimed, credential));
    std::atomic<bool> done{false};
    bool admitted = false;
    std::thread waiting([&] {
        admitted = c->await_admission(3000);
        done.store(true);
    });
    (void)until([&] {
        h.turn();
        return done.load();
    });
    waiting.join();
    (void)admitted;
    return c;
}

/// Send `v` as a compat envelope and collect what came back for `correlation`.
std::vector<BridgeEvent> ask(DoorHost& h, BridgeClient& c, const std::string& role,
                             const Value& v, std::uint64_t correlation) {
    c.send_to_role(role, correlation, compat::serialize(v));
    std::vector<BridgeEvent> got;
    (void)until([&] {
        h.turn();
        std::vector<BridgeEvent> now;
        c.poll(now);
        for (BridgeEvent& e : now) {
            if (e.correlation == correlation &&
                (e.kind == BridgeEvent::Kind::Delivered || e.kind == BridgeEvent::Kind::SendRefused)) {
                got.push_back(std::move(e));
            }
        }
        return !got.empty();
    });
    return got;
}

} // namespace

TEST_SUITE("session") {

TEST_CASE("door: the owner's key admits a CLIENT, in compat, with exactly the client grant") {
    DoorHost h;
    auto c = attach(h, "anybody-at-all", kKey);
    REQUIRE(c->admitted());
    CHECK(c->established_name() == session::kClientSessionName); // the door's word, not the claim
    // What the client grant covers is answered...
    std::vector<BridgeEvent> got = ask(h, *c, session::kSessionRole, to_value(session::Describe{}), 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].answers_ask);
    Admission d = admit(compat::parse(got[0].payload), schema_of<session::Description>());
    REQUIRE(d.ok());
    CHECK(d.value().get("lifetime")->as_text() == "feedfacefeedfacefeedfacefeedface");
    // ...and what it does not -- registering a run, or speaking to a role it was not given --
    // is refused by the BUS, and the client is told.
    got = ask(h, *c, session::kSessionRole, to_value(session::ExpectRun{"r", std::string(64, 'a'), {}}), 2);
    REQUIRE(got.size() == 1);
    CHECK(got[0].dispatch_refused);
    got = ask(h, *c, "echo", to_value(SessEcho{"no"}), 3);
    REQUIRE(got.size() == 1);
    CHECK(got[0].dispatch_refused);
    CHECK(h.echo->heard() == 0);
    CHECK(h.door->expectations() == 0);
}

TEST_CASE("door: anything but the owner's key or an expected credential is refused, and admitted nowhere") {
    DoorHost h;
    std::string err;
    const socket_t s = bridge_connect_tcp("127.0.0.1", h.door->port(), &err);
    REQUIRE(s != kInvalidSocket);
    BridgeClient c(s);
    REQUIRE(c.hello("client", "not-the-key"));
    (void)until([&] {
        h.turn();
        std::vector<BridgeEvent> ev;
        c.poll(ev);
        return c.denied();
    });
    CHECK(c.denied());
    CHECK(c.denial().find("neither") != std::string::npos);
    const std::size_t weaves_before = h.bus.list_weaves().size();
    h.turn();
    CHECK(h.bus.list_weaves().size() == weaves_before); // no proxy was ever registered
}

TEST_CASE("door: an expected credential is admitted ONCE as run:<name>, with the rules granted, and the bus carries only its digest") {
    DoorHost h;
    WeaveId mgr_id{};
    Registrar* mgr = h.registrar("runs", {"SessEcho v1 -> role echo"}, &mgr_id);
    const std::string credential = "the-one-time-credential-of-run-r1";
    h.kick(mgr_id, Kick{"expect", "r1", host::credential_digest(credential), {"SessEcho v1 -> role echo"}});
    REQUIRE(until([&] { h.turn(); return !mgr->expected.empty(); }));
    CHECK(mgr->expected[0].established == "run:r1");
    REQUIRE(mgr->expected[0].granted.size() == 1);
    CHECK(h.door->expectations() == 1);

    auto worker = attach(h, "whatever-it-claims", credential);
    REQUIRE(worker->admitted());
    CHECK(worker->established_name() == "run:r1");
    // The registrar hears it, from the door, with the session it was admitted as.
    REQUIRE(until([&] { h.turn(); return !mgr->notices.empty(); }));
    CHECK(mgr->notices[0].state == "admitted");
    CHECK(static_cast<std::uint64_t>(mgr->notices[0].session) == worker->session());
    CHECK(mgr->notice_senders[0] == h.door_id);
    // Exactly the granted rule: SessEcho to `echo` is answered, anything else is refused.
    std::vector<BridgeEvent> got = ask(h, *worker, "echo", to_value(SessEcho{"hi"}), 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].answers_ask);
    got = ask(h, *worker, session::kSessionRole, to_value(session::Describe{}), 2);
    REQUIRE(got.size() == 1);
    CHECK(got[0].dispatch_refused);

    // ONE-TIME: the same credential again is refused.
    std::string err;
    const socket_t s = bridge_connect_tcp("127.0.0.1", h.door->port(), &err);
    BridgeClient again(s);
    REQUIRE(again.hello("run", credential));
    (void)until([&] { h.turn(); std::vector<BridgeEvent> ev; again.poll(ev); return again.denied(); });
    CHECK(again.denied());

    // THE BUS NEVER HELD THE CREDENTIAL: every retained payload is searched for it; the digest is
    // what crossed.
    bool credential_seen = false;
    bool digest_seen = false;
    for (const HistoryRecord& r : h.recorder.snapshot()) {
        const PayloadLookup p = h.recorder.payload(r.record_seq);
        if (p.state == PayloadState::Retained) {
            credential_seen = credential_seen || p.bytes.find(credential) != std::string::npos;
            digest_seen = digest_seen ||
                          p.bytes.find(host::credential_digest(credential)) != std::string::npos;
        }
    }
    CHECK_FALSE(credential_seen);
    CHECK(digest_seen);
}

TEST_CASE("door: a rule outside the registrar's approved authority refuses the WHOLE registration, naming the decision") {
    DoorHost h;
    WeaveId mgr_id{};
    Registrar* mgr = h.registrar("runs", {"SessEcho v1 -> role echo"}, &mgr_id);
    h.kick(mgr_id, Kick{"expect", "greedy", std::string(64, 'b'),
                        {"SessEcho v1 -> role echo", "any shape -> any target"}});
    REQUIRE(until([&] { h.turn(); return !mgr->refused.empty(); }));
    CHECK(mgr->refused[0].find("'any shape -> any target'") != std::string::npos);
    CHECK(mgr->refused[0].find("authority allow runs any shape -> any target") != std::string::npos);
    CHECK(mgr->expected.empty());
    CHECK(h.door->expectations() == 0); // not a partial grant: nothing was registered
}

TEST_CASE("door: a rule to the registrar's OWN role needs no ceiling; a registrar that is no administered artifact may pass on nothing else") {
    DoorHost h;
    WeaveId mgr_id{};
    Registrar* mgr = h.registrar("runs", {}, &mgr_id, "loom.runs");
    h.kick(mgr_id, Kick{"expect", "reports", std::string(64, 'c'), {"Progress v1 -> role loom.runs"}});
    REQUIRE(until([&] { h.turn(); return !mgr->expected.empty(); }));
    CHECK(mgr->expected[0].granted.size() == 1);

    // An ungoverned participant (no artifact, so no approved authority) asking for anything else.
    Grant g;
    g.allow_to_role(session::ExpectRun::zen_name, 1, session::kSessionRole);
    auto stray = std::make_unique<Registrar>();
    Registrar* raw = stray.get();
    const WeaveId stray_id = h.bus.register_weave(std::move(stray), g);
    raw->zen_set_self(stray_id);
    h.kick(stray_id, Kick{"expect", "stray", std::string(64, 'd'), {"SessEcho v1 -> role echo"}});
    REQUIRE(until([&] { h.turn(); return !raw->refused.empty(); }));
    CHECK(raw->refused[0].find("not an artifact this host administers") != std::string::npos);
}

TEST_CASE("door: ForgetRun severs the run's session; another registrar cannot; a registrar gone severs all of its runs") {
    DoorHost h;
    WeaveId a_id{};
    WeaveId b_id{};
    Registrar* a = h.registrar("runs-a", {}, &a_id);
    Registrar* b = h.registrar("runs-b", {}, &b_id);
    h.kick(a_id, Kick{"expect", "one", host::credential_digest("cred-one"), {}});
    h.kick(a_id, Kick{"expect", "two", host::credential_digest("cred-two"), {}});
    REQUIRE(until([&] { h.turn(); return a->expected.size() == 2; }));
    auto one = attach(h, "w", "cred-one");
    auto two = attach(h, "w", "cred-two");
    REQUIRE(one->admitted());
    REQUIRE(two->admitted());
    // Another registrar may not forget a's run.
    h.kick(b_id, Kick{"forget", "one", "", {}});
    REQUIRE(until([&] { h.turn(); return !b->refused.empty(); }));
    CHECK(b->refused[0].find("another participant") != std::string::npos);
    // a forgets its own: the session is severed, and a is told it closed.
    h.kick(a_id, Kick{"forget", "one", "", {}});
    REQUIRE(until([&] {
        h.turn();
        std::vector<BridgeEvent> ev;
        one->poll(ev);
        return one->disconnected();
    }));
    // a leaves the bus: every session it registered is severed at the next service.
    std::unique_ptr<Weave> gone = h.bus.unregister_weave(a_id);
    REQUIRE(gone != nullptr);
    REQUIRE(until([&] {
        h.turn();
        std::vector<BridgeEvent> ev;
        two->poll(ev);
        return two->disconnected();
    }));
    CHECK(h.door->expectations() == 0);
}

TEST_CASE("door: a registrar gone, its run's session is severed before anything it sent is read, "
          "however long the host rested") {
    DoorHost h;
    WeaveId a_id{};
    Registrar* a = h.registrar("runs-a", {"SessEcho v1 -> role echo"}, &a_id);
    h.kick(a_id, Kick{"expect", "one", host::credential_digest("cred-one"),
                      {"SessEcho v1 -> role echo"}});
    REQUIRE(until([&] { h.turn(); return a->expected.size() == 1; }));
    auto one = attach(h, "w", "cred-one");
    REQUIRE(one->admitted());
    // The grant is real: while the registrar is on the bus, the same send is heard.
    REQUIRE(ask(h, *one, "echo", to_value(SessEcho{"before"}), 76).size() == 1);
    REQUIRE(h.echo->heard() == 1);
    // The registrar leaves in a turn; the worker speaks, within its grant, while the host rests,
    // before the door's next service.
    std::unique_ptr<Weave> gone = h.bus.unregister_weave(a_id);
    REQUIRE(gone != nullptr);
    one->send_to_role("echo", 77, compat::serialize(to_value(SessEcho{"after"})));
    one->flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the bytes reach the door
    REQUIRE(until([&] {
        h.turn();
        std::vector<BridgeEvent> ev;
        one->poll(ev);
        return one->disconnected();
    }));
    for (int i = 0; i < 4; ++i) {
        h.turn();
    }
    CHECK(h.echo->heard() == 1); // the second never read, so never delivered
    CHECK(h.door->expectations() == 0);
}

TEST_CASE("door: Describe lists who is attached as what; Shutdown is the one word that ends the lifetime") {
    DoorHost h;
    WeaveId mgr_id{};
    Registrar* mgr = h.registrar("runs", {}, &mgr_id);
    h.kick(mgr_id, Kick{"expect", "rx", host::credential_digest("cred-x"), {}});
    REQUIRE(until([&] { h.turn(); return !mgr->expected.empty(); }));
    auto client = attach(h, "c", kKey);
    auto worker = attach(h, "w", "cred-x");
    const session::Description d = h.door->description();
    int clients = 0;
    int runs = 0;
    for (const session::Connection& c : d.connections) {
        clients += c.kind == "client" && c.state == "admitted" ? 1 : 0;
        runs += c.kind == "run" && c.name == "run:rx" ? 1 : 0;
        CHECK(c.encoding == "compat");
    }
    CHECK(clients == 1);
    CHECK(runs == 1);
    CHECK_FALSE(h.door->ending());
    std::vector<BridgeEvent> got = ask(h, *client, session::kSessionRole,
                                       to_value(session::Shutdown{"test"}), 9);
    REQUIRE(got.size() == 1);
    CHECK(got[0].answers_ask);
    CHECK(h.door->ending());
    CHECK(h.door->ending_reason() == "test");
}

// ---- the scoped history reader ---------------------------------------------------------------

namespace {

/// Stands in for a link: what it tells ITSELF is a crossing record; handling it, it hands the
/// asker a value -- whose dispatch parent is therefore that record.
class FakeLink final
    : public WeaveBase<FakeLink, Count, Accept<link::Crossed>, Emit<Result, link::Crossed>> {
public:
    WeaveId deliver_to{};
    void on(const link::Crossed& x, Mail& mail) {
        (void)mail.send(deliver_to, Result{"far said " + (x.word() != nullptr ? x.word()->shape
                                                                              : std::string())});
    }
};

class Sink final : public WeaveBase<Sink, Count, Accept<Result, session::Records>, Emit<>> {
public:
    void on(const Result&, Mail&) { ++state_.n; }
    void on(const session::Records& r, Mail&) { last = r; }
    session::Records last;
};

struct ReaderHost {
    Switchboard bus;
    Recorder recorder{bus};
    host::HistoryReader* reader = nullptr;
    WeaveId reader_id{};
    Sink* sink = nullptr;
    WeaveId sink_id{};
    ReaderHost() {
        auto r = std::make_unique<host::HistoryReader>(recorder);
        reader = r.get();
        Grant g;
        g.allow_to_any(session::Records::zen_name, 1);
        reader_id = bus.register_weave(std::move(r), g, std::string(session::kHistoryRole));
        reader->zen_set_self(reader_id);
        recorder.blacklist().declare_participant(reader_id);
        auto s = std::make_unique<Sink>();
        sink = s.get();
        Grant sg;
        sg.allow_to_role(session::DeliveriesTo::zen_name, 1, session::kHistoryRole);
        sg.allow_to_role(session::Delivery::zen_name, 1, session::kHistoryRole);
        sink_id = bus.register_weave(std::move(s), sg);
        sink->zen_set_self(sink_id);
    }
    template <class T>
    session::Records query(const T& q) {
        (void)bus.send_as_to_role(sink_id, session::kHistoryRole, Message(to_value(q), sink_id, sink_id, 77));
        bus.drain_until_idle();
        return sink->last;
    }
    FakeLink* link(WeaveId* id) {
        auto l = std::make_unique<FakeLink>();
        FakeLink* raw = l.get();
        *id = bus.register_weave(std::move(l), Grant{}.allow_any());
        raw->zen_set_self(*id);
        raw->deliver_to = sink_id;
        return raw;
    }
    void crossed(WeaveId link_id, std::int64_t attempt) {
        link::Crossed x;
        x.link = "far";
        x.epoch = 2;
        x.session = 20;
        x.established = "agent";
        link::FarWord w;
        w.attempt = attempt;
        w.far_sender = 16;
        w.shape = "SurfaceCaptured";
        w.version = 1;
        x.what.is.emplace<link::kFrameAnswer>(std::move(w));
        (void)bus.send_as(link_id, link_id, Message(to_value(x), link_id));
        bus.drain_until_idle();
    }
};

} // namespace

TEST_CASE("reader: a delivery descending from a MOUNTED link's own crossing record carries the far session, author and attempt") {
    ReaderHost h;
    WeaveId link_id{};
    (void)h.link(&link_id);
    h.reader->set_links({link_id.value});
    h.crossed(link_id, 287);
    const session::Records r = h.query(session::DeliveriesTo{static_cast<std::int64_t>(h.sink_id.value), "zen.Result", 0});
    REQUIRE(r.rows.size() == 1);
    const session::Record& row = r.rows[0];
    CHECK(row.parent_horizon == "retained");
    const auto* read = std::get_if<session::CrossingRead>(&row.crossing.is);
    REQUIRE(read != nullptr);
    CHECK(read->link == "far");
    CHECK(read->epoch == 2);
    CHECK(read->far_session == 20);
    CHECK(read->established == "agent");
    CHECK(read->attempt == 287);
    CHECK(read->far_sender == 16);
    CHECK(read->kind == "answer");
    CHECK(read->far_shape == "SurfaceCaptured");
}

TEST_CASE("reader: the same record said by a participant that is not a mounted link is no crossing") {
    ReaderHost h;
    WeaveId impostor{};
    (void)h.link(&impostor);
    h.reader->set_links({}); // nobody is a link here
    h.crossed(impostor, 5);
    const session::Records r = h.query(session::DeliveriesTo{static_cast<std::int64_t>(h.sink_id.value), "zen.Result", 0});
    REQUIRE(r.rows.size() == 1);
    // No crossing field is read from a record that is not one: the row holds none.
    CHECK(std::holds_alternative<std::monostate>(r.rows[0].crossing.is));
    CHECK(r.rows[0].parent_shape == link::Crossed::zen_name);
}

TEST_CASE("reader: forgotten, unobserved and retained keep their own words; reading never writes history") {
    ReaderHost h;
    RecorderPolicy p = default_policy();
    p.recent_capacity = 2;
    p.rules.push_back(RetentionRule{link::Crossed::zen_name, 0, true, true}); // no last-call slot
    // Results enter recent context (so four of them push the crossing record out of a window of
    // two) and eight last-call slots keep every one of them.
    p.rules.push_back(RetentionRule{"zen.Result", 8, true, true});
    h.recorder.apply_policy(p);
    WeaveId link_id{};
    (void)h.link(&link_id);
    h.reader->set_links({link_id.value});
    h.crossed(link_id, 1);
    for (int i = 0; i < 4; ++i) { // push the crossing record out of the small recent window
        (void)h.bus.send(h.sink_id, Message(to_value(Result{"filler"})));
        h.bus.drain_until_idle();
    }
    const std::uint64_t declined_before = h.recorder.counters().declined_internal;
    const session::Records r = h.query(session::DeliveriesTo{static_cast<std::int64_t>(h.sink_id.value), "zen.Result", 0});
    REQUIRE_FALSE(r.rows.empty());
    const session::Record& oldest = r.rows.back();
    CHECK(oldest.parent != 0);
    CHECK(oldest.parent_horizon == "forgotten"); // never "none", never "nothing happened"
    CHECK(std::holds_alternative<std::monostate>(oldest.crossing.is)); // its parent is forgotten
    const session::Records u = h.query(session::Delivery{1000000});
    CHECK(u.asked_horizon == "unobserved");
    // The reader's own questions and answers are machinery, not facts about the bus: the
    // structural blacklist declined them (two questions, two answers), and none is a record.
    CHECK(h.recorder.counters().declined_internal - declined_before == 4);
    bool reader_recorded = false;
    for (const HistoryRecord& rec : h.recorder.snapshot()) {
        reader_recorded = reader_recorded || rec.sender == h.reader_id || rec.target == h.reader_id;
    }
    CHECK_FALSE(reader_recorded);
}

} // TEST_SUITE("session")

// The session key is the owner's client key. The directory is made to grant more than its owner
// first, so a key that took the directory's grant would show it; session.json, which is no
// secret, is the canary that the directory's grant reaches a plain file written there.
#ifdef _WIN32
namespace {

std::vector<unsigned char> test_user_sid() {
    HANDLE token = nullptr;
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> info(size);
    REQUIRE(::GetTokenInformation(token, TokenUser, info.data(), size, &size));
    ::CloseHandle(token);
    const PSID user = reinterpret_cast<const TOKEN_USER*>(info.data())->User.Sid;
    std::vector<unsigned char> sid(::GetLengthSid(user));
    REQUIRE(::CopySid(static_cast<DWORD>(sid.size()), sid.data(), user));
    return sid;
}

// Give Everyone read on the directory, inherited by the files made in it.
void grant_everyone_read(const fs::path& dir) {
    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
    PSID everyone = nullptr;
    REQUIRE(::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0,
                                       &everyone));
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR old_sd = nullptr;
    std::wstring wdir = dir.wstring();
    REQUIRE(::GetNamedSecurityInfoW(wdir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &old_dacl, nullptr, &old_sd) ==
            ERROR_SUCCESS);
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = OBJECT_INHERIT_ACE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
    PACL new_dacl = nullptr;
    REQUIRE(::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) == ERROR_SUCCESS);
    REQUIRE(::SetNamedSecurityInfoW(wdir.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, new_dacl, nullptr) == ERROR_SUCCESS);
    ::LocalFree(new_dacl);
    ::LocalFree(old_sd);
    ::FreeSid(everyone);
}

struct FileAccess {
    bool protected_dacl = false;
    bool owner_is_user = false;
    DWORD entries = 0;
    bool only_the_user = true; // every entry allows, and names the process user
};

FileAccess access_of(const fs::path& file, const std::vector<unsigned char>& sid) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    std::wstring wfile = file.wstring();
    REQUIRE(::GetNamedSecurityInfoW(wfile.c_str(), SE_FILE_OBJECT,
                                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                    &owner, nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS);
    const PSID user = const_cast<unsigned char*>(sid.data());
    FileAccess a;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(sd, &control, &revision));
    a.protected_dacl = (control & SE_DACL_PROTECTED) != 0;
    a.owner_is_user = owner != nullptr && ::EqualSid(owner, user);
    REQUIRE(dacl != nullptr); // a null DACL grants everyone everything
    a.entries = dacl->AceCount;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        REQUIRE(::GetAce(dacl, i, &ace));
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            !::EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, user)) {
            a.only_the_user = false;
        }
    }
    ::LocalFree(sd);
    return a;
}

// On Windows Server an administrator's new file belongs to the Administrators group, so the read
// names that owner before the directory's permissions. Assign this user and leave the DACL the
// directory gave the file. The restore privilege lets that assignment name the user; a file this
// user already owns is kept when the token does not carry it.
void own_as_this_user(const fs::path& file) {
    std::vector<unsigned char> sid = test_user_sid();
    std::wstring wfile = file.wstring();
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES previous{};
    bool held = false;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        LUID luid{};
        // Spelled wide. SE_RESTORE_NAME follows TEXT(), which is narrow unless UNICODE is set.
        if (::LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege", &luid)) {
            TOKEN_PRIVILEGES enable{};
            enable.PrivilegeCount = static_cast<DWORD>(1);
            enable.Privileges[0].Luid = luid;
            enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            DWORD returned = 0;
            ::SetLastError(ERROR_SUCCESS);
            if (::AdjustTokenPrivileges(token, FALSE, &enable, static_cast<DWORD>(sizeof(previous)),
                                        &previous, &returned) &&
                ::GetLastError() == ERROR_SUCCESS) {
                held = true;
            }
        }
    } else {
        token = nullptr;
    }
    const DWORD rc =
        ::SetNamedSecurityInfoW(wfile.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                static_cast<PSID>(sid.data()), nullptr, nullptr, nullptr);
    if (held) {
        ::AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
    }
    if (token != nullptr) {
        ::CloseHandle(token);
    }
    const FileAccess got = access_of(file, sid);
    // A Windows client already owns the file it created, so a refused assignment there still
    // leaves this user as owner. The directory's DACL stays the fault.
    REQUIRE_MESSAGE(got.owner_is_user, ("owner assignment returned " + std::to_string(rc)));
    REQUIRE_FALSE(got.protected_dacl);
}

} // namespace
#endif

TEST_SUITE("session") {

TEST_CASE("session files: the key is readable by the host's user alone, whatever the directory grants") {
    TempDir dir("key-access");
    const fs::path key = dir.path / loom::host::kSessionKeyFile;
    const fs::path json = dir.path / loom::host::kSessionFile;
#ifdef _WIN32
    const std::vector<unsigned char> sid = test_user_sid();
    grant_everyone_read(dir.path);
#else
    fs::permissions(dir.path, fs::perms::all);
    const mode_t old_mask = ::umask(0);
#endif
    std::string why;
    const bool key_written = loom::host::write_session_text(key, "secret\n", true, &why);
    const bool json_written = loom::host::write_session_text(json, "{}\n", false, &why);
#ifndef _WIN32
    ::umask(old_mask);
#endif
    REQUIRE_MESSAGE(key_written, why);
    REQUIRE_MESSAGE(json_written, why);
    CHECK_FALSE(fs::exists(dir.path / "session.key.tmp"));
    {
        std::ifstream in(key, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text == "secret\n");
    }
#ifdef _WIN32
    // The canary: the directory's grant reaches a plain file.
    const FileAccess plain = access_of(json, sid);
    REQUIRE_FALSE(plain.only_the_user);
    // The key: a protected DACL with one entry, allowing the process user, who owns it.
    const FileAccess secret = access_of(key, sid);
    CHECK(secret.protected_dacl);
    CHECK(secret.owner_is_user);
    CHECK(secret.entries == 1);
    CHECK(secret.only_the_user);
#else
    const fs::perms group_or_other = fs::perms::group_all | fs::perms::others_all;
    REQUIRE((fs::status(json).permissions() & group_or_other) != fs::perms::none);
    CHECK((fs::status(key).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
#endif
    // The host's own read-back agrees: the key reads as this user's alone, the plain file not.
    std::string fault;
    CHECK_MESSAGE(loom::host::reads_as_owner_only(key, &fault), fault);
    CHECK_FALSE(loom::host::reads_as_owner_only(json, &fault));
}

// The read-back the host serves on: a key that anyone else could read is named, with why. A
// volume that keeps no permissions (FAT, exFAT) is refused before any of this is read; no suite
// has such a volume, so the live witness on one is the record of that branch.
TEST_CASE("session files: a key that is not its user's alone does not read as owner-only, and says why") {
    TempDir dir("key-readback");
    const fs::path key = dir.path / loom::host::kSessionKeyFile;
    std::string why;
    REQUIRE_MESSAGE(loom::host::write_session_text(key, "secret\n", true, &why), why);
    REQUIRE_MESSAGE(loom::host::reads_as_owner_only(key, &why), why);
#ifdef _WIN32
    // A second entry, for Everyone, beside the user's own, the DACL still protected.
    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
    PSID everyone = nullptr;
    REQUIRE(::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0,
                                       &everyone));
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR old_sd = nullptr;
    std::wstring wkey = key.wstring();
    REQUIRE(::GetNamedSecurityInfoW(wkey.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &old_dacl, nullptr, &old_sd) ==
            ERROR_SUCCESS);
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
    PACL new_dacl = nullptr;
    REQUIRE(::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) == ERROR_SUCCESS);
    REQUIRE(::SetNamedSecurityInfoW(wkey.data(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION |
                                        PROTECTED_DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, new_dacl, nullptr) == ERROR_SUCCESS);
    ::LocalFree(new_dacl);
    ::LocalFree(old_sd);
    ::FreeSid(everyone);
    CHECK_FALSE(loom::host::reads_as_owner_only(key, &why));
    CHECK(why == "it reads back with 2 access entries, not 1");
    // A file that takes its directory's permissions, owned by this user.
    grant_everyone_read(dir.path);
    const fs::path plain = dir.path / "plain.txt";
    REQUIRE_MESSAGE(loom::host::write_session_text(plain, "x\n", false, &why), why);
    own_as_this_user(plain);
    CHECK_FALSE(loom::host::reads_as_owner_only(plain, &why));
    CHECK(why == "it reads back taking permissions from its directory");
#else
    fs::permissions(key, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    CHECK_FALSE(loom::host::reads_as_owner_only(key, &why));
    CHECK(why == "it reads back as mode 0640, not 0600");
    const fs::path link = dir.path / "link.key";
    fs::create_symlink(key, link);
    CHECK_FALSE(loom::host::reads_as_owner_only(link, &why));
    CHECK(why == "it reads back as something other than a plain file");
#endif
}

} // TEST_SUITE("session")
