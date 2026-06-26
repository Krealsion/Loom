#include <doctest.h>

#include <zen/bridge/channel.hpp>
#include <zen/bridge/remote_console.hpp>
#include <zen/bridge/server.hpp>

#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/value.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// The remote-operator bridge, proven at the mechanism altitude (the prompt's altitude 1): two
// endpoints over a REAL socket — the same path the Windows->WSL crossing uses (127.0.0.1; WSL2
// forwards localhost). The transport framing, then the operator-protocol (discovery + tap + send as
// messages), the connection-stamped sender against a FORGED wire frame, and disconnect handled as an
// event (close + a genuine SIGKILL of a real peer process). The crossing itself (altitude 2) is the
// Windows console driving this same server.

using namespace loom;

namespace {

// ---- a demo weave: echoes Greet and records the STAMPED sender it was sent as ------------------

std::shared_ptr<const loom::Schema> greet_schema() {
    static const auto s = loom::SchemaBuilder("Greet", 1).field("msg", loom::Kind::Text).build();
    return s;
}

class RecordingGreeter final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const loom::Message& in, loom::Bus& bus) override {
        last_sender_.store(in.sender.value); // the sender the bus delivered (the bridge's stamp)
        loom::Value v(greet_schema());
        const loom::Cell* m = in.payload.get("msg");
        v.set("msg", loom::Cell::text(m != nullptr ? m->as_text() : std::string()));
        bus.send(in.reply_to, loom::Message(std::move(v)));
    }
    loom::Value snapshot() const override {
        loom::Value v(state_schema());
        v.set("n", loom::Cell::integer(0));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

    std::uint64_t last_sender() const noexcept { return last_sender_.load(); }

private:
    std::atomic<std::uint64_t> last_sender_{0};
    static std::shared_ptr<const loom::Schema> state_schema() {
        static const auto s =
            loom::SchemaBuilder("GreeterState", 1).field("n", loom::Kind::Int).build();
        return s;
    }
};

// Spin `predicate` (with a brief grace) until it holds or the timeout elapses; returns its final value.
bool wait_until(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A host running on its own thread (the "second process"): a bus with a RecordingGreeter, a TCP
// listener on an OS-chosen port, and a BridgeServer driving its event-driven multiplexer. The bus is
// touched ONLY by this thread; the test's main thread drives a RemoteConsole over the socket and
// reads only the greeter's atomic. This faithfully models the two-process deployment with clean
// assertions — and the server's blocking request/replies resolve because this thread is stepping.
struct Host {
    loom::Switchboard bus;
    RecordingGreeter* greeter = nullptr;
    loom::WeaveId gid{};
    socket_t listener = kInvalidSocket;
    std::uint16_t port = 0;
    std::string err;
    std::unique_ptr<loom::BridgeServer> server;
    std::atomic<bool> stop{false};
    std::thread th;

    Host() {
        auto g = std::make_unique<RecordingGreeter>();
        greeter = g.get();
        gid = bus.register_weave(std::move(g), loom::Grant{}.allow_any());
        listener = bridge_listen_tcp(0, &err);
        REQUIRE_MESSAGE(listener != kInvalidSocket, err);
        port = bridge_socket_port(listener);
        REQUIRE(port != 0);
        server = std::make_unique<loom::BridgeServer>(bus, listener);
        th = std::thread([this] {
            while (!stop.load()) {
                server->wait_and_step(20); // event-driven: wakes on a ready socket or the tick
            }
        });
    }
    ~Host() {
        stop.store(true);
        if (th.joinable()) {
            th.join();
        }
    }
};

} // namespace

TEST_SUITE_BEGIN("bridge");

// ---- the transport, in isolation ---------------------------------------------------------------

namespace {

struct Pair {
    std::unique_ptr<BridgeChannel> a;
    std::unique_ptr<BridgeChannel> b;
};

Pair make_pair() {
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    REQUIRE(port != 0);
    const socket_t client = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(client != kInvalidSocket, err);
    socket_t accepted = kInvalidSocket;
    for (int i = 0; i < 500 && accepted == kInvalidSocket; ++i) {
        bool would_block = false;
        accepted = bridge_accept(listener, &would_block, &err);
        if (accepted == kInvalidSocket && !would_block) {
            FAIL(err);
        }
        if (accepted == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(accepted != kInvalidSocket);
    bridge_close(listener);
    Pair p;
    p.a = std::make_unique<BridgeChannel>(client);
    p.b = std::make_unique<BridgeChannel>(accepted);
    return p;
}

std::vector<BridgeIncoming> drain(BridgeChannel& ch, std::size_t want) {
    std::vector<BridgeIncoming> got;
    for (int i = 0; i < 1000 && got.size() < want; ++i) {
        ch.poll(got);
        if (got.size() < want && !ch.done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (ch.done()) {
            break;
        }
    }
    return got;
}

} // namespace

TEST_CASE("transport: framed messages round-trip intact over a real loopback socket") {
    Pair p = make_pair();
    const std::string big(20000, 'x'); // exceeds the 8192 recv chunk -> exercises reassembly
    p.a->queue(BridgeOp::Hello, "");
    p.a->queue(BridgeOp::Describe, "Ping\x01\x00\x00\x00");
    p.a->queue(BridgeOp::Send, big);
    p.a->flush();
    CHECK_FALSE(p.a->failed());

    const std::vector<BridgeIncoming> got = drain(*p.b, 3);
    REQUIRE(got.size() == 3);
    CHECK(got[0].op == BridgeOp::Hello);
    CHECK(got[0].payload.empty());
    CHECK(got[1].op == BridgeOp::Describe);
    CHECK(got[1].payload == "Ping\x01\x00\x00\x00");
    CHECK(got[2].op == BridgeOp::Send);
    CHECK(got[2].payload == big);
}

TEST_CASE("transport: a closed peer surfaces as eof (the disconnect-as-an-event signal)") {
    Pair p = make_pair();
    p.a->queue(BridgeOp::Welcome, "hi");
    p.a->flush();
    p.a.reset(); // closes the client socket

    std::vector<BridgeIncoming> got;
    bool saw_eof = false;
    for (int i = 0; i < 1000; ++i) {
        p.b->poll(got);
        if (p.b->eof()) {
            saw_eof = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(saw_eof);
    CHECK(p.b->done());
    REQUIRE(got.size() == 1);
    CHECK(got[0].op == BridgeOp::Welcome);
    CHECK(got[0].payload == "hi");
}

// ---- the operator-protocol (discovery + tap + send as messages) --------------------------------

TEST_CASE("operator-protocol: discovery, a gate-sent message, and the reply buffer cross the wire") {
    Host h;
    socket_t cs = bridge_connect_tcp("127.0.0.1", h.port, &h.err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, h.err);

    RemoteConsole rc(cs); // the SAME engine surface the in-process TUI drives — over the socket
    REQUIRE(rc.connected());
    CHECK(rc.operator_id().value != 0);

    // Discovery: the greeter is visible with its accepted shape (pushed by the host after Welcome).
    REQUIRE(wait_until(
        [&] {
            rc.pump();
            return !rc.weaves().empty();
        },
        2000));
    const std::vector<WeaveInfo> weaves = rc.weaves();
    REQUIRE(weaves.size() == 1);
    CHECK(weaves[0].id.value == h.gid.value);
    REQUIRE(weaves[0].accepts.size() == 1);
    CHECK(weaves[0].accepts[0].name == "Greet");

    // Describe: the shape is fetched over the wire (encoded schema -> reconstructed client-side).
    const std::optional<ShapeDesc> desc = rc.describe("Greet", 1);
    REQUIRE(desc.has_value());
    CHECK(desc->name == "Greet");
    REQUIRE(desc->fields.size() == 1);
    CHECK(desc->fields[0].name == "msg");
    CHECK(desc->fields[0].type == "Text");

    // Compose + send: the assumption ladder runs CLIENT-side; the assembled bytes ship as a Send.
    Arg arg;
    arg.name = "msg";
    arg.value = FieldValue{std::string("hello")};
    const Composed c = rc.compose(h.gid, "Greet", 1, {arg});
    CHECK(c.status == Composed::Status::Ready);

    // The reply buffers as m1 and carries the echoed payload (the round-trip closed over the bus).
    REQUIRE(wait_until(
        [&] {
            rc.pump();
            return rc.buffer_size() >= 1;
        },
        2000));
    const std::optional<BufferEntry> m1 = rc.buffer_at(1);
    REQUIRE(m1.has_value());
    CHECK(m1->name == "Greet");
    REQUIRE(m1->value.get("msg") != nullptr);
    CHECK(m1->value.get("msg")->as_text() == "hello");

    // The tap streamed the bus events (at least the Delivered to the operator).
    bool saw_tap = false;
    for (const TapEvent& e : rc.tap()) {
        if (e.kind == "Delivered") {
            saw_tap = true;
        }
    }
    CHECK(saw_tap);

    // Provenance: the greeter saw the operator's STAMPED sender (its proxy id == operator_id).
    CHECK(h.greeter->last_sender() == rc.operator_id().value);
}

TEST_CASE("operator-protocol: the sender is stamped from the connection — a FORGED wire sender loses") {
    Host h;
    socket_t cs = bridge_connect_tcp("127.0.0.1", h.port, &h.err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, h.err);

    // A RAW client — it bypasses RemoteConsole (which hard-codes wire_sender=0) to MANUFACTURE the
    // hostile frame an honest client cannot express. This is the unsayable-attack discipline: the
    // safe API makes the attack unsayable, so the test forges the wire itself.
    BridgeChannel raw(cs);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    raw.queue(BridgeOp::Hello, hello);
    raw.flush();

    std::uint64_t operator_id = 0;
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> frames;
            raw.poll(frames);
            for (const BridgeIncoming& f : frames) {
                if (f.op == BridgeOp::Welcome) {
                    Cursor cur(f.payload);
                    std::uint64_t id = 0;
                    std::uint32_t proto = 0;
                    if (cur.u64(id) && cur.u32(proto)) {
                        operator_id = id;
                    }
                }
            }
            return operator_id != 0;
        },
        2000));

    // Forge a Send whose WIRE sender + reply_to both claim a different victim id.
    const std::uint64_t kVictim = 999999;
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("forged"));
    std::string frame;
    put_u8(frame, kEmitSend);
    put_u64(frame, kVictim);      // forged wire_sender — the bridge MUST ignore this
    put_u64(frame, h.gid.value);  // target: the greeter
    put_u64(frame, kVictim);      // forged wire_reply_to — the bridge MUST ignore this too
    put_u64(frame, 1);            // correlation
    frame.append(loom::serialize(greet));
    raw.queue(BridgeOp::Send, frame);
    raw.flush();

    // The greeter recorded the STAMPED sender (this connection's operator id), never the forged one.
    REQUIRE(wait_until([&] { return h.greeter->last_sender() != 0; }, 2000));
    CHECK(h.greeter->last_sender() == operator_id);
    CHECK(h.greeter->last_sender() != kVictim);

    // And the echoed reply came back to THIS operator — the forged wire_reply_to did NOT redirect it
    // to the victim (the reply_to is stamped from the connection too; the confused-deputy guard).
    bool got_reply = false;
    (void)wait_until(
        [&] {
            std::vector<BridgeIncoming> frames;
            raw.poll(frames);
            for (const BridgeIncoming& f : frames) {
                if (f.op == BridgeOp::Delivered) {
                    got_reply = true;
                }
            }
            return got_reply;
        },
        2000);
    CHECK(got_reply);
}

// ---- disconnect handled as an event (no hang) --------------------------------------------------

TEST_CASE("operator-protocol: a vanished peer is reaped as an event (the server unregisters its proxy)") {
    // Single-threaded here so connection_count is asserted deterministically after the peer vanishes.
    loom::Switchboard bus;
    bus.register_weave(std::make_unique<RecordingGreeter>(), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);

    socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, err);
    {
        BridgeChannel raw(cs);
        std::string hello;
        put_u32(hello, kBridgeProtocolVersion);
        raw.queue(BridgeOp::Hello, hello);
        raw.flush();
        bool registered = false;
        for (int i = 0; i < 1000 && !registered; ++i) {
            server.step();
            if (server.connection_count() >= 1) {
                registered = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        CHECK(registered);
    } // raw destroyed -> the peer socket closes -> the server's accepted end sees EOF (== a dead peer)

    bool reaped = false;
    for (int i = 0; i < 1000 && !reaped; ++i) {
        server.step(); // never blocks; observes EOF and unregisters the proxy
        if (server.connection_count() == 0) {
            reaped = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(reaped);
}

#ifndef _WIN32
TEST_CASE("operator-protocol: a SIGKILLed operator PROCESS is reaped as an event (two real processes)") {
    loom::Switchboard bus;
    bus.register_weave(std::make_unique<RecordingGreeter>(), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child (the operator process): drop the inherited listener, connect, Hello, then wait to be
        // killed. It touches only the socket — never the parent's bus.
        bridge_close(listener);
        std::string e2;
        const socket_t cs = bridge_connect_tcp("127.0.0.1", port, &e2);
        if (cs == kInvalidSocket) {
            ::_exit(2);
        }
        BridgeChannel ch(cs);
        std::string hello;
        put_u32(hello, kBridgeProtocolVersion);
        ch.queue(BridgeOp::Hello, hello);
        ch.flush();
        for (;;) {
            ::pause(); // block until SIGKILL (uncatchable: no cleanup, like a real crash)
        }
    }

    BridgeServer server(bus, listener);
    bool registered = false;
    for (int i = 0; i < 3000 && !registered; ++i) {
        server.step();
        if (server.connection_count() >= 1) {
            registered = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(registered);

    ::kill(pid, SIGKILL); // KILL the far side
    int status = 0;
    ::waitpid(pid, &status, 0);

    bool reaped = false;
    for (int i = 0; i < 3000 && !reaped; ++i) {
        server.step(); // graceful: observes the EOF and reaps — never a hang
        if (server.connection_count() == 0) {
            reaped = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(reaped);
}
#endif // _WIN32

TEST_SUITE_END();
