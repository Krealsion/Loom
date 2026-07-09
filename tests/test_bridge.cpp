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
#include <string_view>
#include <thread>
#include <utility>
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

// A shape the test bus NEVER registers -> resolve_schema returns null -> "unknown schema" refusal.
std::shared_ptr<const loom::Schema> unknown_schema() {
    static const auto s =
        loom::SchemaBuilder("NopeUnknownShape", 1).field("x", loom::Kind::Int).build();
    return s;
}

// Build a raw Send frame: [u8 kind][u64 wire_sender][u64 target][u64 wire_reply_to][u64 corr][payload].
std::string make_send_frame(std::uint64_t wire_sender, std::uint64_t target,
                            std::uint64_t wire_reply_to, std::uint64_t correlation,
                            std::string_view payload) {
    std::string frame;
    put_u8(frame, kEmitSend);
    put_u64(frame, wire_sender);
    put_u64(frame, target);
    put_u64(frame, wire_reply_to);
    put_u64(frame, correlation);
    frame.append(payload);
    return frame;
}

// Two connected raw sockets over TCP loopback (for driving a RemoteConsole against a fake host).
std::pair<socket_t, socket_t> two_sockets() {
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    const socket_t client = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(client != kInvalidSocket, err);
    socket_t accepted = kInvalidSocket;
    for (int i = 0; i < 500 && accepted == kInvalidSocket; ++i) {
        bool wb = false;
        accepted = bridge_accept(listener, &wb, &err);
        if (accepted == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(accepted != kInvalidSocket);
    bridge_close(listener);
    return {client, accepted};
}

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

#ifndef _WIN32
TEST_CASE("transport: framed messages round-trip over AF_UNIX (decision #4's local transport, POSIX)") {
    // AF_UNIX gains its live consumer: DESIGN.md/decision-#4 say the fast local loop IS unix, so
    // exercise it. (The crossing uses TCP; AF_UNIX is POSIX-only, hence the gate.)
    const std::string path = "/tmp/zen-bridge-hygiene-af-unix.sock";
    std::string err;
    const socket_t listener = bridge_listen_unix(path, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const socket_t client = bridge_connect_unix(path, &err);
    REQUIRE_MESSAGE(client != kInvalidSocket, err);
    socket_t accepted = kInvalidSocket;
    for (int i = 0; i < 500 && accepted == kInvalidSocket; ++i) {
        bool wb = false;
        accepted = bridge_accept(listener, &wb, &err);
        if (accepted == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(accepted != kInvalidSocket);
    bridge_close(listener);

    BridgeChannel a(client);
    BridgeChannel b(accepted);
    a.queue(BridgeOp::Hello, "");
    a.queue(BridgeOp::Send, "over a unix socket");
    a.flush();
    const std::vector<BridgeIncoming> got = drain(b, 2);
    REQUIRE(got.size() == 2);
    CHECK(got[0].op == BridgeOp::Hello);
    CHECK(got[1].op == BridgeOp::Send);
    CHECK(got[1].payload == "over a unix socket");
    ::unlink(path.c_str());
}
#endif // _WIN32

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

// ---- hygiene: the squared edges ----------------------------------------------------------------

TEST_CASE("hygiene: the connection cap sheds past kMax (a reconnecting fd-hog is contained)") {
    loom::Switchboard bus;
    const loom::WeaveId gid =
        bus.register_weave(std::make_unique<RecordingGreeter>(), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);

    const std::size_t over = BridgeServer::kMaxOperatorConnections + 1;
    std::vector<std::unique_ptr<BridgeChannel>> clients;
    for (std::size_t i = 0; i < over; ++i) {
        const socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
        REQUIRE_MESSAGE(cs != kInvalidSocket, err);
        auto ch = std::make_unique<BridgeChannel>(cs);
        std::string hello;
        put_u32(hello, kBridgeProtocolVersion);
        ch->queue(BridgeOp::Hello, hello);
        ch->flush();
        clients.push_back(std::move(ch));
        server.step(); // drain the accept queue each connect so the listen backlog never overflows
    }
    for (int i = 0; i < 2000 && server.declined_count() == 0; ++i) {
        server.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.connection_count() == BridgeServer::kMaxOperatorConnections);
    CHECK(server.declined_count() == 1);

    // The cap bounds the BUS, not just conns_: exactly kMax proxies (plus the greeter).
    std::size_t proxies = 0;
    for (loom::WeaveId id : bus.list_weaves()) {
        if (id.value != gid.value) {
            ++proxies;
        }
    }
    CHECK(proxies == BridgeServer::kMaxOperatorConnections);

    // Exactly one client — the shed one — observes a closed socket.
    std::size_t shed = 0;
    for (int round = 0; round < 500 && shed == 0; ++round) {
        for (auto& ch : clients) {
            std::vector<BridgeIncoming> fr;
            ch->poll(fr);
        }
        for (auto& ch : clients) {
            if (ch->done()) {
                ++shed;
            }
        }
        if (shed == 0) {
            server.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(shed == 1);
}

TEST_CASE("hygiene: a valid frame BEFORE Hello severs the connection (anti-Postel)") {
    loom::Switchboard bus;
    auto g = std::make_unique<RecordingGreeter>();
    RecordingGreeter* greeter = g.get();
    const loom::WeaveId gid = bus.register_weave(std::move(g), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);

    const socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, err);
    {
        BridgeChannel raw(cs);
        // A WELL-FORMED Greet Send — the violation is the ORDERING (before Hello), not the frame.
        loom::Value greet(greet_schema());
        greet.set("msg", loom::Cell::text("premature"));
        raw.queue(BridgeOp::Send, make_send_frame(0, gid.value, 0, 1, loom::serialize(greet)));
        raw.flush();
        bool accepted_once = false;
        for (int i = 0; i < 1000; ++i) {
            server.step();
            if (server.connection_count() >= 1) {
                accepted_once = true;
            }
            if (accepted_once && server.connection_count() == 0) {
                break; // severed + reaped
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(greeter->last_sender() == 0); // the pre-Hello Send never reached send_as
    CHECK(server.connection_count() == 0);
    CHECK(bus.list_weaves().size() == 1u); // the proxy was unregistered — only the greeter remains
}

TEST_CASE("hygiene: hostile Sends post-Hello are refused (SendRefused), the connection survives") {
    Host h;
    const socket_t cs = bridge_connect_tcp("127.0.0.1", h.port, &h.err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, h.err);
    BridgeChannel raw(cs);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    raw.queue(BridgeOp::Hello, hello);
    raw.flush();
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> fr;
            raw.poll(fr);
            for (const BridgeIncoming& f : fr) {
                if (f.op == BridgeOp::Welcome) {
                    return true;
                }
            }
            return false;
        },
        2000));

    // Wait for the next SendRefused frame and return its reason (correlation parsed to reach it).
    auto refusal_reason = [&]() -> std::string {
        std::string reason;
        bool got = false;
        (void)wait_until(
            [&] {
                std::vector<BridgeIncoming> fr;
                raw.poll(fr);
                for (const BridgeIncoming& f : fr) {
                    if (f.op == BridgeOp::SendRefused) {
                        Cursor c(f.payload);
                        std::uint64_t corr = 0;
                        std::string_view r;
                        if (c.u64(corr) && c.bytes(r)) {
                            reason = std::string(r);
                            got = true;
                        }
                    }
                }
                return got;
            },
            2000);
        return reason;
    };

    // 1. Truncated header (too short to parse the Send header -> correlation 0).
    raw.queue(BridgeOp::Send, std::string("\x00", 1));
    raw.flush();
    CHECK(refusal_reason().find("malformed Send header") != std::string::npos);

    // 2. Unknown schema: a well-formed Value of a shape the bus never registered.
    loom::Value unk(unknown_schema());
    unk.set("x", loom::Cell::integer(1));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 2, loom::serialize(unk)));
    raw.flush();
    CHECK(refusal_reason().find("unknown schema") != std::string::npos);

    // 3. A Greet v1 CLAIM over a garbage body (header intact, body truncated -> the gate refuses).
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("x"));
    std::string bad = loom::serialize(greet);
    REQUIRE(bad.size() > 1);
    bad.resize(bad.size() - 1);
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 3, bad));
    raw.flush();
    CHECK(refusal_reason().find("gate refused") != std::string::npos);

    // Not one hostile Send reached the bus.
    CHECK(h.greeter->last_sender() == 0);

    // The connection SURVIVED (per-frame refusals are non-fatal): a subsequent honest Send delivers.
    greet.set("msg", loom::Cell::text("ok"));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 4, loom::serialize(greet)));
    raw.flush();
    CHECK(wait_until([&] { return h.greeter->last_sender() != 0; }, 2000));
}

TEST_CASE("hygiene: a hostile host cannot inject an unbuildable reply — it is refused, not buffered") {
    // The forge is NECESSARY: an honest BridgeServer NEVER ships a Delivered whose schema it has not
    // published — it stamps replies from real bus Values whose schemas ARE registered. Only a FAKE
    // host can manufacture a Delivered-without-a-registered-schema, so the client's defense is
    // testable only by forging the host. (If an honest server could express it, that would be a finding.)
    const std::pair<socket_t, socket_t> pair = two_sockets();
    RemoteConsole rc(pair.first, /*handshake_timeout_ms=*/0); // non-blocking: the test drives both ends
    BridgeChannel host(pair.second);

    std::string welcome;
    put_u64(welcome, 7);
    put_u32(welcome, kBridgeProtocolVersion);
    host.queue(BridgeOp::Welcome, welcome);
    host.flush();
    REQUIRE(wait_until(
        [&] {
            host.flush();
            rc.pump();
            return rc.connected();
        },
        2000));

    loom::Value unk(unknown_schema());
    unk.set("x", loom::Cell::integer(9));
    host.queue(BridgeOp::Delivered, loom::serialize(unk));
    host.flush();

    // The client requests Describe (pending the reply); the fake host answers SchemaNone.
    bool answered = false;
    for (int i = 0; i < 1000 && !answered; ++i) {
        rc.pump();
        std::vector<BridgeIncoming> fr;
        host.poll(fr);
        for (const BridgeIncoming& f : fr) {
            if (f.op == BridgeOp::Describe) {
                Cursor c(f.payload);
                std::string_view name;
                std::uint32_t ver = 0;
                if (c.bytes(name) && c.u32(ver)) {
                    std::string body;
                    put_bytes(body, name);
                    put_u32(body, ver);
                    host.queue(BridgeOp::SchemaNone, body);
                    host.flush();
                    answered = true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(answered);
    for (int i = 0; i < 200; ++i) {
        rc.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rc.buffer_size() == 0); // the unbuildable reply never entered the buffer
    bool refused = false;
    for (const TapEvent& e : rc.tap()) {
        if (e.kind == "BridgeRefused") {
            refused = true;
        }
    }
    CHECK(refused);
}

TEST_CASE("hygiene: the client bounds pending replies a hostile host can pile up") {
    const std::pair<socket_t, socket_t> pair = two_sockets();
    RemoteConsole rc(pair.first, /*handshake_timeout_ms=*/0);
    BridgeChannel host(pair.second);
    std::string welcome;
    put_u64(welcome, 7);
    put_u32(welcome, kBridgeProtocolVersion);
    host.queue(BridgeOp::Welcome, welcome);
    host.flush();
    REQUIRE(wait_until(
        [&] {
            host.flush();
            rc.pump();
            return rc.connected();
        },
        2000));

    // Flood kMaxPendingDelivered + 1 unknown-schema Delivereds BEFORE answering any Describe.
    loom::Value unk(unknown_schema());
    unk.set("x", loom::Cell::integer(1));
    const std::string bytes = loom::serialize(unk);
    for (std::size_t i = 0; i < RemoteConsole::kMaxPendingDelivered + 1; ++i) {
        host.queue(BridgeOp::Delivered, bytes);
    }
    host.flush();
    for (int i = 0; i < 500; ++i) {
        rc.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rc.buffer_size() == 0);
    bool overflow = false;
    for (const TapEvent& e : rc.tap()) {
        if (e.kind == "BridgeRefused" && e.refusal.find("pending-overflow") != std::string::npos) {
            overflow = true;
        }
    }
    CHECK(overflow);
}

// ---- malformed-input hardening: three forged frames (coverage, not a fix) ----------------------
//
// An honest RemoteConsole composes against a real schema, so it can NEVER emit a malformed frame —
// a test through the honest client cannot reach these paths at all. So each case FORGES the hostile
// wire-frame by hand via a raw BridgeChannel (Cases 1-2) or bridge_send_raw (Case 3's lying length),
// exactly as the sender-forge test does. Four assertions each: rejected / no-leak / connection-
// survives / no-hang-crash-desync — the cluster that makes these BRIDGE tests, not just admit() tests.
// (This is NOT a fuzzer: three representative frames pin the mechanism; wire-fuzzing is the seam tied
// to actual off-host network exposure, which the bridge is explicitly not built for.)

TEST_CASE("hardening (value, known schema): a corrupt body is gate-refused, no leak, connection survives") {
    Host h;
    const socket_t cs = bridge_connect_tcp("127.0.0.1", h.port, &h.err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, h.err);
    BridgeChannel raw(cs);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    raw.queue(BridgeOp::Hello, hello);
    raw.flush();
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> fr;
            raw.poll(fr);
            for (const BridgeIncoming& f : fr) {
                if (f.op == BridgeOp::Welcome) {
                    return true;
                }
            }
            return false;
        },
        2000));

    auto next_refusal = [&]() -> std::string {
        std::string reason;
        (void)wait_until(
            [&] {
                std::vector<BridgeIncoming> fr;
                raw.poll(fr);
                for (const BridgeIncoming& f : fr) {
                    if (f.op == BridgeOp::SendRefused) {
                        Cursor c(f.payload);
                        std::uint64_t corr = 0;
                        std::string_view r;
                        if (c.u64(corr) && c.bytes(r)) {
                            reason = std::string(r);
                        }
                        return true;
                    }
                }
                return false;
            },
            2000);
        return reason;
    };

    // Forge a well-FRAMED Send whose payload claims Greet v1 (REGISTERED) but whose body is corrupt for
    // that schema: resolve_schema finds Greet, then admit() refuses the body -> the "gate refused"
    // branch (admit's malformed-value path). The header/claim stay intact so this is NOT Case 2's
    // unknown-schema branch.
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("hardening"));
    std::string body = loom::serialize(greet);
    REQUIRE(body.size() > 2);
    body.resize(body.size() - 2); // truncate the body; the field can no longer decode
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 11, body));
    raw.flush();

    CHECK(next_refusal().find("gate refused") != std::string::npos); // (1) rejected, admit branch named
    CHECK(h.greeter->last_sender() == 0);                            // (2) no leak — the weave never saw it

    // (3)+(4) connection survives + stream in sync: a subsequent HONEST Send delivers.
    greet.set("msg", loom::Cell::text("ok"));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 12, loom::serialize(greet)));
    raw.flush();
    CHECK(wait_until([&] { return h.greeter->last_sender() != 0; }, 2000));
}

TEST_CASE("hardening (value, unknown schema): a distinct branch is refused, no leak, connection survives") {
    Host h;
    const socket_t cs = bridge_connect_tcp("127.0.0.1", h.port, &h.err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, h.err);
    BridgeChannel raw(cs);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    raw.queue(BridgeOp::Hello, hello);
    raw.flush();
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> fr;
            raw.poll(fr);
            for (const BridgeIncoming& f : fr) {
                if (f.op == BridgeOp::Welcome) {
                    return true;
                }
            }
            return false;
        },
        2000));

    // A well-formed Value of a shape the bus NEVER registered: resolve_schema returns null -> the
    // "unknown schema" branch, DISTINCT from Case 1's admit-refused branch (both pinned so neither
    // stands in for the other). Forged by hand — the honest client only composes registered shapes.
    loom::Value unk(unknown_schema());
    unk.set("x", loom::Cell::integer(42));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 21, loom::serialize(unk)));
    raw.flush();

    std::string reason;
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> fr;
            raw.poll(fr);
            for (const BridgeIncoming& f : fr) {
                if (f.op == BridgeOp::SendRefused) {
                    Cursor c(f.payload);
                    std::uint64_t corr = 0;
                    std::string_view r;
                    if (c.u64(corr) && c.bytes(r)) {
                        reason = std::string(r);
                    }
                    return true;
                }
            }
            return false;
        },
        2000));
    CHECK(reason.find("unknown schema") != std::string::npos); // (1) rejected, the resolve-null branch
    CHECK(h.greeter->last_sender() == 0);                       // (2) no leak

    // (3)+(4) survives + in sync: an honest Send delivers.
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("ok"));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 22, loom::serialize(greet)));
    raw.flush();
    CHECK(wait_until([&] { return h.greeter->last_sender() != 0; }, 2000));
}

TEST_CASE("hardening (framing): garbage at the transport layer — the framer, not admit(), handles it") {
    // The important case: a frame malformed at the PROTOCOL level. admit() never sees this — it is the
    // BridgeChannel FRAMER that must handle it. Single-threaded so connection_count() is deterministic.
    loom::Switchboard bus;
    auto g = std::make_unique<RecordingGreeter>();
    RecordingGreeter* greeter = g.get();
    const loom::WeaveId gid = bus.register_weave(std::move(g), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);

    const socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, err);
    BridgeChannel raw(cs);
    {
        std::string hello;
        put_u32(hello, kBridgeProtocolVersion);
        raw.queue(BridgeOp::Hello, hello); // handshake first (a bogus op BEFORE Hello would sever)
        raw.flush();
    }
    bool up = false;
    for (int i = 0; i < 1000 && !up; ++i) {
        server.step();
        if (server.connection_count() >= 1) {
            up = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(up);

    // A COMPLETE, well-framed frame whose OPCODE is garbage (200 — not a BridgeOp). queue() writes an
    // honest length, so the framer parses it in-bounds and advances correctly; on_frame's switch
    // matches no case -> the frame is dropped with no effect. admit() never sees it.
    raw.queue(static_cast<BridgeOp>(200), std::string("garbage-opcode-body"));
    raw.flush();
    for (int i = 0; i < 100; ++i) {
        server.step(); // give the bogus frame time to arrive + be processed
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.connection_count() == 1);  // (3) the connection SURVIVES garbage framing
    CHECK(bus.list_weaves().size() == 2u);  //     the proxy is still registered (greeter + 1 proxy)
    CHECK(greeter->last_sender() == 0);     // (2) no leak — nothing reached the weave

    // (4) NO DESYNC: a subsequent well-formed Send on the SAME connection still delivers.
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("after-garbage"));
    raw.queue(BridgeOp::Send, make_send_frame(0, gid.value, 0, 31, loom::serialize(greet)));
    raw.flush();
    bool delivered = false;
    for (int i = 0; i < 1000 && !delivered; ++i) {
        server.step();
        if (greeter->last_sender() != 0) {
            delivered = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(delivered);                       // (1)+(4) the framer stayed in sync across the bad frame
    CHECK(server.connection_count() == 1);

    // (4, over-read safety) The framer's LENGTH parser — the real buffer-over-read risk, which admit()
    // never reaches. Forge a length header that LIES (queue() cannot; bridge_send_raw can). ASan is the
    // judge: substr(pos+5, len) is guarded by len<=kMaxFrameLen and inbox_.size()-pos>=5+len.
    {
        // (a) len claims 16 MiB (UNDER the 64 MiB cap) but sends 3 bytes -> the framer WAITS: it must
        //     deliver no frame and NOT over-read (never touch bytes it does not have).
        const std::pair<socket_t, socket_t> p = two_sockets();
        BridgeChannel framer(p.second);
        std::string lie;
        put_u32(lie, 0x01000000u); // 16 MiB
        put_u8(lie, static_cast<std::uint8_t>(BridgeOp::Hello));
        lie.append("abc");
        bridge_send_raw(p.first, lie);
        std::vector<BridgeIncoming> frames;
        for (int i = 0; i < 100 && frames.empty() && !framer.done(); ++i) {
            framer.poll(frames);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(frames.empty());        // no bogus frame delivered from the lie
        CHECK_FALSE(framer.failed()); // under the cap: it waits (graceful), it does not fail or over-read
        bridge_close(p.first);

        // (b) len OVER the 64 MiB cap -> the framer fails the channel CLEANLY (the defensive cap; a
        //     reap follows). No over-read, no hang.
        const std::pair<socket_t, socket_t> q = two_sockets();
        BridgeChannel framer2(q.second);
        std::string over;
        put_u32(over, kMaxFrameLen + 1u);
        put_u8(over, static_cast<std::uint8_t>(BridgeOp::Hello));
        bridge_send_raw(q.first, over);
        std::vector<BridgeIncoming> f2;
        for (int i = 0; i < 100 && !framer2.done(); ++i) {
            framer2.poll(f2);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(f2.empty());
        CHECK(framer2.failed()); // len > kMaxFrameLen -> clean failure (no over-read, no hang)
        bridge_close(q.first);
    }
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
    // The proxy was UNREGISTERED from the bus (not just dropped from conns_) — only the greeter remains.
    CHECK(bus.list_weaves().size() == 1u);
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
    // The proxy was UNREGISTERED from the bus (not just dropped from conns_) — only the greeter remains.
    CHECK(bus.list_weaves().size() == 1u);
}
#endif // _WIN32

TEST_SUITE_END();
