// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include <zen/bridge/channel.hpp>
#include <zen/bridge/remote_console.hpp>
#include <zen/bridge/server.hpp>

#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/value.hpp>

#include <algorithm>
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
#include <sys/socket.h>
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

namespace loom {
/// The R2F-C observation instrument (see the friend declaration in zen/bridge/channel.hpp): reads
/// the channel's OWN retained buffers, so the bounded-storage law is stated as an assertion about
/// transport state rather than inferred from process memory -- RSS is allocator- and OS-sensitive
/// and cannot tell "capacity remains reusable" from "sent bytes remain part of the live buffer".
/// Those are different claims and only the second is F-18. This adds no member and no code path:
/// channel.cpp's object file is byte-identical with and without the friend declaration.
struct BridgeChannelStorageProbe {
    static std::size_t live(const BridgeChannel& c) { return c.outbox_.size(); }
    static std::size_t sent(const BridgeChannel& c) { return c.out_pos_; }
    static std::size_t unsent(const BridgeChannel& c) { return c.outbox_.size() - c.out_pos_; }
    static std::size_t inbox(const BridgeChannel& c) { return c.inbox_.size(); }
};
} // namespace loom

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

// ---- R2F-A: a weave whose door is the amplification carrier -----------------------------------
//
// A zero-field Message costs ZERO wire bytes, so `Bulk`'s list is the shape whose decoded
// population is unrelated to its serialized size. Registering this weave is what puts that door in
// the bus's registry — which is exactly how a hostile participant reaches the host's decoder: it
// declares a schema, the host registers it, and thereafter the host parses that participant's bytes
// against it, IN THE HOST PROCESS, before any grant is consulted.

std::shared_ptr<const loom::Schema> bulk_nothing_schema() {
    static const auto s = loom::SchemaBuilder("R2FA.Nothing", 1).build();
    return s;
}
std::shared_ptr<const loom::Schema> bulk_schema() {
    static const auto s = loom::SchemaBuilder("R2FA.Bulk", 1)
                              .list("items", loom::type_message(bulk_nothing_schema()))
                              .build();
    return s;
}

class BulkSink final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {bulk_schema()};
    }
    void handle(const loom::Message& in, loom::Bus&) override {
        const loom::Cell* items = in.payload.get("items");
        delivered_.fetch_add(1);
        received_.store(items != nullptr ? items->as_list().size() : 0);
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

    std::uint64_t delivered() const noexcept { return delivered_.load(); }
    std::size_t received() const noexcept { return received_.load(); }

private:
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::size_t> received_{0};
    static std::shared_ptr<const loom::Schema> state_schema() {
        static const auto s =
            loom::SchemaBuilder("BulkSinkState", 1).field("n", loom::Kind::Int).build();
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
    BulkSink* bulk = nullptr; ///< non-null only under Host(WithBulk) — see R2F-A below
    loom::WeaveId gid{};
    loom::WeaveId bulk_id{};
    socket_t listener = kInvalidSocket;
    std::uint16_t port = 0;
    std::string err;
    std::unique_ptr<loom::BridgeServer> server;
    std::atomic<bool> stop{false};
    std::thread th;

    /// Opt-in second participant, so the default host every other case uses is unchanged.
    enum WithBulk { kWithBulk };

    Host() : Host(false) {}
    explicit Host(WithBulk) : Host(true) {}

    explicit Host(bool with_bulk) {
        auto g = std::make_unique<RecordingGreeter>();
        greeter = g.get();
        gid = bus.register_weave(std::move(g), loom::Grant{}.allow_any());
        if (with_bulk) {
            auto b = std::make_unique<BulkSink>();
            bulk = b.get();
            bulk_id = bus.register_weave(std::move(b), loom::Grant{}.allow_any());
        }
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
    // AF_UNIX gains its live consumer: the frozen design record (docs/history/pre-r2c/DESIGN.md,
    // decision #4) says the fast local loop IS unix, so exercise it. (The crossing uses TCP;
    // AF_UNIX is POSIX-only, hence the gate.)
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

// ---- R2F-C: consumed transport bytes are history, not live channel storage (LIFE-07) -----------
//
// COLD-1 F-18: flush() clear()ed the outbox ONLY on an exact drain, so a peer that keeps up but
// never lets the socket run dry left a standing residue at every flush, the reset never fired, and
// the buffer grew by the session's whole byte volume. kMaxBacklog measures the UNSENT residue, so
// it never noticed. Measured pre-repair on exactly this shape: +261 B per round, strictly linear,
// 524,160 bytes already sent and still retained after 2,000 rounds, with failed() never set.
//
// These proofs are POSIX-gated because they need a deliberately small in-flight socket window
// (SO_SNDBUF/SO_RCVBUF over a socketpair) to state the law at kilobyte scale instead of at the
// ~2.6 MB TCP-loopback window. The repaired code is the platform-agnostic framing half that both
// raw-I/O backends share. The isolation Channel's identical repair is proven independently in
// test_isolation.cpp -- neither suite is evidence for the other.
//
// Note what the outbox is NOT: unlike the inbox it is an undifferentiated byte stream, with no
// header/payload structure to respect. A compaction boundary may fall anywhere -- inside a length
// header, inside a payload, on a frame edge -- and the only correctness question is whether the
// unsent bytes survive the move exactly. The tests classify where each boundary actually landed
// and assert on the resulting wire stream.

namespace {

struct TinyPair { // a socketpair with a deliberately small in-flight window
    int producer = -1;
    int consumer = -1;
};

TinyPair tiny_pair(bool shrink = true) {
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    if (shrink) {
        const int small = 2048; // the point is the RULE, not the volume
        (void)::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        (void)::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    }
    return TinyPair{sv[0], sv[1]};
}

/// Deterministic, self-identifying frame bodies of a UNIFORM size, so a byte-for-byte comparison
/// names the frame and a compaction boundary can be classified by a single modulo.
constexpr std::size_t kBodyLen = 200;
constexpr std::size_t kFrameLen = 5 + kBodyLen;

std::string body(int i) {
    std::string p = "frame:" + std::to_string(i) + ":";
    p.resize(kBodyLen, static_cast<char>('a' + (i % 26)));
    return p;
}

/// Where a byte offset falls inside the uniform frame layout above.
enum class Landing { FrameEdge, InHeader, InPayload };
Landing classify(std::size_t offset) {
    const std::size_t off = offset % kFrameLen;
    if (off == 0) {
        return Landing::FrameEdge;
    }
    return off < 5 ? Landing::InHeader : Landing::InPayload;
}

} // namespace

TEST_CASE("R2F-C (bridge): a channel that is never idle still reclaims what it has already sent") {
    using P = BridgeChannelStorageProbe;
    const TinyPair fds = tiny_pair();
    BridgeChannel ch(static_cast<socket_t>(fds.producer));
    BridgeChannel peer(static_cast<socket_t>(fds.consumer));

    int next = 0;
    std::size_t queued_bytes = 0; // counted here, so it survives every clear() and compaction
    const auto queue_one = [&]() {
        ch.queue(BridgeOp::Tap, body(next++));
        queued_bytes += kFrameLen;
    };

    // Phase 1 -- measure the socket's in-flight window. Nothing has been drained yet, so everything
    // queued minus what is still unsent is exactly what the socket swallowed.
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        queue_one();
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    const std::size_t window = queued_bytes - P::unsent(ch);

    // Phase 2 -- build a standing backlog LARGER than that window, so no later flush can empty the
    // buffer. A residue SMALLER than the window is drained away the moment the peer makes room and
    // the exact-drain clear() fires: F-18 needs a PERSISTENT suffix, not merely a slow peer.
    for (int i = 0; i < 20000 && P::unsent(ch) < window + 8192; ++i) {
        queue_one();
        ch.flush();
    }
    const std::size_t target = P::unsent(ch);
    REQUIRE(target > window);

    std::vector<BridgeIncoming> got;
    std::size_t compactions = 0;
    std::size_t bytes_moved = 0;
    std::size_t max_live = 0;
    int in_header = 0;
    int in_payload = 0;
    int on_edge = 0;
    int law_violations = 0;
    int idle_rounds = 0;
    constexpr int kRounds = 400;
    for (int r = 0; r < kRounds; ++r) {
        peer.poll(got);                  // the peer keeps up: it drains everything available ...
        while (P::unsent(ch) < target) { // ... and the producer tops the backlog straight back up
            queue_one();
        }
        const std::size_t live_before = P::live(ch);
        ch.flush();
        if (P::live(ch) < live_before) { // only a clear()/compaction can shrink the buffer
            ++compactions;
            bytes_moved += P::live(ch);  // after erase(0, out_pos_) the size IS the bytes moved
            switch (classify(queued_bytes - P::unsent(ch))) { // where the boundary landed
            case Landing::InHeader:
                ++in_header;
                break;
            case Landing::InPayload:
                ++in_payload;
                break;
            case Landing::FrameEdge:
                ++on_edge;
                break;
            }
        }
        if (P::unsent(ch) == 0) {
            ++idle_rounds; // the exact-drain clear() would have been reachable after all
        }
        if (P::live(ch) > 2 * P::unsent(ch)) {
            ++law_violations; // THE LAW: live storage tracks the BACKLOG, never the history
        }
        max_live = std::max(max_live, P::live(ch));
    }

    MESSAGE("window " << window << " B; queued " << queued_bytes << " B over " << next
                      << " frames; live " << P::live(ch) << " B (high-water " << max_live
                      << " B, backlog target " << target << " B); " << compactions
                      << " compactions moved " << bytes_moved << " B; boundary landed in-header "
                      << in_header << ", in-payload " << in_payload << ", on-edge " << on_edge);

    CHECK(idle_rounds == 0);      // the buffer never once became empty -- the F-18 shape held
    CHECK(law_violations == 0);   // ... and live storage stayed bounded by twice the backlog anyway
    CHECK(compactions > 0);       // reclamation actually ran (guards a vacuously bounded pass)
    CHECK(bytes_moved <= queued_bytes); // amortized: a move never costs more than the bytes it drops
    CHECK(queued_bytes > 20 * max_live); // history dwarfs the high-water of live storage
    CHECK(in_header + in_payload > 0);   // a compaction really did split a frame and carry the rest
    CHECK_FALSE(ch.failed());
    CHECK_FALSE(peer.failed());

    // Every frame still arrives, exactly once, in order, byte for byte.
    for (int i = 0; i < 20000 && static_cast<int>(got.size()) < next; ++i) {
        ch.flush();
        peer.poll(got);
    }
    REQUIRE(static_cast<int>(got.size()) == next);
    std::size_t first_bad = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].op != BridgeOp::Tap || got[i].payload != body(static_cast<int>(i))) {
            first_bad = i;
            break;
        }
    }
    CHECK(first_bad == static_cast<std::size_t>(-1)); // index of the first corrupted/reordered frame
}

TEST_CASE("R2F-C (bridge): frames queued behind a half-sent one keep their order and their bytes") {
    using P = BridgeChannelStorageProbe;
    const TinyPair fds = tiny_pair();
    BridgeChannel ch(static_cast<socket_t>(fds.producer));
    BridgeChannel peer(static_cast<socket_t>(fds.consumer));

    // Fill the socket, so the frame at the boundary is genuinely half-sent ...
    int next = 0;
    std::size_t queued_bytes = 0;
    const auto queue_one = [&]() {
        ch.queue(BridgeOp::Tap, body(next++));
        queued_bytes += kFrameLen;
    };
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        queue_one();
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    const std::size_t window = queued_bytes - P::unsent(ch);
    // ... and give it a backlog several windows deep, so draining cannot empty it in one flush
    // (a buffer that empties takes the exact-drain clear() path, which is not what is under test).
    for (int i = 0; i < 20000 && P::unsent(ch) < 4 * window + 4096; ++i) {
        queue_one();
        ch.flush();
    }
    // ... then queue three COMPLETE frames behind it, none of which has been touched by the socket.
    const int first_untouched = next;
    for (int i = 0; i < 3; ++i) {
        queue_one();
    }
    REQUIRE(P::unsent(ch) > 3 * kFrameLen); // the half-sent frame plus the three whole ones

    std::vector<BridgeIncoming> got;
    std::size_t compactions_with_unsent_data = 0;
    int in_header = 0;
    int in_payload = 0;
    for (int i = 0; i < 20000 && static_cast<int>(got.size()) < next; ++i) {
        peer.poll(got);
        const std::size_t live_before = P::live(ch);
        ch.flush();
        if (P::live(ch) < live_before && P::unsent(ch) > 0) {
            ++compactions_with_unsent_data;
            switch (classify(queued_bytes - P::unsent(ch))) {
            case Landing::InHeader:
                ++in_header;
                break;
            case Landing::InPayload:
                ++in_payload;
                break;
            case Landing::FrameEdge:
                break;
            }
        }
    }
    MESSAGE("compactions carrying live data: " << compactions_with_unsent_data << " (in-header "
                                               << in_header << ", in-payload " << in_payload << ")");
    CHECK(compactions_with_unsent_data > 0); // the reclamation under test actually ran

    REQUIRE(static_cast<int>(got.size()) == next);
    std::size_t first_bad = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].op != BridgeOp::Tap || got[i].payload != body(static_cast<int>(i))) {
            first_bad = i;
            break;
        }
    }
    CHECK(first_bad == static_cast<std::size_t>(-1));
    // The three frames that were still whole in the buffer when it was compacted: exact and in order.
    CHECK(got[static_cast<std::size_t>(first_untouched)].payload == body(first_untouched));
    CHECK(got[static_cast<std::size_t>(first_untouched) + 1].payload == body(first_untouched + 1));
    CHECK(got[static_cast<std::size_t>(first_untouched) + 2].payload == body(first_untouched + 2));
}

TEST_CASE("R2F-C (bridge): reclamation moves the backlog, it does not shrink it") {
    using P = BridgeChannelStorageProbe;
    const TinyPair fds = tiny_pair();
    BridgeChannel ch(static_cast<socket_t>(fds.producer));
    BridgeChannel peer(static_cast<socket_t>(fds.consumer));

    // kMaxBacklog is measured as `outbox_.size() - out_pos_`. A compaction subtracts the SAME
    // amount from both terms, so the number the cap reads is invariant -- which is what keeps an
    // undrained peer contained exactly as before. Pin it on a real compaction.
    int next = 0;
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        ch.queue(BridgeOp::Tap, body(next++));
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    for (int i = 0; i < 200; ++i) { // a backlog wide enough that draining cannot empty it at once
        ch.queue(BridgeOp::Tap, body(next++));
    }

    std::vector<BridgeIncoming> got;
    bool saw_compaction = false;
    for (int i = 0; i < 20000 && !saw_compaction; ++i) {
        peer.poll(got);
        const std::size_t live_before = P::live(ch);
        const std::size_t unsent_before = P::unsent(ch);
        if (unsent_before == 0) {
            break;
        }
        ch.flush();
        if (P::live(ch) < live_before && P::unsent(ch) > 0) {
            saw_compaction = true;
            CHECK(P::sent(ch) == 0);                  // the offset moved to the front ...
            CHECK(P::live(ch) == P::unsent(ch));      // ... and the buffer is now exactly the backlog
            CHECK(P::unsent(ch) <= unsent_before);    // the backlog never grew across the move
        }
    }
    REQUIRE(saw_compaction);

    // And the cap still fires: pile an undrained backlog past kMaxBacklog on a channel whose
    // reclamation has already been exercised.
    const std::string mib(1024u * 1024u, 'z');
    for (int i = 0; i < 80 && !ch.failed(); ++i) {
        ch.queue(BridgeOp::Send, mib);
    }
    CHECK(ch.failed()); // a peer that will not drain is contained, exactly as before the repair

    // A failed channel stays failed and stays inert.
    const std::size_t live_when_failed = P::live(ch);
    ch.flush();
    CHECK(ch.failed());
    CHECK(ch.done());
    CHECK(P::live(ch) == live_when_failed);
    ch.queue(BridgeOp::Send, "ignored");
    CHECK(P::live(ch) == live_when_failed); // queue() on a failed channel is still a no-op
}

TEST_CASE("R2F-C (bridge): a failed channel neither sends nor reclaims") {
    using P = BridgeChannelStorageProbe;
    const TinyPair fds = tiny_pair();
    BridgeChannel ch(static_cast<socket_t>(fds.producer));
    BridgeChannel peer(static_cast<socket_t>(fds.consumer));

    // Stage the state where a HEALTHY flush would visibly act: a small unsent remainder and a
    // socket with room for all of it, so an honest flush would send everything and empty the
    // buffer. Without this the failed channel sits behind a 64 MiB backlog and a full socket,
    // where flush() has nothing it could do anyway and the guard cannot be observed at all.
    int next = 0;
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        ch.queue(BridgeOp::Tap, body(next++));
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    std::vector<BridgeIncoming> got;
    peer.poll(got); // the socket is now empty: room for the whole remainder
    const std::size_t delivered_before = got.size();

    ch.fail(); // the existing severance affordance (a protocol violation uses it)
    const std::size_t live_when_failed = P::live(ch);
    const std::size_t sent_when_failed = P::sent(ch);
    REQUIRE(live_when_failed > 0);

    ch.flush();
    peer.poll(got);
    CHECK(ch.failed());                     // still failed ...
    CHECK(ch.done());                       // ... and still done
    CHECK(P::live(ch) == live_when_failed); // nothing cleared, nothing compacted ...
    CHECK(P::sent(ch) == sent_when_failed); // ... and nothing sent
    CHECK(got.size() == delivered_before);  // ... so the peer received nothing more
}

TEST_CASE("R2F-C (bridge): an over-length frame is still refused, and EOF still arrives whole") {
    using P = BridgeChannelStorageProbe;
    SUBCASE("the per-frame cap is a property of the payload, untouched by any buffer state") {
        const TinyPair fds = tiny_pair();
        BridgeChannel ch(static_cast<socket_t>(fds.producer));
        BridgeChannel peer(static_cast<socket_t>(fds.consumer));
        // Drive a real compaction first, so the refusal below is judged on a reclaimed buffer.
        int next = 0;
        for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
            ch.queue(BridgeOp::Tap, body(next++));
            ch.flush();
        }
        for (int i = 0; i < 200; ++i) {
            ch.queue(BridgeOp::Tap, body(next++));
        }
        std::vector<BridgeIncoming> got;
        bool compacted = false;
        for (int i = 0; i < 20000 && !compacted; ++i) {
            peer.poll(got);
            const std::size_t live_before = P::live(ch);
            ch.flush();
            compacted = P::live(ch) < live_before && P::unsent(ch) > 0;
        }
        REQUIRE(compacted);
        CHECK_FALSE(ch.failed());
        const std::string over(static_cast<std::size_t>(kMaxFrameLen) + 1u, 'x');
        ch.queue(BridgeOp::Send, over);
        CHECK(ch.failed()); // over the per-frame cap -> the channel is failed, not the frame sent
    }
    SUBCASE("a complete frame buffered before the peer vanishes is still delivered, then EOF") {
        const TinyPair fds = tiny_pair();
        auto producer = std::make_unique<BridgeChannel>(static_cast<socket_t>(fds.producer));
        BridgeChannel ch(static_cast<socket_t>(fds.consumer));
        producer->queue(BridgeOp::Welcome, "last words");
        producer->flush();
        producer.reset(); // the peer goes away with a whole frame already in flight

        std::vector<BridgeIncoming> got;
        bool saw_eof = false;
        for (int i = 0; i < 2000 && !saw_eof; ++i) {
            ch.poll(got);
            saw_eof = ch.eof();
        }
        CHECK(saw_eof);
        CHECK(ch.done());
        REQUIRE(got.size() == 1);
        CHECK(got[0].op == BridgeOp::Welcome);
        CHECK(got[0].payload == "last words");
    }
}

TEST_CASE("R2F-C (bridge): the RECEIVE buffer was never part of F-18") {
    // The finding named the outbox. Its sibling already reclaims decoded bytes unconditionally
    // (`inbox_.erase(0, pos)`), so a permanently incomplete suffix does NOT pin consumed history in
    // place. Measured, not assumed -- this is the evidence for "inspected, already correct". The
    // partial suffix must be a GENUINE prefix of the next frame; junk would merely desync the
    // framer, which is a different (and already covered) question.
    using P = BridgeChannelStorageProbe;
    const TinyPair fds = tiny_pair(/*shrink=*/false); // raw pushes must never block the test
    BridgeChannel ch(static_cast<socket_t>(fds.consumer));

    constexpr int kFrames = 400;
    std::string stream;
    for (int i = 0; i <= kFrames; ++i) {
        std::string f;
        put_u32(f, static_cast<std::uint32_t>(kBodyLen));
        put_u8(f, static_cast<std::uint8_t>(BridgeOp::Tap));
        f += body(i);
        stream += f;
    }

    std::vector<BridgeIncoming> got;
    std::size_t max_inbox = 0;
    std::size_t pos = 0;
    for (int i = 0; i < kFrames; ++i) {
        // Round 1 pushes one frame plus three bytes; every later round pushes exactly one frame's
        // worth. So EVERY poll() completes one frame and is left holding a 3-byte partial header.
        const std::size_t push = (i == 0) ? kFrameLen + 3 : kFrameLen;
        bridge_send_raw(static_cast<socket_t>(fds.producer),
                        std::string_view(stream).substr(pos, push));
        pos += push;
        ch.poll(got);
        max_inbox = std::max(max_inbox, P::inbox(ch));
    }
    MESSAGE("pushed " << pos << " B through the framer; high-water live inbox " << max_inbox << " B");
    CHECK(static_cast<int>(got.size()) == kFrames);
    CHECK(P::inbox(ch) == 3);        // exactly the incomplete suffix, and nothing behind it
    CHECK(max_inbox < 2 * kFrameLen); // bounded by framing state, never by the traffic volume
    CHECK(pos > 20 * max_inbox);
    CHECK_FALSE(ch.failed());
    ::close(fds.producer);
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

TEST_CASE("R2F-A (end-to-end): a compact frame cannot command an unbounded host decode") {
    // The whole chain, over a REAL loopback socket, with the bytes chosen by the peer:
    //   peer's schema is registered host-side  ->  peer sends a tiny frame  ->  the HOST
    //   process parses and admits it, before any grant is consulted.
    // That is the shape COLD-1 measured (37 wire bytes -> 1,048,576 admitted cells -> +102 MB
    // of HOST RSS). It is refused here by the decoder, at the seam, for the whole host.
    Host h{Host::kWithBulk};
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

    // The forged payload: a Bulk v1 envelope whose one list claims 1,048,576 zero-body elements.
    // The honest client cannot compose this — loom::serialize writes a count matching an array it
    // actually holds — so it is built byte by byte, exactly as the sender-forge case does.
    auto bulk_bytes = [](std::uint64_t count) {
        const std::shared_ptr<const loom::Schema> s = bulk_schema();
        std::string b;
        b.push_back('\x5A');
        b.push_back('\x4E');
        b.push_back('\x01');
        const auto nlen = static_cast<std::uint16_t>(s->name().size());
        b.push_back(static_cast<char>(nlen & 0xFF));
        b.push_back(static_cast<char>((nlen >> 8) & 0xFF));
        b += s->name();
        const std::uint32_t ver = s->version();
        for (int i = 0; i < 4; ++i) {
            b.push_back(static_cast<char>((ver >> (8 * i)) & 0xFF));
        }
        const std::uint64_t cid = s->content_id();
        for (int i = 0; i < 8; ++i) {
            b.push_back(static_cast<char>((cid >> (8 * i)) & 0xFF));
        }
        b.push_back('\x01'); // presence: field 0 present
        std::uint64_t v = count;
        while (v >= 0x80) {
            b.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        b.push_back(static_cast<char>(v));
        return b;
    };

    const std::string hostile = bulk_bytes(1u << 20);
    CHECK(hostile.size() < 64); // the entire attack, in fewer bytes than this comment

    raw.queue(BridgeOp::Send, make_send_frame(0, h.bulk_id.value, 0, 41, hostile));
    raw.flush();

    // (1) refused at the gate — the decoder's branch, not the unknown-schema branch: the door IS
    //     registered, so this is the amplification path and nothing else.
    const std::string reason = next_refusal();
    CHECK(reason.find("gate refused") != std::string::npos);
    CHECK(reason.find("materialization budget") != std::string::npos);
    // (2) it did not become trusted state: no delivery, and nothing was ever handed to the weave.
    CHECK(h.bulk->delivered() == 0);
    CHECK(h.bulk->received() == 0);

    // (3) the host is still usable afterwards — an HONEST Bulk of the same shape delivers, which
    //     also proves the repair did not simply outlaw zero-field-message lists.
    loom::Value honest(bulk_schema());
    loom::Cell::Array arr;
    for (int i = 0; i < 3; ++i) {
        arr.push_back(loom::Cell::message(loom::Value(bulk_nothing_schema())));
    }
    honest.set("items", loom::Cell::list(std::move(arr)));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.bulk_id.value, 0, 42, loom::serialize(honest)));
    raw.flush();
    CHECK(wait_until([&] { return h.bulk->delivered() == 1; }, 2000));
    CHECK(h.bulk->received() == 3);

    // (4) and the OTHER participant on the same host is unharmed: the stream is in sync.
    loom::Value greet(greet_schema());
    greet.set("msg", loom::Cell::text("still here"));
    raw.queue(BridgeOp::Send, make_send_frame(0, h.gid.value, 0, 43, loom::serialize(greet)));
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

// ---- R2E-0: composing with a perpetual in-process service --------------------
//
// The Rule Garden's sharpest seam, at the altitude it was found: a repeating
// Zengine Timer paces itself inside Drive and enqueues its next Drive before
// returning, so the queue never empties and BridgeServer::step()'s drain-to-empty
// pump never returns to poll sockets. Its playground workaround was to append a
// fake application message (`GardenYieldPump`) whose handler called
// Switchboard::stop() — observable machinery with no business purpose. This is
// the same composition with the legitimate surface instead.

/// A weave that re-arms itself on every delivery, exactly as a repeating Timer
/// does. Nothing bounds it; that is the point.
class PerpetualDriver final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const loom::Message&, loom::Bus& bus) override {
        ++turns;
        loom::Value v(greet_schema());
        v.set("msg", loom::Cell::text("drive"));
        bus.send(self, loom::Message(std::move(v))); // the next Drive, before returning
    }
    loom::Value snapshot() const override {
        loom::Value v(state_schema());
        v.set("n", loom::Cell::integer(0));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(2));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

    loom::WeaveId self{};
    std::int64_t turns = 0;

private:
    static std::shared_ptr<const loom::Schema> state_schema() {
        static const auto s = loom::SchemaBuilder("Counter", 1).field("n", loom::Kind::Int).build();
        return s;
    }
};

TEST_CASE("R2E-0: a bridge host with a bounded turn stays responsive while a perpetual service "
          "runs — no fake yield message, no second thread, FIFO intact") {
    loom::Switchboard bus;
    auto owned = std::make_unique<PerpetualDriver>();
    PerpetualDriver* driver = owned.get();
    const loom::WeaveId did = bus.register_weave(std::move(owned), loom::Grant{}.allow_any());
    driver->self = did;

    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    BridgeServer server(bus, listener);

    // THE HOST'S CHOICE, stated once and with no number in it. Without it, the
    // first step() below never returns: the driver re-arms inside its own
    // handler forever.
    server.set_bounded_dispatch();
    CHECK(server.bounded_dispatch());

    // Start the perpetual service: ONE envelope waiting at entry.
    loom::Value kick(greet_schema());
    kick.set("msg", loom::Cell::text("go"));
    bus.send(did, loom::Message(std::move(kick)));

    // A step() with the service already running RETURNS. That is the whole fix.
    // The backlog at entry was 1, so exactly one turn happened and the driver's
    // own continuation was left for the next step.
    server.step();
    CHECK(driver->turns == 1);

    // ...and an operator can still connect and be served, turn after turn, while
    // the service keeps running.
    const socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, err);
    BridgeChannel client(cs);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    client.queue(BridgeOp::Hello, hello);
    client.flush();

    bool welcomed = false;
    for (int i = 0; i < 500 && !welcomed; ++i) {
        server.step();
        std::vector<BridgeIncoming> frames;
        client.poll(frames);
        for (const BridgeIncoming& f : frames) {
            if (f.op == BridgeOp::Welcome) {
                welcomed = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The operator was accepted and welcomed — the outer loop kept control.
    CHECK(welcomed);
    CHECK(server.connection_count() == 1u);
    // The service never stopped: every step took the backlog it found and gave
    // control back, so the driver kept advancing while the operator was served.
    CHECK(driver->turns > 1);
}

TEST_CASE("R2E-0: set_bounded_dispatch needs no number — the backlog at entry bounds the turn, "
          "and the perpetual service keeps running") {
    loom::Switchboard bus;
    auto owned = std::make_unique<PerpetualDriver>();
    PerpetualDriver* driver = owned.get();
    const loom::WeaveId did = bus.register_weave(std::move(owned), loom::Grant{}.allow_any());
    driver->self = did;

    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    BridgeServer server(bus, listener);
    server.set_bounded_dispatch();
    CHECK(server.bounded_dispatch());

    // A real backlog, plus the perpetual driver started inside it.
    loom::Value kick(greet_schema());
    kick.set("msg", loom::Cell::text("go"));
    for (int i = 0; i < 12; ++i) {
        bus.send(did, loom::Message(loom::Value(kick)));
    }
    REQUIRE(bus.pending() == 12u);

    // ONE step clears the whole backlog and RETURNS — no number was chosen, and
    // nothing was throttled. The 12 continuations wait for the next turn.
    server.step();
    CHECK(driver->turns == 12);
    CHECK(bus.pending() == 12u);

    // ...and it stays that way, turn after turn: bounded, adaptive, and the
    // service never stopped.
    server.step();
    CHECK(driver->turns == 24);
    CHECK(bus.pending() == 12u);
}

TEST_CASE("R2E-0: unbounded is the pre-existing contract — step() still drains to empty") {
    loom::Switchboard bus;
    auto g = std::make_unique<RecordingGreeter>();
    const loom::WeaveId gid = bus.register_weave(std::move(g), loom::Grant{}.allow_any());
    std::string err;
    const socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    BridgeServer server(bus, listener);
    CHECK_FALSE(server.bounded_dispatch()); // the default nobody had to opt into

    for (int i = 0; i < 20; ++i) {
        loom::Value v(greet_schema());
        v.set("msg", loom::Cell::text("hi"));
        bus.send(gid, loom::Message(std::move(v)));
    }
    server.step();
    CHECK(bus.pending() == 0u); // drained, exactly as before R2E-0
}

TEST_SUITE_END();
