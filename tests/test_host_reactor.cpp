// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE HOST'S ONE WAIT (zen/host/reactor.hpp), over real handles -- an event on Windows, a pipe on
// POSIX, a crossing's sockets: claims held, woken, refused or let go; the deadline kept; a source's
// time kept on a clock that moves only when told, and once on the machine's; the first version of
// the claim answered in kind; and what the host serves itself. Times on the machine's clock are
// bounds far from either answer, so a busy machine moves no verdict.

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
using loom::host::ManualClock;
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

/// A claim of a time and nothing else.
WaitSet until(std::int64_t at) {
    WaitSet s;
    s.until_ms = at;
    return s;
}

/// A SOURCE BUILT BEFORE TIMES: it claims the first version of `WaitSet` and accepts the first
/// version of `Waited`.
class SourceV1 final
    : public WeaveBase<SourceV1, SourceState, Accept<ClaimYourWait, loom::host::WaitedV1>, Emit<>,
                       Claims<loom::host::WaitSetV1>> {
public:
    void on(const ClaimYourWait&, Mail& mail) { (void)mail.as_role(role).claim(set); }
    void on(const loom::host::WaitedV1& w, Mail& mail) {
        if (!mail.sender().valid()) {
            heard.push_back(w);
        }
    }

    std::string role;
    loom::host::WaitSetV1 set;
    std::vector<loom::host::WaitedV1> heard;
};

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
        bus.register_weave(std::move(forger),
                           Grant{}.allow_to_role(Waited::zen_name, Waited::zen_version,
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

TEST_CASE("a source due at a time is woken then and told the time, and nothing is queued for it "
          "meanwhile") {
    Switchboard bus;
    ManualClock clock(1000);
    Source* src = mount(bus, "test.timer", [] { return until(1500); });
    Reactor wait(bus, clock);
    wait.trust("test.timer");
    Reactor::Woke woke;
    (void)turn(bus, wait, 0, &woke); // the claim is made in this turn and read after it
    (void)bus.pump_pending();        // ...and the source hears that it is held, and the time
    REQUIRE(src->held());
    CHECK(src->heard.back().now_ms == 1000);
    CHECK_FALSE(src->heard.back().due);
    REQUIRE(wait.sources().front().until_ms.has_value());
    CHECK(*wait.sources().front().until_ms == 1500);

    // Between the claim and its time the bus holds nothing for it.
    CHECK(bus.pending() == 0);
    CHECK(bus.pump_pending() == 0); // a turn finds nothing to deliver

    // The host's own deadline comes first: the clock moves to it, and the source is not due.
    woke = wait.after_turn(100);
    CHECK(woke.deadline);
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 1100);
    CHECK(bus.pending() == 0);

    // With no deadline of the host's, the wait moves the clock to the source's time and wakes it.
    woke = wait.after_turn();
    CHECK_FALSE(woke.deadline);
    CHECK(woke.due == 1);
    CHECK(woke.roles == 0);
    CHECK(woke.now_ms == 1500);
    CHECK(clock.now_ms() == 1500);
    (void)bus.pump_pending();
    REQUIRE(src->heard.back().due);
    CHECK_FALSE(src->heard.back().ready);
    CHECK(src->heard.back().now_ms == 1500);
    CHECK(src->heard.back().revision ==
          static_cast<std::int64_t>(wait.sources().front().revision));
}

TEST_CASE("on the machine's clock the wait ends at a source's time, before the host's deadline") {
    Switchboard bus;
    loom::host::MachineClock machine;
    const std::int64_t at = machine.now_ms() + 50;
    Source* src = mount(bus, "test.timer", [at] { return until(at); });
    Reactor wait(bus);
    wait.trust("test.timer");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(src->held());
    Reactor::Woke woke;
    const std::int64_t took = turn(bus, wait, 5000, &woke);
    CHECK(took >= 30);
    CHECK(took < 2500);
    CHECK(woke.due == 1);
    CHECK_FALSE(woke.deadline);
    CHECK(woke.now_ms >= at);
    (void)bus.pump_pending();
    REQUIRE(src->heard.back().due);
    CHECK(src->heard.back().now_ms >= at);
}

TEST_CASE("a time is due once: a time already past is told at the next wait, and a source that "
          "does not claim again is neither told again nor spun on") {
    Switchboard bus;
    ManualClock clock(100);
    Source* src = mount(bus, "test.timer", [] { return until(0); }); // "what time is it?"
    src->takes_wakes = false;
    Reactor wait(bus, clock);
    wait.trust("test.timer");
    Reactor::Woke woke;
    (void)turn(bus, wait, 0, &woke);
    CHECK(woke.due == 1); // read after the turn that claimed it, and already past
    CHECK(clock.now_ms() == 100);
    (void)bus.pump_pending();
    REQUIRE(src->heard.back().due);
    CHECK(src->heard.back().now_ms == 100);

    // Told once: nothing waits on it now, so a wait with no deadline returns at once.
    woke = wait.after_turn();
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 100);
    CHECK_FALSE(wait.sources().front().until_ms.has_value());
    CHECK(wait.sources().front().held); // the claim stands; only its time is spent
    woke = wait.after_turn(50);
    CHECK(woke.deadline);
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 150);

    // Claiming again asks again.
    bus.send_to_role("test.timer", Message(to_value(ClaimYourWait{})));
    (void)bus.pump_pending();
    woke = wait.after_turn();
    CHECK(woke.due == 1);
}

TEST_CASE("a new claim's time replaces the one before it, and a later time is what the wait "
          "keeps") {
    Switchboard bus;
    ManualClock clock(0);
    std::int64_t at = 500;
    Source* src = mount(bus, "test.timer", [&at] { return until(at); });
    Reactor wait(bus, clock);
    wait.trust("test.timer");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(src->held());
    const std::uint64_t first = wait.sources().front().revision;
    at = 900;
    bus.send_to_role("test.timer", Message(to_value(ClaimYourWait{})));
    (void)bus.pump_pending();
    const Reactor::Woke woke = wait.after_turn();
    CHECK(woke.due == 1);
    CHECK(clock.now_ms() == 900); // never stopped at the first claim's 500
    (void)bus.pump_pending();
    REQUIRE(src->heard.back().due);
    CHECK(src->heard.back().revision > static_cast<std::int64_t>(first));
    CHECK(src->heard.back().revision ==
          static_cast<std::int64_t>(wait.sources().front().revision));
}

TEST_CASE("a time claimed by a role the host did not trust is never kept, and moves no clock") {
    Switchboard bus;
    ManualClock clock(0);
    Source* src = mount(bus, "test.stranger", [] { return until(10); });
    Reactor wait(bus, clock);
    wait.trust("test.timer");
    (void)turn(bus, wait, 0);
    Reactor::Woke woke = wait.after_turn(100);
    CHECK(woke.deadline);
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 100); // the host's deadline, never the stranger's 10
    woke = wait.after_turn();
    CHECK(clock.now_ms() == 100);
    (void)bus.pump_pending();
    CHECK(src->heard.empty());
}

TEST_CASE("a role's next holder is waited on for its own time, never its predecessor's") {
    Switchboard bus;
    ManualClock clock(0);
    (void)mount(bus, "test.timer", [] { return until(300); });
    Reactor wait(bus, clock);
    wait.trust("test.timer");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    (void)bus.unregister_weave(bus.role_holder("test.timer"));
    Source* next = mount(bus, "test.timer", [] { return WaitSet{}; });
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(next->held());
    const Reactor::Woke woke = wait.after_turn();
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 0); // nothing was due, so nothing moved it
}

TEST_CASE("on a clock that moves only when told, a ready source ends the look and moves nothing") {
    Switchboard bus;
    ManualClock clock(0);
    Signal sig;
    Source* src = mount(bus, "test.both", [&sig] {
        WaitSet s = of({sig.handle()});
        s.until_ms = 1000;
        return s;
    });
    src->on_ready = [&sig] { sig.clear(); };
    Reactor wait(bus, clock);
    wait.trust("test.both");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE(src->held());
    sig.set();
    Reactor::Woke woke = wait.after_turn();
    CHECK(woke.roles == 1);
    CHECK(woke.due == 0);
    CHECK(clock.now_ms() == 0);
    (void)bus.pump_pending(); // the source takes it and claims anew
    woke = wait.after_turn();
    CHECK(woke.roles == 0);
    CHECK(woke.due == 1);
    CHECK(clock.now_ms() == 1000);
}

TEST_CASE("a source that claims the first version is held and answered in the first version") {
    Switchboard bus;
    Signal sig;
    auto owned = std::make_unique<SourceV1>();
    SourceV1* src = owned.get();
    src->role = "test.old";
    src->set.handles = {sig.handle()};
    const WeaveId id = bus.register_weave(std::move(owned), Grant{}, "test.old");
    src->zen_set_self(id);
    bus.send(id, Message(to_value(ClaimYourWait{})));
    Reactor wait(bus);
    wait.trust("test.old");
    (void)turn(bus, wait, 0);
    (void)bus.pump_pending();
    REQUIRE_FALSE(src->heard.empty());
    CHECK(src->heard.back().held);
    CHECK(wait.sources().front().version == loom::host::WaitSetV1::zen_version);
    sig.set();
    Reactor::Woke woke;
    (void)turn(bus, wait, 5000, &woke);
    CHECK(woke.roles == 1);
    (void)bus.pump_pending();
    CHECK(src->heard.back().ready);
    CHECK(src->heard.size() == 2); // every answer reached it, in the version it accepts
}

TEST_CASE("what the host serves itself ends the wait when ready, and a handle the OS refuses is "
          "waited on no more while it is named") {
    Switchboard bus;
    Signal sig;
    Reactor wait(bus);
    const std::int64_t handle = sig.handle(); // named after it closes, as a stale set would
    wait.watch_own([handle] { return of({handle}); });
    Reactor::Woke woke;
    CHECK(turn(bus, wait, 60, &woke) >= 40);
    CHECK(woke.deadline);
    CHECK_FALSE(woke.own);

    std::thread setter([&sig] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sig.set();
    });
    const std::int64_t took = turn(bus, wait, 5000, &woke);
    setter.join();
    CHECK(took < 2500);
    CHECK(woke.own);
    CHECK_FALSE(woke.deadline);
    sig.clear();

    sig.close(); // still named
    CHECK(turn(bus, wait, 5000, &woke) < 2500); // refused at once
    CHECK(turn(bus, wait, 60, &woke) >= 40);   // and the next wait is an ordinary one
    CHECK(woke.deadline);
    CHECK_FALSE(woke.own);
}

TEST_CASE("a wait with nothing to wait on and no deadline returns at once") {
    Switchboard bus;
    Reactor wait(bus);
    Reactor::Woke woke;
    const Clock::time_point t = Clock::now();
    woke = wait.after_turn();
    CHECK(ms_since(t) < 2500);
    CHECK_FALSE(woke.deadline);
    CHECK(woke.roles == 0);
    CHECK(woke.due == 0);
}

} // TEST_SUITE("host_reactor")
