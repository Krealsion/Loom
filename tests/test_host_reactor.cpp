// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE HOST'S ONE WAIT (zen/host/reactor.hpp), over real handles -- an event on Windows, a pipe on
// POSIX, a crossing's sockets: a trusted role's claim held and woken, an untrusted one never
// read, the bound told, a closed handle and an untaken wake let go rather than spun on, the
// deadline kept, and a crossing's answer queued in a turn ending the wait at once. Times are bounds
// far from either answer, so a busy machine moves no verdict.

#include <doctest.h>

#include <zen/bridge/client.hpp>
#include <zen/bridge/server.hpp>
#include <zen/host/reactor.hpp>
#include <zen/host/wait_set.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace loom;
using loom::host::Reactor;
using loom::host::WaitSet;
using loom::host::Waited;

namespace {

/// ONE WAITABLE THING the test sets and clears: a manual-reset event, or a pipe whose read end is
/// readable while a byte is in it.
class Signal {
public:
    Signal() {
#ifdef _WIN32
        event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
#else
        int fds[2] = {-1, -1};
        REQUIRE(::pipe(fds) == 0);
        read_ = fds[0];
        write_ = fds[1];
        (void)::fcntl(read_, F_SETFL, ::fcntl(read_, F_GETFL) | O_NONBLOCK);
#endif
    }
    ~Signal() { close(); }
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    std::int64_t handle() const {
#ifdef _WIN32
        return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(event_));
#else
        return read_;
#endif
    }
    void set() {
#ifdef _WIN32
        (void)::SetEvent(event_);
#else
        const char one = 1;
        REQUIRE(::write(write_, &one, 1) == 1);
#endif
    }
    void clear() {
#ifdef _WIN32
        (void)::ResetEvent(event_);
#else
        char buf[16];
        while (::read(read_, buf, sizeof buf) > 0) {
        }
#endif
    }
    void close() {
#ifdef _WIN32
        if (event_ != nullptr) {
            (void)::CloseHandle(event_);
            event_ = nullptr;
        }
#else
        if (read_ >= 0) {
            ::close(read_);
            ::close(write_);
            read_ = write_ = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE event_ = nullptr;
#else
    int read_ = -1;
    int write_ = -1;
#endif
};

/// Asks a source to claim what it waits on, as the host would never need to: the test's start.
struct ClaimYourWait {
    ZEN_SHAPE(ClaimYourWait, 1);
};

struct SourceState {
    std::int64_t heard = 0;
    ZEN_SHAPE(SourceState, 1, ZEN_FIELD(heard));
};

/// A SOURCE as a window or a reader is one: it claims its wait set as its role, takes a wake by
/// clearing what woke it and claiming anew, and reads a `Waited` from anyone but the host as
/// nothing.
class Source final : public WeaveBase<Source, SourceState, Accept<ClaimYourWait, Waited>, Emit<>,
                                      Claims<WaitSet>> {
public:
    void on(const ClaimYourWait&, Mail& mail) { claim(mail); }
    void on(const Waited& w, Mail& mail) {
        if (mail.sender().valid()) {
            ++from_others;
            return;
        }
        heard.push_back(w);
        if (w.ready) {
            if (on_ready) {
                on_ready();
            }
            if (takes_wakes) {
                claim(mail);
            }
        }
    }
    void claim(Mail& mail) { (void)mail.as_role(role).claim(set()); }

    std::string role;
    std::function<WaitSet()> set;
    std::function<void()> on_ready;
    bool takes_wakes = true;
    std::vector<Waited> heard;
    int from_others = 0;

    std::size_t readies() const {
        std::size_t n = 0;
        for (const Waited& w : heard) {
            n += w.ready ? 1 : 0;
        }
        return n;
    }
    bool held() const { return !heard.empty() && heard.back().held; }
};

Source* mount(Switchboard& bus, std::string role, std::function<WaitSet()> set) {
    auto owned = std::make_unique<Source>();
    Source* raw = owned.get();
    raw->role = role;
    raw->set = std::move(set);
    const WeaveId id = bus.register_weave(std::move(owned), Grant{}, role);
    raw->zen_set_self(id);
    bus.send(id, Message(to_value(ClaimYourWait{})));
    return raw;
}

WaitSet of(std::vector<std::int64_t> handles) {
    WaitSet s;
    s.handles = std::move(handles);
    return s;
}

using Clock = std::chrono::steady_clock;

std::int64_t ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

/// A turn, then the wait, as a host's loop runs them; how long the wait took.
std::int64_t turn(Switchboard& bus, Reactor& wait, std::int64_t ms, Reactor::Woke* woke = nullptr) {
    (void)bus.pump_pending();
    const Clock::time_point t = Clock::now();
    const Reactor::Woke w = wait.after_turn(ms);
    if (woke != nullptr) {
        *woke = w;
    }
    return ms_since(t);
}

} // namespace

TEST_SUITE("host_reactor") {

TEST_CASE("a trusted role's source is held, woken when ready, and the wait lasts its deadline "
          "when nothing is") {
    Switchboard bus;
    Signal sig;
    Source* src = mount(bus, "test.source", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.source");

    Reactor::Woke woke;
    (void)turn(bus, wait, 0, &woke); // the claim is made in this turn and read after it
    (void)bus.pump_pending();        // ...and the source hears that it is held
    REQUIRE(src->held());
    CHECK(src->readies() == 0);

    // Nothing ready: the wait lasts its deadline.
    const std::int64_t idle = turn(bus, wait, 60, &woke);
    CHECK(woke.deadline);
    CHECK(woke.roles == 0);
    CHECK(idle >= 40);

    // Ready from another thread: the wait ends long before its deadline and wakes the role.
    std::thread setter([&sig] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sig.set();
    });
    src->on_ready = [&sig] { sig.clear(); };
    const std::int64_t took = turn(bus, wait, 5000, &woke);
    setter.join();
    CHECK(took < 2500);
    CHECK_FALSE(woke.deadline);
    CHECK(woke.roles == 1);
    (void)bus.pump_pending();
    CHECK(src->readies() == 1);
    CHECK(wait.sources().front().wakes == 1);
}

TEST_CASE("a role the host did not trust is never waited on and never told") {
    Switchboard bus;
    Signal sig;
    sig.set(); // ready from the start: a wait holding it would end at once
    Source* src = mount(bus, "test.stranger", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.someone-else");
    Reactor::Woke woke;
    (void)turn(bus, wait, 0);
    const std::int64_t took = turn(bus, wait, 60, &woke);
    (void)bus.pump_pending();
    CHECK(woke.deadline);
    CHECK(took >= 40);
    CHECK(src->heard.empty());
}

TEST_CASE("a Waited from any participant but the host is nothing to its source") {
    Switchboard bus;
    Signal sig;
    Source* src = mount(bus, "test.source", [&sig] { return of({sig.handle()}); });
    (void)bus.pump_pending();
    auto forger = std::make_unique<Source>();
    forger->role = "test.forger";
    const WeaveId fid =
        bus.register_weave(std::move(forger), Grant{}.allow_to_role("loom.host.Waited", 1,
                                                                   "test.source"),
                           "test.forger");
    Waited w;
    w.held = true;
    w.ready = true;
    (void)bus.send_as_to_role(fid, "test.source", Message(to_value(w), fid));
    (void)bus.pump_pending();
    CHECK(src->from_others == 1);
    CHECK(src->heard.empty());
}

TEST_CASE("a source that does not take its wake is held no longer, and the wait never spins on "
          "it") {
    Switchboard bus;
    Signal sig;
    sig.set(); // stays set: a source that never clears it
    Source* src = mount(bus, "test.stuck", [&sig] { return of({sig.handle()}); });
    src->takes_wakes = false; // and never claims anew
    Reactor wait(bus);
    wait.trust("test.stuck");
    Reactor::Woke woke;
    (void)turn(bus, wait, 0, &woke); // held, and ready at once: the first wake is told
    CHECK(woke.roles == 1);
    (void)turn(bus, wait, 5000, &woke); // ready again under the same claim: held no longer
    CHECK(woke.roles == 0);
    (void)bus.pump_pending();
    REQUIRE_FALSE(src->heard.empty());
    CHECK_FALSE(src->heard.back().held);
    CHECK(src->heard.back().why.find("did not take what woke it") != std::string::npos);
    // From now on the wait lasts its deadline: no spin while the claim stands.
    const std::int64_t took = turn(bus, wait, 60, &woke);
    CHECK(woke.deadline);
    CHECK(took >= 40);
    // A new claim is held again.
    bus.send_to_role("test.stuck", Message(to_value(ClaimYourWait{})));
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    CHECK(src->held());
}

TEST_CASE("a handle that closes is held no longer and stalls nothing") {
    Switchboard bus;
    Signal sig;
    Source* src = mount(bus, "test.closing", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.closing");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(src->held());
    sig.close(); // the claim still names it
    Reactor::Woke woke;
    const std::int64_t first = turn(bus, wait, 5000, &woke);
    CHECK(first < 2500); // refused at once, not waited on for the whole deadline
    (void)bus.pump_pending();
    CHECK_FALSE(src->held());
    CHECK_FALSE(wait.sources().front().held);
    const std::int64_t next = turn(bus, wait, 60, &woke);
    CHECK(woke.deadline); // and the next wait is an ordinary one, not a spin
    CHECK(next >= 40);
}

TEST_CASE("the wait holds what fits in trust order, and tells the source that does not fit why") {
    Switchboard bus;
    Signal sig;
    std::vector<std::int64_t> many(loom::host::kMaxWaitedHandles, sig.handle());
    Source* big = mount(bus, "test.big", [&many] { return of(many); });
    Source* late = mount(bus, "test.late", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.big");
    wait.trust("test.late");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    CHECK(big->held());
    REQUIRE_FALSE(late->heard.empty());
    CHECK_FALSE(late->held());
    CHECK(late->heard.back().why.find("the wait holds") != std::string::npos);
}

TEST_CASE("a claim that names nothing is held: there is nothing to poll for, and the wait lasts") {
    Switchboard bus;
    Source* src = mount(bus, "test.empty", [] { return WaitSet{}; });
    Reactor wait(bus);
    wait.trust("test.empty");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    CHECK(src->held());
    Reactor::Woke woke;
    CHECK(turn(bus, wait, 60, &woke) >= 40);
    CHECK(woke.deadline);
}

TEST_CASE("a source the host stops trusting is told it is held no longer") {
    Switchboard bus;
    Signal sig;
    Source* src = mount(bus, "test.source", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.source");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(src->held());
    wait.distrust("test.source");
    CHECK_FALSE(wait.trusts("test.source"));
    (void)bus.pump_pending();
    CHECK_FALSE(src->held());
    CHECK(src->heard.back().why == "the host no longer waits on this role");
}

TEST_CASE("a role's next holder starts unheld and is told for itself") {
    Switchboard bus;
    Signal sig;
    Source* first = mount(bus, "test.source", [&sig] { return of({sig.handle()}); });
    Reactor wait(bus);
    wait.trust("test.source");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(first->held());
    const WeaveId gone = bus.role_holder("test.source");
    (void)bus.unregister_weave(gone);
    Source* next = mount(bus, "test.source", [&sig] { return of({sig.handle()}); });
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    CHECK(next->held());
}

TEST_CASE("a crossing is waited on: a connection, its Hello and an answer queued in a turn each "
          "end the wait at once, and an idle connection lets it last") {
    Switchboard bus;
    std::string err;
    REQUIRE(bridge_net_init(&err));
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE(listener != kInvalidSocket);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);
    Source* door = mount(bus, "test.door", [&server] { return server.wait_set(); });
    door->on_ready = [&server] { server.service(); };
    Reactor wait(bus);
    wait.trust("test.door");
    // Asking first turns waiting on, and says once what was owed before it.
    Reactor::Woke woke;
    (void)turn(bus, wait, 0);
    (void)turn(bus, wait, 0);
    (void)turn(bus, wait, 0);
    CHECK(turn(bus, wait, 60, &woke) >= 40);
    CHECK(woke.deadline);

    // A peer connects and says Hello from a thread of its own, as a peer does: the wait ends for
    // each, and service admits it.
    BridgeClient client(bridge_connect_tcp("127.0.0.1", port, &err));
    REQUIRE(client.hello("guest", ""));
    std::atomic<bool> admitted{false};
    std::thread peer([&client, &admitted] {
        const Clock::time_point began = Clock::now();
        while (!client.admitted() && ms_since(began) < 10000) {
            std::vector<BridgeEvent> got;
            client.poll(got); // flushes the Hello once the connection stands
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        admitted = client.admitted();
    });
    const auto admitted_here = [&server] {
        for (const Connection& c : server.connections()) {
            if (c.state == ConnectionState::Admitted) {
                return true;
            }
        }
        return false;
    };
    const Clock::time_point t = Clock::now();
    int turns = 0;
    for (; turns < 8; ++turns) {
        (void)bus.pump_pending(); // a service here may admit it
        if (admitted_here()) {
            break;
        }
        (void)wait.after_turn(5000);
    }
    CHECK(ms_since(t) < 2500); // a handful of waits, none of them its deadline
    peer.join();
    REQUIRE(client.admitted());

    // Idle and connected: the wait lasts.
    for (int i = 0; i < 3; ++i) {
        (void)turn(bus, wait, 0);
    }
    CHECK(turn(bus, wait, 60, &woke) >= 40);
    CHECK(woke.deadline);

    // An answer to the session, queued in a turn: the wait ends at once, and service ships it.
    Waited sent;
    sent.why = "an answer";
    bus.send(WeaveId{client.session()}, Message(to_value(sent)));
    const std::int64_t took = turn(bus, wait, 5000, &woke);
    CHECK(took < 2500);
    CHECK(woke.roles == 1);
    (void)bus.pump_pending(); // the door services
    bool delivered = false;
    const Clock::time_point d = Clock::now();
    while (!delivered && ms_since(d) < 5000) {
        std::vector<BridgeEvent> got;
        client.poll(got);
        for (const BridgeEvent& e : got) {
            delivered = delivered || e.kind == BridgeEvent::Kind::Delivered;
        }
        if (!delivered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(delivered);
}

} // TEST_SUITE("host_reactor")
