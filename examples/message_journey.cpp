// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A message's journey: the compiled twin of docs/guides/message-journey.md. Each journey runs on
// a bus of its own and prints the bus's own record of it -- every event the tap reported, in the
// order of the sequence numbers the bus gave out -- with what the receiving handler read off its
// Mail beneath the delivery it read it from. The page shows this program's output, and the
// `message_journey` CTest entry checks that the two still agree.

#include <zen/bridge/channel.hpp>
#include <zen/bridge/server.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include "link.hpp" // the supplied host's link (src/host/link.hpp), one per `links` row

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loom;

namespace {

struct Start { std::int64_t n = 0; ZEN_SHAPE(Start, 1, ZEN_FIELD(n)); };
struct Job { std::int64_t n = 0; ZEN_SHAPE(Job, 1, ZEN_FIELD(n)); };
struct Put { std::int64_t n = 0; ZEN_SHAPE(Put, 1, ZEN_FIELD(n)); };
struct Match { std::int64_t server = 0; ZEN_SHAPE(Match, 1, ZEN_FIELD(server)); };
struct Tick { std::int64_t n = 0; ZEN_SHAPE(Tick, 1, ZEN_FIELD(n)); };
struct Query { std::int64_t n = 0; ZEN_SHAPE(Query, 1, ZEN_FIELD(n)); };
struct Reply { std::int64_t n = 0; ZEN_SHAPE(Reply, 1, ZEN_FIELD(n)); };
struct Thanks { std::int64_t n = 0; ZEN_SHAPE(Thanks, 1, ZEN_FIELD(n)); };
struct Hot { std::int64_t n = 0; ZEN_SHAPE(Hot, 1, ZEN_FIELD(n)); };
struct Ping { std::int64_t seq = 0; ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq)); };
struct Pong { std::int64_t seq = 0; ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq)); };
struct Echo { std::string text; ZEN_SHAPE(Echo, 1, ZEN_FIELD(text)); };
struct Echoed { std::string text; ZEN_SHAPE(Echoed, 1, ZEN_FIELD(text)); };
struct Nothing { std::int64_t n = 0; ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n)); };

// ---- the record --------------------------------------------------------------------------

/// One bus's record: a line per tap event, kept by sequence number, and what a handler read
/// during a delivery, attached to that delivery when its event arrives.
class Record {
public:
    explicit Record(Switchboard& bus) : bus_(bus) {
        observer_ = bus_.add_observer([this](const BusEvent& e) { note(e); });
    }
    ~Record() { bus_.remove_observer(observer_); }
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    void name(WeaveId id, std::string who) { names_[id.value] = std::move(who); }
    std::string who(WeaveId id) const { return who(id, ""); }
    /// Called by a handler while it runs: what it read off its delivery.
    void read(const std::string& who, const std::string& what) {
        const std::lock_guard<std::mutex> lock(mutex_);
        reading_ += "     " + who + " reads: " + what + "\n";
    }
    void print(std::ostream& out) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::stable_sort(lines_.begin(), lines_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& line : lines_) {
            out << line.second;
        }
    }

private:
    std::string who(WeaveId id, const std::string& shape) const {
        if (!id.valid()) {
            return shape == DispatchRefused::zen_name ? "loom" : "host";
        }
        const auto it = names_.find(id.value);
        return it == names_.end() ? "weave " + std::to_string(id.value) : it->second;
    }
    void note(const BusEvent& e) {
        std::ostringstream s;
        const char* kind = e.kind == EventKind::Delivered ? "Delivered"
                           : e.kind == EventKind::Refused ? "Refused  "
                                                          : "Failed   ";
        s << "#" << e.seq << " " << kind << " " << who(e.sender, e.schema_name) << " -> "
          << (e.target.valid() ? who(e.target, "") : "nobody")
          << "  " << e.schema_name << " v" << e.schema_version;
        if (!e.addressed_role.empty()) { s << ", to office " << e.addressed_role; }
        if (!e.authored_role.empty()) { s << ", as office " << e.authored_role; }
        if (e.correlation != 0) { s << ", correlation " << e.correlation; }
        if (e.dispatch_parent != 0) { s << ", queued in #" << e.dispatch_parent; }
        if (e.kind == EventKind::Refused) { s << ": " << name_of(e.refusal.reason); }
        s << "\n";
        const std::lock_guard<std::mutex> lock(mutex_);
        if (e.kind != EventKind::Refused) {
            s << reading_;
            reading_.clear();
        }
        lines_.emplace_back(e.seq, s.str());
    }

    Switchboard& bus_;
    ObserverId observer_ = 0;
    std::map<std::uint64_t, std::string> names_;
    std::mutex mutex_; // the far bus of the link journey is stepped on its own thread
    std::string reading_;
    std::vector<std::pair<std::uint64_t, std::string>> lines_;
};

Record* g_record = nullptr; // the journey being recorded

const char* yes(bool b) { return b ? "yes" : "no"; }

/// What Loom stamped on the delivery being handled, in the words the page uses.
std::string stamps(Mail& mail, const std::string& sender) {
    std::string s = "from " + sender + ", answers my ask " + yes(mail.answers_ask());
    if (!mail.authored_role().empty()) {
        s += ", spoken as office " + std::string(mail.authored_role());
    }
    return s;
}

/// Register `W` as `who`, under the grant `grant_of` gives the constructed weave, holding `role`
/// when one is named.
template <class W, class G, class... Args>
W* add(Switchboard& bus, Record& rec, const std::string& who, G grant_of, std::string role,
       Args&&... args) {
    auto weave = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = weave.get();
    Grant grant = grant_of(*raw);
    const WeaveId id = role.empty() ? bus.register_weave(std::move(weave), std::move(grant))
                                    : bus.register_weave(std::move(weave), std::move(grant),
                                                         std::move(role));
    raw->zen_set_self(id);
    raw->id = id;
    rec.name(id, who);
    return raw;
}

/// The grant a trusted in-process weave gets from its own Emit<...> list (`mount` gives the same).
const auto declared = [](const auto& w) { return emit_default_grant(w); };
/// No authority to say anything.
const auto silent = [](const auto&) { return Grant{}; };

// ---- the participants --------------------------------------------------------------------

/// Hears whatever it is sent and says what Loom stamped on it.
class Worker final : public WeaveBase<Worker, Nothing, Accept<Job, Put, Match, Tick>> {
public:
    explicit Worker(std::string who) : who_(std::move(who)) {}
    WeaveId id{};
    void on(const Job&, Mail& m) { g_record->read(who_, stamps(m, from(m))); }
    void on(const Put&, Mail& m) { g_record->read(who_, stamps(m, from(m))); }
    void on(const Tick&, Mail& m) { g_record->read(who_, stamps(m, from(m))); }
    void on(const Match&, Mail& m) {
        g_record->read(who_, "from " + from(m) + ", spoken as the matchmaker office " +
                                 yes(m.authored_from_role("matchmaker")));
    }

private:
    static std::string from(Mail& m) { return g_record->who(m.sender()); }
    std::string who_;
};

/// Has a door for Put alone.
class PutOnly final : public WeaveBase<PutOnly, Nothing, Accept<Put>> {
public:
    WeaveId id{};
    void on(const Put&, Mail&) {}
};

/// The sender of the spine and its variations: what it does on Start is the journey.
class Sender final : public WeaveBase<Sender, Nothing, Accept<Start, DispatchRefused>,
                                      Emit<Job, Put, Match, Tick>> {
public:
    enum class Does { Direct, ToRole, AsOffice, Publish, Refusals };
    Sender(Does does, WeaveId to) : does_(does), to_(to) {}
    WeaveId id{};
    void on(const Start&, Mail& m) {
        switch (does_) {
        case Does::Direct: (void)m.send(to_, Job{1}); break;
        case Does::ToRole: (void)m.send_to_role("storage", Put{1}); break;
        case Does::AsOffice:
            (void)m.send(to_, Match{7});                       // holding the office: personal
            (void)m.as_role("matchmaker").send(to_, Match{7}); // speaking as it: stamped
            break;
        case Does::Publish: (void)m.publish(Tick{1}); break;
        case Does::Refusals:
            (void)m.send_to_role("nobody", Job{1}); // no one holds that office
            (void)m.send(to_, Put{1});              // the grant does not allow Put
            (void)m.send(to_, Job{1});              // the target has no door for Job
            break;
        }
    }
    void on(const DispatchRefused& r, Mail& m) {
        g_record->read("sender", std::string("Loom's refusal notice ") + yes(m.dispatch_refused()) +
                                     ", for my send #" + std::to_string(r.refused_attempt().seq) +
                                     ": " + r.reason);
    }

private:
    Does does_;
    WeaveId to_;
};

/// Answers a Query at once, or defers it until it is Hot; a second answer is refused.
class Responder final
    : public WeaveBase<Responder, Nothing, Accept<Query, Hot, Thanks>, Emit<Reply>> {
public:
    explicit Responder(bool defers) : defers_(defers) {}
    WeaveId id{};
    void on(const Query& q, Mail& m) {
        if (defers_) {
            pending_ = m.defer_answer();
            g_record->read("responder", std::string("kept the answer for later ") +
                                            yes(pending_.valid()));
            return;
        }
        (void)m.answer(Reply{q.n});
        const Ticket again = m.answer(Reply{q.n}); // one ask, one answer
        g_record->read("responder", std::string("a second answer queued ") + yes(again.valid()));
    }
    void on(const Hot&, Mail& m) { (void)answer_deferred(pending_, m, Reply{1}); }
    void on(const Thanks&, Mail& m) { g_record->read("responder", stamps(m, "asker")); }

private:
    bool defers_;
    DeferredAnswer pending_{};
};

class Asker final : public WeaveBase<Asker, Nothing, Accept<Start, Reply>, Emit<Query, Thanks>> {
public:
    Asker(WeaveId responder, bool thanks) : responder_(responder), thanks_(thanks) {}
    WeaveId id{};
    void on(const Start&, Mail& m) { (void)m.send(responder_, Query{1}, /*correlation=*/41); }
    void on(const Reply&, Mail& m) {
        g_record->read("asker", stamps(m, "responder") + ", correlation " +
                                    std::to_string(m.correlation()));
        if (thanks_) {
            (void)m.answer(Thanks{1}); // an answer may itself be answered: a new conversation
        }
    }

private:
    WeaveId responder_;
    bool thanks_;
};

/// The README's Responder: an ordinary reply to the reply address.
class PingResponder final
    : public WeaveBase<PingResponder, Nothing, Accept<Ping>, Emit<Pong>> {
public:
    WeaveId id{};
    void on(const Ping& p, Mail& m) { (void)m.reply(Pong{p.seq}); }
};
class PongListener final : public WeaveBase<PongListener, Nothing, Accept<Pong>> {
public:
    WeaveId id{};
    void on(const Pong&, Mail& m) { g_record->read("listener", stamps(m, "responder")); }
};

// ---- the journeys ------------------------------------------------------------------------

void heading(const char* name) { std::cout << "== " << name << " ==\n"; }

template <class Build>
void journey(const char* name, Build build) {
    heading(name);
    Switchboard bus;
    Record rec(bus);
    g_record = &rec;
    build(bus, rec);
    bus.drain_until_idle();
    rec.print(std::cout);
    g_record = nullptr;
}

/// The spine and the variations that differ only in what the sender does.
template <class G>
void sender_journey(const char* name, Sender::Does does, G grant_of) {
    journey(name, [&](Switchboard& bus, Record& rec) {
        WeaveId to{};
        if (does == Sender::Does::Refusals) {
            to = add<PutOnly>(bus, rec, "put-only", silent, "")->id;
        } else if (does == Sender::Does::ToRole) {
            (void)add<Worker>(bus, rec, "store", silent, "storage", "store");
        } else {
            to = add<Worker>(bus, rec, "worker", silent, "", "worker")->id;
        }
        if (does == Sender::Does::Publish) {
            (void)add<Worker>(bus, rec, "second worker", silent, "", "second worker");
        }
        Sender* s = add<Sender>(bus, rec, "sender", grant_of,
                                does == Sender::Does::AsOffice ? "matchmaker" : "", does, to);
        bus.send(s->id, Message(to_value(Start{1})));
    });
}

const auto jobs_only = [](const auto&) {
    return Grant{}.allow_to_any(Job::zen_name, Job::zen_version);
};

void ask_journey(const char* name, bool defers, bool thanks) {
    journey(name, [&](Switchboard& bus, Record& rec) {
        Responder* r = add<Responder>(bus, rec, "responder", declared, "", defers);
        Asker* a = add<Asker>(bus, rec, "asker", declared, "", r->id, thanks);
        bus.send(a->id, Message(to_value(Start{1})));
        if (defers) {
            bus.drain_until_idle(); // the ask is delivered and its answer kept; then the heat
            bus.send(r->id, Message(to_value(Hot{1})));
        }
    });
}

void reply_journey() {
    journey("reply", [](Switchboard& bus, Record& rec) {
        PingResponder* r = add<PingResponder>(bus, rec, "responder", declared, "");
        PongListener* l = add<PongListener>(bus, rec, "listener", silent, "");
        bus.send(r->id, Message(to_value(Ping{7}), WeaveId{}, l->id)); // with a reply address
        bus.send(r->id, Message(to_value(Ping{8})));                   // with none
    });
}

// ---- across a link -----------------------------------------------------------------------

class FarEcho final : public WeaveBase<FarEcho, Nothing, Accept<Echo>, Emit<Echoed>> {
public:
    void on(const Echo& e, Mail& m) { (void)m.answer(Echoed{"echo: " + e.text}); }
};

class LinkAsker final
    : public WeaveBase<LinkAsker, Nothing, Accept<Start, Echoed, link::Outcome>,
                       Emit<link::Ask>> {
public:
    void on(const Start&, Mail& m) {
        (void)m.send_to_role(link::role_of("far"), link::ask_role("echo", Echo{"across"}),
                             /*correlation=*/1);
    }
    void on(const Echoed& e, Mail& m) {
        g_record->read("asker", stamps(m, "link") + ", correlation " +
                                    std::to_string(m.correlation()) + ", \"" + e.text + "\"");
        done = true;
    }
    void on(const link::Outcome& o, Mail&) {
        g_record->read("asker", "the link's outcome " + o.state);
        done = true;
    }
    std::atomic<bool> done{false};
};

bool link_journey() {
    heading("link");
    // THE FAR HOST: its own bus, one office, a bridge server admitting a session that may say
    // Echo to that office and nothing else. Stepped on its own thread, as a host would.
    Switchboard far;
    Record far_rec(far);
    auto echo = std::make_unique<FarEcho>();
    FarEcho* eraw = echo.get();
    const WeaveId echo_id = far.register_weave(std::move(echo), emit_default_grant(*eraw), "echo");
    eraw->zen_set_self(echo_id);
    far_rec.name(echo_id, "echo");
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    if (listener == kInvalidSocket) {
        std::cout << "no listener: " << err << "\n";
        return false;
    }
    const std::uint16_t port = bridge_socket_port(listener);
    auto server = std::make_unique<BridgeServer>(far, listener, [](const ConnectionRequest&) {
        ConnectionAdmitted a;
        a.grant.allow_to_role(Echo::zen_name, Echo::zen_version, "echo");
        a.established_name = "agent";
        return ConnectionVerdict::admit(std::move(a));
    });
    std::atomic<bool> stop{false};
    std::thread far_turns([&] {
        while (!stop.load()) {
            server->wait_and_step(10);
        }
    });

    // THE NEAR HOST: its bus, the link mounted as the supplied host mounts one, and an asker
    // whose grant lets it ask the link's office.
    bool answered = false;
    {
        Switchboard near;
        Record rec(near);
        g_record = &rec;
        auto l = std::make_unique<host::LinkWeave>("far", "127.0.0.1:" + std::to_string(port),
                                                   "agent", "");
        host::LinkWeave* link = l.get();
        const WeaveId link_id =
            near.register_weave(std::move(l), Grant{}.allow_any(), link::role_of("far"));
        link->zen_set_self(link_id);
        link->attach(near);
        rec.name(link_id, "link");
        std::string why;
        if (link->connect(3000, &why)) {
            for (const auto& id : far.list_weaves()) {
                if (id != echo_id) { far_rec.name(id, "session"); }
            }
            auto a = std::make_unique<LinkAsker>();
            LinkAsker* asker = a.get();
            const WeaveId asker_id = near.register_weave(
                std::move(a), Grant{}.allow_to_role(link::Ask::zen_name, link::Ask::zen_version,
                                                    link::role_of("far")));
            asker->zen_set_self(asker_id);
            rec.name(asker_id, "asker");
            near.send(asker_id, Message(to_value(Start{1})));
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!asker->done.load() && std::chrono::steady_clock::now() < until) {
                link->service();
                near.pump_pending();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            answered = asker->done.load();
        } else {
            std::cout << "the link did not connect: " << why << "\n";
        }
        std::cout << "near host:\n";
        rec.print(std::cout);
        g_record = nullptr;
    }
    stop.store(true);
    far_turns.join();
    server.reset();
    std::cout << "far host:\n";
    far_rec.print(std::cout);
    return answered;
}

} // namespace

int main() {
    sender_journey("spine", Sender::Does::Direct, declared);
    sender_journey("role", Sender::Does::ToRole, declared);
    sender_journey("office", Sender::Does::AsOffice, declared);
    sender_journey("publication", Sender::Does::Publish, declared);
    ask_journey("ask", /*defers=*/false, /*thanks=*/true);
    ask_journey("deferred", /*defers=*/true, /*thanks=*/false);
    sender_journey("refusals", Sender::Does::Refusals, jobs_only);
    reply_journey();
    return link_journey() ? 0 : 1;
}
