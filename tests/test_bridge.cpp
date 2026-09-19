// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>
#include "switchboard_fixtures.hpp"
#include "link.hpp" // the supplied host's LinkWeave, header-only (src/host is on the include path)
#include <zen/weave/dispatch_refusal.hpp>

#include <zen/bridge/channel.hpp>
#include <zen/bridge/client.hpp>
#include <zen/bridge/link.hpp>
#include <zen/bridge/remote_console.hpp>
#include <zen/weave/ask_book.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <zen/weave.hpp>
#include <zen/bridge/server.hpp>

#include <zen/console/console.hpp>       // kConsoleTapCapacity / kConsoleBufferCapacity
#include <zen/kernel/schema_codec.hpp>   // encode_schema, for a fake host that publishes a shape
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
#include <mutex>
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
/// The LIFE-07 observation instrument (see the friend declaration in zen/bridge/channel.hpp): reads
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

// ---- a weave whose door is the amplification carrier ------------------------------------------
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
    BulkSink* bulk = nullptr; ///< non-null only under Host(WithBulk) — see the amplification cases below
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

// Build a raw Send frame: [u8 kind][u8 flags][u64 wire_sender][u64 target][u64 wire_reply_to]
// [u64 corr][bytes role][payload].
std::string make_send_frame(std::uint64_t wire_sender, std::uint64_t target,
                            std::uint64_t wire_reply_to, std::uint64_t correlation,
                            std::string_view payload) {
    std::string frame;
    put_u8(frame, kEmitSend);
    put_u8(frame, 0); // flags
    put_u64(frame, wire_sender);
    put_u64(frame, target);
    put_u64(frame, wire_reply_to);
    put_u64(frame, correlation);
    put_bytes(frame, ""); // v4: no office address
    frame.append(payload);
    return frame;
}

// Build a raw host->client Delivered frame as a v4 host ships one: the stamped context ahead of
// the bytes. A fake host forging a delivery has to speak the same wire an honest one does.
std::string make_delivered_frame(std::string_view payload, std::uint64_t sender = 7,
                                 std::uint64_t correlation = 0, std::uint8_t flags = 0) {
    std::string frame;
    put_u64(frame, sender);
    put_u64(frame, correlation);
    put_u8(frame, flags);
    put_bytes(frame, "");
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

// ---- consumed transport bytes are history, not live channel storage (LIFE-07) -----------------
//
// THE FAILURE THIS FALSIFIES: flush() clear()ing the outbox ONLY on an exact drain, so a peer that keeps up but
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

TEST_CASE("RTH-1a: a handler that fails reaches a REMOTE operator as HandlerFailed, not Delivered") {
    // THE BRIDGE HALF OF RTH-1's REPAIR, exercised over a real socket. RTH-1 added
    // `EventKind::HandlerFailed` and a wire kind for it (protocol v3); this is the
    // witness that the kind survives the whole path — Switchboard tap, server
    // encode, wire, client decode — rather than arriving as the `Delivered` a
    // remote operator would have believed.
    class Thrower final : public loom::Weave {
    public:
        std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
            return {greet_schema()};
        }
        void handle(const loom::Message&, loom::Bus&) override {
            throw std::runtime_error("the handler did not complete");
        }
        loom::Value snapshot() const override {
            loom::Value v(loom::SchemaBuilder("ThrowerState", 1).build());
            return v;
        }
        loom::Value policy() const override {
            loom::Value v(loom::lifecycle_policy_schema());
            v.set("max_reloads", loom::Cell::integer(0));
            v.set("revive_from_last_good", loom::Cell::boolean(true));
            return v;
        }
        void revive(const loom::Value&) override {}
    };

    loom::Switchboard bus;
    const loom::WeaveId tid = bus.register_weave(std::make_unique<Thrower>(),
                                                 loom::Grant{}.allow_any());
    std::string err;
    socket_t listener = bridge_listen_tcp(0, &err);
    REQUIRE_MESSAGE(listener != kInvalidSocket, err);
    const std::uint16_t port = bridge_socket_port(listener);
    loom::BridgeServer server(bus, listener);
    std::atomic<bool> stop{false};
    std::atomic<int> escaped{0};
    // The host thread CATCHES. A native handler's exception is rethrown to whoever
    // pumped (MSG-10), and here that is this loop — so the loop is the "host" that
    // decides what to do about it, exactly as a real bridge host must.
    std::thread th([&] {
        while (!stop.load()) {
            try {
                server.wait_and_step(20);
            } catch (const std::exception&) {
                escaped.fetch_add(1);
            }
        }
    });

    socket_t cs = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(cs != kInvalidSocket, err);
    RemoteConsole rc(cs);
    REQUIRE(rc.connected());
    REQUIRE(wait_until(
        [&] {
            rc.pump();
            return !rc.weaves().empty();
        },
        2000));
    Arg arg;
    arg.name = "msg";
    arg.value = FieldValue{std::string("boom")};
    const Composed c = rc.compose(tid, "Greet", 1, {arg});
    CHECK(c.status == Composed::Status::Ready);

    bool saw_failed = false;
    bool saw_delivered_to_thrower = false;
    REQUIRE(wait_until(
        [&] {
            rc.pump();
            for (const TapEvent& e : rc.tap()) {
                if (e.kind == "HandlerFailed") {
                    saw_failed = true;
                }
                if (e.kind == "Delivered" && e.schema == "Greet") {
                    saw_delivered_to_thrower = true;
                }
            }
            return saw_failed;
        },
        3000));
    CHECK(saw_failed);
    // ...and the SAME delivery did not also announce itself a success. That is the
    // silence-wearing-a-success's-clothes RTH-1 found, stated on the wire.
    CHECK(!saw_delivered_to_thrower);
    CHECK(escaped.load() > 0); // the exception still reached the host, unswallowed
    CHECK(kBridgeProtocolVersion >= 3);

    stop.store(true);
    if (th.joinable()) {
        th.join();
    }
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
    put_u8(frame, 0);             // flags
    put_u64(frame, kVictim);      // forged wire_sender — the bridge MUST ignore this
    put_u64(frame, h.gid.value);  // target: the greeter
    put_u64(frame, kVictim);      // forged wire_reply_to — the bridge MUST ignore this too
    put_u64(frame, 1);            // correlation
    put_bytes(frame, "");         // v4: no office address
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
    host.queue(BridgeOp::Delivered, make_delivered_frame(loom::serialize(unk)));
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
        host.queue(BridgeOp::Delivered, make_delivered_frame(bytes));
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

// ---- composing with a perpetual in-process service (MSG-09) ------------------
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
    CHECK(bus.pending() == 0u); // drained, exactly as the unbounded pump does
}

// ---- the CLIENT's retained state is bounded too -------------------------------------------------
//
// RemoteConsole holds four things a peer can feed. TREATING THEM AS ONE SHAPE IS THE MISTAKE — the
// classification is what decides the fix, and each row wants a different one:
//
//   tap_               HISTORY        -> bounded window, oldest evicted and counted
//   buffer_            HISTORY        -> bounded window, oldest evicted, labels stay identities
//   pending_delivered_ ACTIVE BACKLOG -> already bounded by REFUSAL (kMaxPendingDelivered), not
//                                        eviction: each entry is a reply still owed a schema, and
//                                        dropping the oldest would discard an obligation. Proven
//                                        above ("the client bounds pending replies..."); untouched.
//   schema_absent_     CACHE          -> the easiest one to miss, and the only one a peer could
//                                        still grow forever: every Delivered naming a novel unknown
//                                        shape adds an entry that is never removed.

namespace loom {
/// See the friend declaration in zen/bridge/remote_console.hpp. The absent-schema memo has no
/// operator-visible surface (unlike the tap and the buffer, whose eviction the operator must SEE),
/// so its bound is asserted about client state rather than inferred from process memory.
struct RemoteConsoleStorageProbe {
    static std::size_t absent(const RemoteConsole& rc) { return rc.schema_absent_.size(); }
    static bool absent_holds(const RemoteConsole& rc, const std::string& name,
                             std::uint32_t version) {
        return rc.known_absent(name, version);
    }
    static std::size_t pending(const RemoteConsole& rc) { return rc.pending_delivered_.size(); }
};
} // namespace loom

namespace {

/// A Tap frame carrying `target` as its identity, so tap entry i is recognizable as tap entry i.
std::string make_tap_frame(std::uint64_t target) {
    std::string body;
    put_u8(body, kTapDelivered);
    put_u64(body, target);
    put_u64(body, 1);
    put_bytes(body, "Mark");
    put_u32(body, 1);
    put_bytes(body, "");
    return body;
}

std::shared_ptr<const loom::Schema> mark_schema() {
    static const auto s = loom::SchemaBuilder("Mark", 1).field("n", loom::Kind::Int).build();
    return s;
}

/// Bring a RemoteConsole up against a hand-driven fake host (the only way to push an exact number
/// of Tap/Delivered frames; a real BridgeServer emits what the bus happens to produce).
void welcome(RemoteConsole& rc, BridgeChannel& host) {
    std::string body;
    put_u64(body, 7);
    put_u32(body, kBridgeProtocolVersion);
    host.queue(BridgeOp::Welcome, body);
    host.flush();
    REQUIRE(wait_until(
        [&] {
            host.flush();
            rc.pump();
            return rc.connected();
        },
        2000));
}

} // namespace

TEST_CASE("C-1 (bridge): the remote tap and reply buffer are bounded windows, with stable labels") {
    const std::pair<socket_t, socket_t> pair = two_sockets();
    RemoteConsole rc(pair.first, /*handshake_timeout_ms=*/0);
    BridgeChannel host(pair.second);
    welcome(rc, host);

    // Teach the client the reply shape first, so every Delivered admits straight into the buffer
    // instead of parking in the pending backlog.
    host.queue(BridgeOp::Schema, loom::serialize(loom::encode_schema(*mark_schema())));

    // A reply per label, each carrying the number its label must agree with.
    constexpr std::size_t kExtraReplies = 3;
    for (std::size_t i = 1; i <= kConsoleBufferCapacity + kExtraReplies; ++i) {
        loom::Value v(mark_schema());
        v.set("n", loom::Cell::integer(static_cast<std::int64_t>(i)));
        host.queue(BridgeOp::Delivered, make_delivered_frame(loom::serialize(v)));
    }
    // ... and more bus events than the tap can hold.
    constexpr std::uint64_t kBase = 5000;
    constexpr std::uint64_t kExtraTaps = 5;
    for (std::uint64_t i = 0; i < kConsoleTapCapacity + kExtraTaps; ++i) {
        host.queue(BridgeOp::Tap, make_tap_frame(kBase + i));
    }
    host.flush();

    REQUIRE(wait_until(
        [&] {
            host.flush();
            rc.pump();
            return rc.evicted().tap == kExtraTaps && rc.evicted().buffer == kExtraReplies;
        },
        10000));

    // The tap: saturated, sliding, and in order.
    CHECK(rc.tap().size() == kConsoleTapCapacity);
    CHECK(rc.tap().front().target.value == kBase + kExtraTaps);
    CHECK(rc.tap().back().target.value == kBase + kConsoleTapCapacity + kExtraTaps - 1);

    // The buffer: saturated, and mN still names reply N — the evicted labels refuse rather than
    // handing back whatever now sits in that slot.
    CHECK(rc.buffer_size() == kConsoleBufferCapacity);
    CHECK_FALSE(rc.buffer_at(1).has_value());
    CHECK_FALSE(rc.buffer_at(kExtraReplies).has_value()); // the last evicted label
    for (std::size_t n = kExtraReplies + 1; n <= kExtraReplies + kConsoleBufferCapacity; ++n) {
        const std::optional<BufferEntry> e = rc.buffer_at(n);
        REQUIRE_MESSAGE(e.has_value(), "retained label m" << n << " must resolve");
        CHECK(e->label == "m" + std::to_string(n));
        CHECK(e->value.get("n")->as_int() == static_cast<std::int64_t>(n));
    }
    CHECK_FALSE(rc.buffer_at(kExtraReplies + kConsoleBufferCapacity + 1).has_value());

    // The active backlog is a different question and it answered correctly: nothing is owed.
    CHECK(RemoteConsoleStorageProbe::pending(rc) == 0);
}

TEST_CASE("C-1 (bridge): the absent-schema memo is bounded — a host cannot grow it forever") {
    // Every Delivered naming an unknown shape makes the client ask Describe and remember the "no
    // such schema" answer. A host that keeps naming NOVEL shapes therefore used to add one entry
    // per distinct name, forever. It is a memo, so the bound is eviction: the cost is one repeated
    // Describe, and it is the only bound here that also fixes a staleness (a shape registered later
    // is no longer remembered as absent for the life of the process).
    const std::pair<socket_t, socket_t> pair = two_sockets();
    RemoteConsole rc(pair.first, /*handshake_timeout_ms=*/0);
    BridgeChannel host(pair.second);
    welcome(rc, host);

    constexpr std::size_t kOverflow = 6;
    const auto absent_name = [](std::size_t k) { return "Absent" + std::to_string(k); };

    // One at a time, answering each Describe before the next: pushing them all at once would trip
    // kMaxPendingDelivered instead, which is the OTHER bound and not what this case is about.
    for (std::size_t k = 1; k <= RemoteConsole::kMaxAbsentSchemas + kOverflow; ++k) {
        const auto schema =
            loom::SchemaBuilder(absent_name(k), 1).field("x", loom::Kind::Int).build();
        loom::Value v(schema);
        v.set("x", loom::Cell::integer(static_cast<std::int64_t>(k)));
        host.queue(BridgeOp::Delivered, make_delivered_frame(loom::serialize(v)));
        host.flush();

        // Wait for THIS name specifically: once the memo saturates its size stops moving, so a
        // size-based wait would fall through without the client ever learning the new answer.
        REQUIRE_MESSAGE(wait_until(
                            [&] {
                                host.flush();
                                rc.pump();
                                std::vector<BridgeIncoming> frames;
                                host.poll(frames);
                                for (const BridgeIncoming& f : frames) {
                                    if (f.op != BridgeOp::Describe) {
                                        continue;
                                    }
                                    Cursor c(f.payload);
                                    std::string_view name;
                                    std::uint32_t ver = 0;
                                    if (c.bytes(name) && c.u32(ver)) {
                                        std::string body;
                                        put_bytes(body, name);
                                        put_u32(body, ver);
                                        host.queue(BridgeOp::SchemaNone, body);
                                        host.flush();
                                    }
                                }
                                rc.pump();
                                return RemoteConsoleStorageProbe::absent_holds(rc, absent_name(k), 1);
                            },
                            5000),
                        "the client never learned that " << absent_name(k) << " is absent");
    }

    // Saturated, not growing: the memo stopped at its capacity while six more distinct names went
    // through it.
    CHECK(RemoteConsoleStorageProbe::absent(rc) == RemoteConsole::kMaxAbsentSchemas);
    // FIFO, and in the direction that matters: the entry just learned survives (so a fetch waiting
    // on THIS answer still terminates) and the stalest one is the one that left.
    CHECK(RemoteConsoleStorageProbe::absent_holds(
        rc, absent_name(RemoteConsole::kMaxAbsentSchemas + kOverflow), 1));
    CHECK_FALSE(RemoteConsoleStorageProbe::absent_holds(rc, absent_name(1), 1));
    CHECK(RemoteConsoleStorageProbe::absent_holds(rc, absent_name(kOverflow + 1), 1));
    // And nothing was owed at the end: the backlog drained as the answers arrived.
    CHECK(RemoteConsoleStorageProbe::pending(rc) == 0);
}

TEST_SUITE_END();

TEST_SUITE("bridge") {
TEST_CASE("wire-originated refusal-shaped speech cannot acquire Loom attestation") {
    Switchboard bus;
    auto recipient=sbfx::register_probe(bus,{schema_of<DispatchRefused>()});
    int ordinary=0,trusted=0;
    WeaveId actual_sender{};
    recipient.weave->on_handle=[&](const Message& in,Bus&,sbfx::ProbeWeave&) {
        if(in.provenance.dispatch_refused()) { ++trusted; }
        else { ++ordinary; actual_sender=in.sender; }
        CHECK_FALSE(in.provenance.answers_ask());
    };
    std::string error;
    const auto listener=bridge_listen_tcp(0,&error);
    REQUIRE_MESSAGE(listener!=kInvalidSocket,error);
    BridgeServer server(bus,listener);
    const auto connection=bridge_connect_tcp("127.0.0.1",bridge_socket_port(listener),&error);
    REQUIRE_MESSAGE(connection!=kInvalidSocket,error);
    BridgeChannel raw(connection);
    std::string hello;put_u32(hello,kBridgeProtocolVersion);
    raw.queue(BridgeOp::Hello,hello);raw.flush();
    std::uint64_t operator_id=0;
    REQUIRE(wait_until([&] {
        server.step();std::vector<BridgeIncoming> frames;raw.poll(frames);
        for(const auto& frame:frames) if(frame.op==BridgeOp::Welcome) {
            Cursor cur(frame.payload);std::uint32_t version=0;
            if(!cur.u64(operator_id)||!cur.u32(version)) operator_id=0;
        }
        return operator_id!=0;
    },2000));
    DispatchRefused fake;fake.attempt="1";fake.target="999";
    fake.shape="Ping";fake.version=1;fake.reason="CapabilityDenied";
    raw.queue(BridgeOp::Send,make_send_frame(999999,recipient.id.value,999999,0,serialize(to_value(fake))));
    raw.flush();
    REQUIRE(wait_until([&] {server.step();return ordinary==1;},2000));
    CHECK(trusted==0);CHECK(actual_sender.value==operator_id);
    // Same payload family, genuine dispatch owner: the recipient can distinguish it.
    const auto attempt=bus.send_as(recipient.id,WeaveId{999},Message(sbfx::ping(1)));
    REQUIRE(attempt.valid());bus.pump_pending();CHECK(trusted==0);bus.pump_pending();
    CHECK(trusted==1);CHECK(ordinary==1);CHECK(bus.pending()==0);
}

// =================================================================================================
// THE TWO-HOST CROSSING (v4): admission, identity, stamped context, the link.
// =================================================================================================
//
// Everything above this line proves the crossing at the mechanism altitude for ONE principal --
// the operator. The cases below are the guest's: a connection that is admitted deliberately,
// under a grant the host's policy chose, told what it is and what it is not, and given nothing
// it was not granted. Two of them are the guards the prompt names as the ones that must fail
// when removed: an unadmitted connection acting, and a claimed name becoming an identity.


namespace {

/// A typed shape and a participant that ANSWERS it -- `mail.answer`, so Loom attests the reply
/// -- the way a real Workshop door answers a guest. The greeter above replies with an ordinary
/// send, which is exactly the distinction the Delivered flags exist to carry.
struct Echo {
    std::string msg;
    ZEN_SHAPE(Echo, 1, ZEN_FIELD(msg));
};
struct EchoState {
    std::int64_t heard = 0;
    ZEN_SHAPE(EchoState, 1, ZEN_FIELD(heard));
};
class EchoAnswerer final
    : public loom::WeaveBase<EchoAnswerer, EchoState, loom::Accept<Echo>, loom::Emit<loom::Result>> {
public:
    void on(const Echo& e, loom::Mail& mail) {
        ++state_.heard;
        last_sender_ = mail.sender();
        (void)mail.answer(loom::Result{"echo:" + e.msg});
    }
    std::int64_t heard() const { return state_.heard; }
    loom::WeaveId last_sender_{};
};

/// A host with a POLICY of the test's choosing, stepped on its own thread like `Host`.
struct GuestHost {
    loom::Switchboard bus;
    EchoAnswerer* echo = nullptr;
    loom::WeaveId echo_id{};
    socket_t listener = kInvalidSocket;
    std::uint16_t port = 0;
    std::unique_ptr<loom::BridgeServer> server;
    std::atomic<bool> stop{false};
    std::thread th;
    std::vector<loom::Connection> told; ///< every state change the server reported
    std::mutex told_mutex;

    explicit GuestHost(loom::BridgeAdmission policy, bool threaded = true) {
        auto e = std::make_unique<EchoAnswerer>();
        echo = e.get();
        echo_id = bus.register_weave(std::move(e), loom::Grant{}.allow_any(), std::string("echo"));
        echo->zen_set_self(echo_id);
        std::string err;
        listener = bridge_listen_tcp(0, &err);
        REQUIRE_MESSAGE(listener != kInvalidSocket, err);
        port = bridge_socket_port(listener);
        server = std::make_unique<loom::BridgeServer>(bus, listener, std::move(policy));
        server->on_connection([this](const loom::Connection& c) {
            const std::lock_guard<std::mutex> lock(told_mutex);
            told.push_back(c);
        });
        if (threaded) {
            th = std::thread([this] {
                while (!stop.load()) {
                    server->wait_and_step(20);
                }
            });
        }
    }
    ~GuestHost() {
        stop.store(true);
        if (th.joinable()) {
            th.join();
        }
    }
    std::vector<loom::Connection> reported() {
        const std::lock_guard<std::mutex> lock(told_mutex);
        return told;
    }
};

/// The narrow grant a Workshop-like policy hands a guest: one shape, to one office.
loom::ConnectionAdmitted echo_only(std::string name) {
    loom::ConnectionAdmitted a;
    a.grant.allow_to_role(Echo::zen_name, Echo::zen_version, "echo");
    a.established_name = std::move(name);
    a.observe = false;
    return a;
}

loom::BridgeClient& connect_client(std::unique_ptr<loom::BridgeClient>& out, std::uint16_t port,
                                   const char* name, const char* credential) {
    std::string err;
    const socket_t s = bridge_connect_tcp("127.0.0.1", port, &err);
    REQUIRE_MESSAGE(s != kInvalidSocket, err);
    out = std::make_unique<loom::BridgeClient>(s);
    REQUIRE(out->hello(name, credential));
    return *out;
}

bool drain_events(loom::BridgeClient& c, std::vector<loom::BridgeEvent>& into,
                  const std::function<bool()>& done, int timeout_ms) {
    return wait_until(
        [&] {
            std::vector<loom::BridgeEvent> got;
            c.poll(got);
            for (loom::BridgeEvent& e : got) {
                into.push_back(std::move(e));
            }
            return done();
        },
        timeout_ms);
}

} // namespace

TEST_CASE("admission: a refused connection is told why, gets no proxy, and its sends act on nothing") {
    GuestHost h([](const loom::ConnectionRequest& r) {
        return r.credential == "open-sesame" ? loom::ConnectionVerdict::admit(echo_only("agent"))
                                             : loom::ConnectionVerdict::refuse("wrong credential");
    });
    std::unique_ptr<loom::BridgeClient> c;
    loom::BridgeClient& client = connect_client(c, h.port, "agent", "nope");
    CHECK_FALSE(client.await_admission(2000));
    CHECK(client.denied());
    CHECK(client.denial() == "wrong credential");
    // The connection is severed and nothing of it remains on the bus.
    CHECK(wait_until([&] { return h.server->connection_count() == 0; }, 2000));
    CHECK(h.server->refused_count() == 1);
    CHECK(h.bus.list_weaves().size() == 1); // the echo answerer alone; no proxy was ever registered
    // ...and the refusal was REPORTED, as a state the inventory can show and drop.
    bool saw_refused = false;
    bool saw_closed = false;
    for (const loom::Connection& t : h.reported()) {
        saw_refused = saw_refused || t.state == loom::ConnectionState::Refused;
        saw_closed = saw_closed || t.state == loom::ConnectionState::Closed;
    }
    CHECK(saw_refused);
    CHECK(saw_closed);
    CHECK(h.echo->heard() == 0);
}

TEST_CASE("admission: the established name is the policy's word, never the peer's claim") {
    GuestHost h([](const loom::ConnectionRequest& r) {
        // The policy knows the credential, not the name the peer chose for itself.
        if (r.credential != "open-sesame") {
            return loom::ConnectionVerdict::refuse("wrong credential");
        }
        return loom::ConnectionVerdict::admit(echo_only("the-agent-on-record"));
    });
    std::unique_ptr<loom::BridgeClient> c;
    loom::BridgeClient& client = connect_client(c, h.port, "root", "open-sesame");
    REQUIRE(client.await_admission(2000));
    CHECK(client.established_name() == "the-agent-on-record");
    CHECK(client.session() != 0);
    bool inventory_kept_both = false;
    for (const loom::Connection& t : h.reported()) {
        if (t.state == loom::ConnectionState::Admitted) {
            inventory_kept_both = t.claimed_name == "root" &&
                                  t.established_name == "the-agent-on-record" &&
                                  t.session.value == client.session();
        }
    }
    CHECK(inventory_kept_both);
}

TEST_CASE("admission: a guest's send is stamped from the session and answered WITH Loom's attestation") {
    GuestHost h([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_only("agent"));
    });
    std::unique_ptr<loom::BridgeClient> c;
    loom::BridgeClient& client = connect_client(c, h.port, "agent", "");
    REQUIRE(client.await_admission(2000));
    // A role-addressed send, the way a guest that never learned a WeaveId speaks.
    client.send_to_role("echo", /*correlation=*/42, loom::serialize(loom::to_value(Echo{"hi"})));
    std::vector<loom::BridgeEvent> events;
    REQUIRE(drain_events(
        client, events,
        [&] {
            for (const loom::BridgeEvent& e : events) {
                if (e.kind == loom::BridgeEvent::Kind::Delivered) {
                    return true;
                }
            }
            return false;
        },
        2000));
    const loom::BridgeEvent* delivered = nullptr;
    for (const loom::BridgeEvent& e : events) {
        if (e.kind == loom::BridgeEvent::Kind::Delivered) {
            delivered = &e;
        }
    }
    REQUIRE(delivered != nullptr);
    CHECK(delivered->correlation == 42);
    CHECK(delivered->sender == h.echo_id.value); // the bus's stamp of who answered
    CHECK(delivered->answers_ask);               // Loom attests: THE answer to this session's ask
    CHECK_FALSE(delivered->dispatch_refused);
    loom::Unverified u = loom::parse(delivered->payload);
    CHECK(u.claimed_name() == loom::Result::zen_name);
    // ...and the answerer saw the SESSION as the sender, never anything the wire could claim.
    CHECK(h.echo->last_sender_.value == client.session());
}

TEST_CASE("admission: what the grant does not cover is refused at the bus, and the guest is told") {
    GuestHost h([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_only("agent"));
    });
    // A participant that accepts the dispatch-refusal notice, so the shape is registered and the
    // proxy (accept-any) can receive it -- exactly as Workshop's terminal registers it.
    auto probe = sbfx::register_probe(h.bus, {schema_of<DispatchRefused>()});
    (void)probe;
    std::unique_ptr<loom::BridgeClient> c;
    loom::BridgeClient& client = connect_client(c, h.port, "agent", "");
    REQUIRE(client.await_admission(2000));
    // Echo to a WeaveId the grant does not cover (the probe) -- CapabilityDenied at delivery.
    client.send(probe.id.value, /*correlation=*/7, loom::serialize(loom::to_value(Echo{"no"})));
    std::vector<loom::BridgeEvent> events;
    REQUIRE(drain_events(
        client, events,
        [&] {
            for (const loom::BridgeEvent& e : events) {
                if (e.kind == loom::BridgeEvent::Kind::Delivered && e.dispatch_refused) {
                    return true;
                }
            }
            return false;
        },
        2000));
    bool right = false;
    for (const loom::BridgeEvent& e : events) {
        if (e.kind == loom::BridgeEvent::Kind::Delivered && e.dispatch_refused) {
            loom::Unverified u = loom::parse(e.payload);
            loom::Admission a = loom::admit(u, schema_of<DispatchRefused>());
            right = a.ok() && a.value().get("reason")->as_text() == "CapabilityDenied" &&
                    e.correlation == 7 && !e.answers_ask;
        }
    }
    CHECK(right);
}

TEST_CASE("admission: a deferred connection can act on nothing until decided; deciding admits or refuses") {
    // UNTHREADED: the decision is the HOST's act, made on the host's thread between steps --
    // which is exactly where a console command or a popup would make it.
    GuestHost h([](const loom::ConnectionRequest&) { return loom::ConnectionVerdict::defer(); },
                /*threaded=*/false);
    std::unique_ptr<loom::BridgeClient> c;
    loom::BridgeClient& client = connect_client(c, h.port, "agent", "");
    std::vector<loom::BridgeEvent> events;
    const auto step_until = [&](const std::function<bool()>& done, int timeout_ms) {
        return wait_until(
            [&] {
                h.server->step();
                std::vector<loom::BridgeEvent> got;
                client.poll(got);
                for (loom::BridgeEvent& e : got) {
                    events.push_back(std::move(e));
                }
                return done();
            },
            timeout_ms);
    };
    std::uint64_t waiting = 0;
    REQUIRE(step_until(
        [&] {
            for (const loom::Connection& t : h.reported()) {
                if (t.state == loom::ConnectionState::AwaitingDecision) {
                    waiting = t.connection;
                }
            }
            return waiting != 0;
        },
        2000));
    CHECK_FALSE(client.admitted()); // no verdict yet: neither admitted nor denied
    CHECK_FALSE(client.denied());
    CHECK(h.bus.list_weaves().size() == 1); // no proxy while the decision is open

    // A send while waiting is REFUSED ALOUD and reaches nothing.
    client.send_to_role("echo", 1, loom::serialize(loom::to_value(Echo{"early"})));
    REQUIRE(step_until(
        [&] {
            for (const loom::BridgeEvent& e : events) {
                if (e.kind == loom::BridgeEvent::Kind::SendRefused) {
                    return true;
                }
            }
            return false;
        },
        2000));
    CHECK(h.echo->heard() == 0);

    // THE DECISION, MADE LATER -- where a popup attaches. Deferring again is not a decision.
    CHECK_FALSE(h.server->decide(waiting, loom::ConnectionVerdict::defer()));
    CHECK(h.server->decide(waiting, loom::ConnectionVerdict::admit(echo_only("agent"))));
    CHECK_FALSE(h.server->decide(waiting, loom::ConnectionVerdict::admit(echo_only("twice"))));
    REQUIRE(step_until([&] { return client.admitted(); }, 2000));
    CHECK(client.established_name() == "agent");
    CHECK(h.bus.list_weaves().size() == 2);
    client.send_to_role("echo", 2, loom::serialize(loom::to_value(Echo{"now"})));
    REQUIRE(step_until([&] { return h.echo->heard() == 1; }, 2000));
    CHECK(h.echo->last_sender_.value == client.session());
}

TEST_CASE("admission: a peer speaking another protocol version is refused in words") {
    GuestHost h([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_only("agent"));
    });
    std::string err;
    const socket_t s = bridge_connect_tcp("127.0.0.1", h.port, &err);
    REQUIRE_MESSAGE(s != kInvalidSocket, err);
    BridgeChannel raw(s);
    std::string hello;
    put_u32(hello, 3); // a v3 console
    raw.queue(BridgeOp::Hello, hello);
    raw.flush();
    bool denied = false;
    std::string why;
    REQUIRE(wait_until(
        [&] {
            std::vector<BridgeIncoming> frames;
            raw.poll(frames);
            for (const BridgeIncoming& f : frames) {
                if (f.op == BridgeOp::Denied) {
                    Cursor cur(f.payload);
                    std::string_view w;
                    (void)cur.bytes(w);
                    why = std::string(w);
                    denied = true;
                }
            }
            return denied;
        },
        2000));
    CHECK(why.find("v3") != std::string::npos);
    CHECK(why.find("v4") != std::string::npos);
    CHECK(wait_until([&] { return raw.done() || h.server->connection_count() == 0; }, 2000));
}

TEST_CASE("admission: a guest is given no tap, and an operator still is") {
    GuestHost h([](const loom::ConnectionRequest& r) {
        if (r.claimed_name == "operator") {
            return loom::operator_admission()(r);
        }
        return loom::ConnectionVerdict::admit(echo_only("agent"));
    });
    std::unique_ptr<loom::BridgeClient> g;
    std::unique_ptr<loom::BridgeClient> o;
    loom::BridgeClient& guest = connect_client(g, h.port, "agent", "");
    loom::BridgeClient& op = connect_client(o, h.port, "operator", "");
    REQUIRE(guest.await_admission(2000));
    REQUIRE(op.await_admission(2000));
    guest.send_to_role("echo", 5, loom::serialize(loom::to_value(Echo{"tapped?"})));
    std::vector<loom::BridgeEvent> guest_events;
    std::vector<loom::BridgeEvent> op_events;
    REQUIRE(drain_events(
        guest, guest_events,
        [&] {
            for (const loom::BridgeEvent& e : guest_events) {
                if (e.kind == loom::BridgeEvent::Kind::Delivered) {
                    return true;
                }
            }
            return false;
        },
        2000));
    const bool op_saw_echo = drain_events(
        op, op_events,
        [&] {
            for (const loom::BridgeEvent& e : op_events) {
                if (e.kind == loom::BridgeEvent::Kind::Tap && e.shape == "Echo") {
                    return true;
                }
            }
            return false;
        },
        2000);
    REQUIRE(op_saw_echo);
    (void)drain_events(guest, guest_events, [] { return false; }, 200);
    bool guest_saw_tap = false;
    for (const loom::BridgeEvent& e : guest_events) {
        guest_saw_tap = guest_saw_tap || e.kind == loom::BridgeEvent::Kind::Tap;
    }
    CHECK_FALSE(guest_saw_tap);
}

TEST_CASE("identity: a disconnected session's proxy leaves the bus, and a late answer settles nothing") {
    GuestHost h([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_only("agent"));
    });
    std::uint64_t first = 0;
    {
        std::unique_ptr<loom::BridgeClient> c;
        loom::BridgeClient& client = connect_client(c, h.port, "agent", "");
        REQUIRE(client.await_admission(2000));
        first = client.session();
        CHECK(wait_until([&] { return h.bus.list_weaves().size() == 2; }, 2000));
    } // the socket closes here
    CHECK(wait_until([&] { return h.bus.list_weaves().size() == 1; }, 2000));
    // Two facts the server's reap establishes one after the other, on ITS thread: the proxy
    // leaves the bus, then the connection leaves the inventory. Wait for each.
    CHECK(wait_until([&] { return h.server->connection_count() == 0; }, 2000));
    std::unique_ptr<loom::BridgeClient> c2;
    loom::BridgeClient& again = connect_client(c2, h.port, "agent", "");
    REQUIRE(again.await_admission(2000));
    CHECK(again.session() != first); // a reconnect is a NEW session
    // A message aimed at the old session reaches nobody: the id is gone from the bus.
    std::atomic<bool> refused{false};
    const loom::ObserverId obs = h.bus.add_observer([&](const loom::BusEvent& e) {
        if (e.kind == loom::EventKind::Refused && e.target.value == first) {
            refused.store(true);
        }
    });
    (void)h.bus.send_as(h.echo_id, loom::WeaveId{first},
                        loom::Message(loom::to_value(loom::Result{"late"}), h.echo_id, {}, 42));
    CHECK(wait_until([&] { return refused.load(); }, 2000));
    h.bus.remove_observer(obs);
}

// ---- the link: two buses, one crossing, a typed ask and a typed answer ----------------------

namespace {

struct AskerState {
    std::int64_t answers = 0;
    ZEN_SHAPE(AskerState, 1, ZEN_FIELD(answers));
};

/// An ordinary weave on the LINKING host that asks across and settles on its own book.
class Asker final : public loom::WeaveBase<Asker, AskerState,
                                           loom::Accept<loom::Result, loom::link::Outcome, Echo>,
                                           loom::Emit<loom::link::Ask>> {
public:
    explicit Asker(std::string link_role) : link_role_(std::move(link_role)), book_(4) {}
    void on(const Echo&, loom::Mail& mail) {
        // Kicked by the test: open a conversation and ask across.
        const loom::AskOpened opened = book_.open_to_role(link_role_, "Echo", 1);
        REQUIRE(opened.ok);
        opened_ = opened.correlation;
        (void)mail.send_to_role(link_role_, loom::link::ask_role("echo", Echo{"across"}),
                                opened.correlation);
    }
    void on(const loom::Result& r, loom::Mail& mail) {
        const std::optional<loom::PendingAsk> settled =
            book_.settle(mail.correlation(), mail.sender());
        if (settled.has_value()) {
            ++state_.answers;
            last_ = r.value;
            link_sender_ = mail.sender();
        } else {
            ++unsolicited_;
        }
    }
    void on(const loom::link::Outcome& o, loom::Mail& mail) {
        (void)book_.settle(mail.correlation(), mail.sender());
        outcome_ = o.state + ": " + o.reason;
    }
    std::int64_t answers() const { return state_.answers; }
    std::string last_;
    std::string outcome_;
    loom::WeaveId link_sender_{};
    std::uint64_t opened_ = 0;
    int unsolicited_ = 0;

private:
    std::string link_role_;
    loom::AskBook book_;
};

} // namespace

TEST_CASE("link: an ordinary weave asks across two buses and settles the far answer on its own book") {
    GuestHost far([](const loom::ConnectionRequest& r) {
        return r.credential == "open-sesame" ? loom::ConnectionVerdict::admit(echo_only("agent"))
                                             : loom::ConnectionVerdict::refuse("wrong credential");
    });
    // THE LINKING HOST: its own bus, a link mounted as host wiring, and an asker.
    loom::Switchboard near;
    auto link = std::make_unique<loom::host::LinkWeave>(
        "far", "127.0.0.1:" + std::to_string(far.port), "agent", "open-sesame");
    loom::host::LinkWeave* raw = link.get();
    const loom::WeaveId link_id =
        near.register_weave(std::move(link), loom::Grant{}.allow_any(), loom::link::role_of("far"));
    raw->zen_set_self(link_id);
    raw->attach(near);
    std::string why;
    REQUIRE_MESSAGE(raw->connect(3000, &why), why);
    CHECK(raw->state() == "admitted");
    CHECK(raw->established_name() == "agent");

    loom::Grant may_ask;
    may_ask.allow_to_role(loom::link::Ask::zen_name, loom::link::Ask::zen_version,
                          loom::link::role_of("far"));
    Asker* asker = nullptr;
    loom::WeaveId asker_id{};
    {
        auto a = std::make_unique<Asker>(loom::link::role_of("far"));
        asker = a.get();
        asker_id = near.register_weave(std::move(a), may_ask);
        asker->zen_set_self(asker_id);
        // Kick the asker with a root send; it opens its book and asks across.
        (void)near.send(asker_id, loom::Message(loom::to_value(Echo{"kick"})));
    }
    REQUIRE(wait_until(
        [&] {
            raw->service();
            near.pump_pending();
            return asker->answers() == 1;
        },
        3000));
    CHECK(asker->last_ == "echo:across");
    CHECK(asker->link_sender_ == link_id); // settled on the LINK's stamp, never a far id
    CHECK(asker->unsolicited_ == 0);
    CHECK(raw->open() == 0);
    CHECK(far.echo->last_sender_.value == raw->session());

    // A SECOND ASK AFTER THE FAR HOST GOES AWAY IS `unlinked`, and never resent.
    far.stop.store(true);
    if (far.th.joinable()) {
        far.th.join();
    }
    far.server.reset(); // closes the listener and every connection
    REQUIRE(wait_until(
        [&] {
            raw->service();
            near.pump_pending();
            return raw->state() == "lost";
        },
        3000));
    (void)near.send(asker_id, loom::Message(loom::to_value(Echo{"kick"})));
    REQUIRE(wait_until(
        [&] {
            raw->service();
            near.pump_pending();
            return !asker->outcome_.empty();
        },
        3000));
    CHECK(asker->outcome_.rfind(loom::link::kOutcomeUnlinked, 0) == 0);
    CHECK(asker->answers() == 1);
}

// ---- the link keeps each crossing its own, and says which words are answers -------------------
//
// Two separate askers each keep a book, and a book's first conversation is 1: two askers
// asking across one link at once hold the SAME local correlation. The far side must still
// answer each one its own answer -- in whatever order it answers -- and what reaches an asker
// must say which words are Loom's answer and which are somebody's ordinary speech.

namespace {

/// A far participant that holds the FIRST ask it hears and answers the second first: the
/// order a crossing must not depend on.
class ReverseAnswerer final
    : public loom::WeaveBase<ReverseAnswerer, EchoState, loom::Accept<Echo>, loom::Emit<loom::Result>> {
public:
    void on(const Echo& e, loom::Mail& mail) {
        ++state_.heard;
        if (!first_.valid()) {
            first_ = mail.defer_answer();
            first_msg_ = e.msg;
            return;
        }
        (void)mail.answer(loom::Result{"echo:" + e.msg});
        loom::DeferredAnswer due = std::move(first_);
        first_ = loom::DeferredAnswer{};
        (void)loom::answer_deferred(due, mail, loom::Result{"echo:" + first_msg_});
    }

private:
    loom::DeferredAnswer first_;
    std::string first_msg_;
};

/// A far participant that says something ORDINARY to the asking session under the ask's own
/// correlation before it answers: the word a crossing must not hand over as the answer.
class ChattyAnswerer final
    : public loom::WeaveBase<ChattyAnswerer, EchoState, loom::Accept<Echo>, loom::Emit<loom::Result>> {
public:
    void on(const Echo& e, loom::Mail& mail) {
        ++state_.heard;
        (void)mail.send(mail.sender(), loom::Result{"imposter"}, mail.correlation());
        (void)mail.answer(loom::Result{"echo:" + e.msg});
    }
};

/// A near asker that writes down EVERYTHING it is handed and what Loom said about it, and
/// settles its own book only on Loom's answer from the link.
class WitnessAsker final : public loom::WeaveBase<WitnessAsker, AskerState,
                                                  loom::Accept<loom::Result, loom::link::Outcome, Echo>,
                                                  loom::Emit<loom::link::Ask>> {
public:
    struct Heard {
        std::string text;
        loom::WeaveId sender{};
        std::uint64_t correlation = 0;
        bool answers_ask = false;
        bool settled = false;
    };
    WitnessAsker(std::string link_role, std::string far_role)
        : link_role_(std::move(link_role)), far_role_(std::move(far_role)), book_(4) {}
    /// Ask for settlement too; and, when set, send this envelope instead of the Echo.
    bool settle = false;
    std::optional<loom::link::Ask> instead;
    void on(const Echo& kick, loom::Mail& mail) {
        const loom::AskOpened opened = book_.open_to_role(link_role_, "Echo", 1);
        REQUIRE(opened.ok);
        opened_ = opened.correlation;
        loom::link::Ask ask =
            instead ? *instead : loom::link::ask_role(far_role_, Echo{kick.msg}, settle);
        (void)mail.send_to_role(link_role_, ask, opened.correlation);
    }
    void on(const loom::Result& r, loom::Mail& mail) { note(r.value, mail); }
    void on(const loom::link::Outcome& o, loom::Mail& mail) {
        last_outcome_ = o;
        note("outcome:" + o.state, mail);
    }
    std::optional<loom::link::Outcome> last_outcome_;
    /// Everything this asker was handed, in order, as one line a failing case prints.
    std::string account() const {
        std::string out;
        for (const Heard& h : heard_) {
            out += "[" + h.text + " from #" + std::to_string(h.sender.value) + " corr " +
                   std::to_string(h.correlation) + (h.answers_ask ? " ANSWER" : " ordinary") +
                   (h.settled ? " settled] " : "] ");
        }
        return out.empty() ? std::string("(nothing)") : out;
    }
    /// The one answer this asker's book settled on, or empty.
    std::string settled() const {
        for (const Heard& h : heard_) {
            if (h.settled) {
                return h.text;
            }
        }
        return {};
    }
    const Heard* settled_record() const {
        for (const Heard& h : heard_) {
            if (h.settled) {
                return &h;
            }
        }
        return nullptr;
    }
    std::vector<Heard> heard_;
    std::uint64_t opened_ = 0;

private:
    void note(std::string text, loom::Mail& mail) {
        Heard h;
        h.text = std::move(text);
        h.sender = mail.sender();
        h.correlation = mail.correlation();
        h.answers_ask = mail.answers_ask();
        // THE ASKER'S OWN WALL: an ask to an office is settled by Loom's answer and nothing less.
        h.settled = h.answers_ask && book_.settle(mail.correlation(), mail.sender()).has_value();
        heard_.push_back(std::move(h));
    }
    std::string link_role_;
    std::string far_role_;
    loom::AskBook book_;
};

/// The far host, stepped by hand on the test's own thread, with the participants a case names.
struct FarHost {
    GuestHost host;
    explicit FarHost(loom::BridgeAdmission policy) : host(std::move(policy), /*threaded=*/false) {}
    template <class W, class... A>
    W* mount(const std::string& role, A&&... args) {
        auto w = std::make_unique<W>(std::forward<A>(args)...);
        W* raw = w.get();
        const loom::WeaveId id = host.bus.register_weave(std::move(w), loom::Grant{}.allow_any(), role);
        raw->zen_set_self(id);
        return raw;
    }
};

/// The near host: a bus, one link to the far host, and askers.
struct NearHost {
    loom::Switchboard bus;
    loom::host::LinkWeave* link = nullptr;
    loom::WeaveId link_id{};
    explicit NearHost(std::uint16_t far_port, const char* identity = "agent",
                      const char* credential = "open-sesame") {
        auto l = std::make_unique<loom::host::LinkWeave>(
            "far", "127.0.0.1:" + std::to_string(far_port), identity, credential);
        link = l.get();
        link_id = bus.register_weave(std::move(l), loom::Grant{}.allow_any(), loom::link::role_of("far"));
        link->zen_set_self(link_id);
        link->attach(bus);
    }
    template <class W, class... A>
    W* mount(loom::WeaveId* id_out, A&&... args) {
        loom::Grant may_ask;
        may_ask.allow_to_role(loom::link::Ask::zen_name, loom::link::Ask::zen_version,
                              loom::link::role_of("far"));
        auto w = std::make_unique<W>(std::forward<A>(args)...);
        W* raw = w.get();
        *id_out = bus.register_weave(std::move(w), may_ask);
        raw->zen_set_self(*id_out);
        return raw;
    }
    void kick(loom::WeaveId who, const std::string& msg) {
        (void)bus.send(who, loom::Message(loom::to_value(Echo{msg})));
    }
};

/// Both hosts take turns until `done` holds, or a bounded number of turns pass.
bool turn_until(FarHost& far, NearHost& near, const std::function<bool()>& done, int timeout_ms = 3000) {
    return wait_until(
        [&] {
            far.host.server->step();
            near.link->service();
            near.bus.pump_pending();
            return done();
        },
        timeout_ms);
}

loom::ConnectionAdmitted echo_roles(std::string name, std::initializer_list<const char*> roles) {
    loom::ConnectionAdmitted a;
    for (const char* role : roles) {
        a.grant.allow_to_role(Echo::zen_name, Echo::zen_version, role);
    }
    a.established_name = std::move(name);
    return a;
}

} // namespace

TEST_CASE("link: two askers with the same correlation are each answered their own, in any order") {
    FarHost far([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_roles("agent", {"hold"}));
    });
    far.mount<ReverseAnswerer>("hold");
    NearHost near(far.host.port);
    std::string why;
    std::atomic<bool> connected{false};
    std::thread connecting([&] { connected.store(near.link->connect(3000, &why)); });
    (void)wait_until([&] { far.host.server->step(); return connected.load(); }, 3000);
    connecting.join();
    REQUIRE_MESSAGE(connected.load(), why);
    loom::WeaveId a_id{};
    loom::WeaveId b_id{};
    WitnessAsker* a = near.mount<WitnessAsker>(&a_id, loom::link::role_of("far"), "hold");
    WitnessAsker* b = near.mount<WitnessAsker>(&b_id, loom::link::role_of("far"), "hold");
    near.kick(a_id, "A");
    near.bus.pump_pending(); // A asks first...
    near.kick(b_id, "B");
    near.bus.pump_pending(); // ...then B, under the SAME local correlation
    REQUIRE(a->opened_ == b->opened_);
    REQUIRE(turn_until(far, near, [&] { return !a->heard_.empty() && !b->heard_.empty(); }));
    // The far side answered B first. Each asker still has its own answer, and Loom's word on it.
    INFO("A heard: " << a->account());
    INFO("B heard: " << b->account());
    CHECK(a->settled() == "echo:A");
    CHECK(b->settled() == "echo:B");
    REQUIRE(a->settled_record() != nullptr);
    CHECK(a->settled_record()->sender == near.link_id);
    CHECK(a->settled_record()->correlation == a->opened_);
    CHECK(near.link->open() == 0);
}

TEST_CASE("link: a far participant's ordinary word under an ask's correlation is not its answer") {
    FarHost far([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_roles("agent", {"chatter"}));
    });
    far.mount<ChattyAnswerer>("chatter");
    NearHost near(far.host.port);
    std::string why;
    std::atomic<bool> connected{false};
    std::thread connecting([&] { connected.store(near.link->connect(3000, &why)); });
    (void)wait_until([&] { far.host.server->step(); return connected.load(); }, 3000);
    connecting.join();
    REQUIRE_MESSAGE(connected.load(), why);
    loom::WeaveId asker_id{};
    WitnessAsker* asker = near.mount<WitnessAsker>(&asker_id, loom::link::role_of("far"), "chatter");
    near.kick(asker_id, "x");
    const bool settled = turn_until(far, near, [&] { return !asker->settled().empty(); });
    INFO("the asker heard: " << asker->account());
    REQUIRE(settled);
    // The ordinary word did not settle the ask, and nothing handed it over as Loom's answer.
    CHECK(asker->settled() == "echo:x");
    for (const WitnessAsker::Heard& h : asker->heard_) {
        if (h.text == "imposter") {
            CHECK_FALSE(h.answers_ask);
            CHECK_FALSE(h.settled);
        }
    }
    REQUIRE(asker->settled_record() != nullptr);
    CHECK(asker->settled_record()->answers_ask);
}

namespace {

/// A far participant that answers at once and then takes `steps` deliveries of its own -- the
/// work an ask SETS IN MOTION on the far bus, which its answer does not wait for.
struct FarStep {
    std::int64_t left = 0;
    ZEN_SHAPE(FarStep, 1, ZEN_FIELD(left));
};
class BusyAnswerer final : public loom::WeaveBase<BusyAnswerer, EchoState, loom::Accept<Echo, FarStep>,
                                                  loom::Emit<loom::Result, FarStep>> {
public:
    std::int64_t steps = 3;
    bool done = false;
    void on(const Echo& e, loom::Mail& mail) {
        ++state_.heard;
        done = false;
        (void)mail.answer(loom::Result{"echo:" + e.msg});
        (void)mail.send(this->self_, FarStep{steps});
    }
    void on(const FarStep& s, loom::Mail& mail) {
        if (s.left > 0) {
            (void)mail.send(this->self_, FarStep{s.left - 1});
        } else {
            done = true;
        }
    }
};

/// A shape the far host has never heard of.
struct NearOnly {
    std::string note;
    ZEN_SHAPE(NearOnly, 1, ZEN_FIELD(note));
};

/// A local participant that tells the link a story about a crossing.
class Storyteller final : public loom::WeaveBase<Storyteller, EchoState, loom::Accept<Echo>,
                                                 loom::Emit<loom::link::Crossed>> {
public:
    loom::WeaveId link{};
    loom::link::Crossed story;
    void on(const Echo&, loom::Mail& mail) { (void)mail.send(link, story); }
};

/// A FAR HOST THAT SAYS EXACTLY WHAT A CASE SCRIPTS -- raw frames on a real socket, for the
/// replies a well-behaved far bus never produces: a duplicate, a stale attempt, an answer in the
/// link's own vocabulary, settlement before or after the answer.
struct ScriptedFar {
    struct Seen {
        std::uint64_t correlation = 0;
        std::uint8_t flags = 0;
        std::string role;
    };
    socket_t listener = kInvalidSocket;
    std::uint16_t port = 0;
    std::unique_ptr<BridgeChannel> ch;
    std::vector<Seen> sends;
    std::uint64_t session = 40;

    ScriptedFar() {
        std::string err;
        listener = bridge_listen_tcp(0, &err);
        REQUIRE_MESSAGE(listener != kInvalidSocket, err);
        port = bridge_socket_port(listener);
    }
    ~ScriptedFar() {
        ch.reset();
        bridge_close(listener);
    }
    /// Accept the next connection and welcome it (a fresh session number each time).
    bool welcome() {
        return wait_until(
            [&] {
                if (!ch) {
                    bool would_block = false;
                    std::string err;
                    const socket_t s = bridge_accept(listener, &would_block, &err);
                    if (s == kInvalidSocket) {
                        return false;
                    }
                    ch = std::make_unique<BridgeChannel>(s);
                }
                std::vector<BridgeIncoming> frames;
                ch->poll(frames);
                for (const BridgeIncoming& f : frames) {
                    if (f.op == BridgeOp::Hello) {
                        std::string w;
                        put_u64(w, ++session);
                        put_u32(w, kBridgeProtocolVersion);
                        put_bytes(w, "agent");
                        ch->queue(BridgeOp::Welcome, w);
                        ch->flush();
                        return true;
                    }
                }
                return false;
            },
            3000);
    }
    void poll() {
        if (!ch) {
            return;
        }
        std::vector<BridgeIncoming> frames;
        ch->poll(frames);
        for (const BridgeIncoming& f : frames) {
            if (f.op != BridgeOp::Send) {
                continue;
            }
            Cursor cur(f.payload);
            std::uint8_t kind = 0;
            Seen seen;
            std::uint64_t ignored = 0;
            std::string_view role;
            if (cur.u8(kind) && cur.u8(seen.flags) && cur.u64(ignored) && cur.u64(ignored) &&
                cur.u64(ignored) && cur.u64(seen.correlation) && cur.bytes(role)) {
                seen.role = std::string(role);
                sends.push_back(seen);
            }
        }
    }
    void deliver(std::uint64_t correlation, std::uint8_t flags, const loom::Value& v) {
        std::string body;
        put_u64(body, 77); // the far bus's stamp: a far number, never a local id
        put_u64(body, correlation);
        put_u8(body, flags);
        put_bytes(body, "");
        body.append(serialize(v));
        ch->queue(BridgeOp::Delivered, body);
        ch->flush();
    }
    void settled(std::uint64_t correlation) {
        std::string body;
        put_u64(body, correlation);
        ch->queue(BridgeOp::Settled, body);
        ch->flush();
    }
    void drop() { ch.reset(); }
};

/// Connect the near host's link while the scripted far host welcomes it.
void link_to(NearHost& near, ScriptedFar& far) {
    std::string why;
    std::atomic<bool> connected{false};
    std::thread connecting([&] { connected.store(near.link->connect(3000, &why)); });
    const bool welcomed = far.welcome();
    connecting.join();
    REQUIRE(welcomed);
    REQUIRE_MESSAGE(connected.load(), why);
}

bool turn_until(ScriptedFar& far, NearHost& near, const std::function<bool()>& done,
                int timeout_ms = 3000) {
    return wait_until(
        [&] {
            far.poll();
            near.link->service();
            near.bus.pump_pending();
            return done();
        },
        timeout_ms);
}

void link_to(NearHost& near, FarHost& far) {
    std::string why;
    std::atomic<bool> connected{false};
    std::thread connecting([&] { connected.store(near.link->connect(3000, &why)); });
    (void)wait_until([&] { far.host.server->step(); return connected.load(); }, 3000);
    connecting.join();
    REQUIRE_MESSAGE(connected.load(), why);
}

} // namespace

TEST_CASE("link: answers and refusals for askers under one correlation each reach their own asker") {
    FarHost far([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_roles("agent", {"hold"})); // not "chatter"
    });
    far.mount<ChattyAnswerer>("chatter");
    far.mount<ReverseAnswerer>("hold");
    NearHost near(far.host.port);
    link_to(near, far);
    loom::WeaveId refused_id{};
    loom::WeaveId answered_id{};
    loom::WeaveId unknown_id{};
    WitnessAsker* refused = near.mount<WitnessAsker>(&refused_id, loom::link::role_of("far"), "chatter");
    WitnessAsker* answered = near.mount<WitnessAsker>(&answered_id, loom::link::role_of("far"), "hold");
    WitnessAsker* unknown = near.mount<WitnessAsker>(&unknown_id, loom::link::role_of("far"), "hold");
    unknown->instead = loom::link::ask_role("hold", NearOnly{"a shape the far host never heard of"});
    near.kick(refused_id, "no");
    near.kick(answered_id, "yes");
    near.kick(unknown_id, "?");
    near.bus.pump_pending();
    REQUIRE(refused->opened_ == answered->opened_);
    REQUIRE(answered->opened_ == unknown->opened_);
    // `hold` answers the second ask first, so hand it one more of its own afterwards.
    loom::WeaveId second_id{};
    WitnessAsker* second = near.mount<WitnessAsker>(&second_id, loom::link::role_of("far"), "hold");
    near.kick(second_id, "second");
    REQUIRE(turn_until(far, near, [&] {
        return !refused->settled().empty() && !answered->settled().empty() &&
               !unknown->settled().empty() && !second->settled().empty();
    }));
    INFO("refused: " << refused->account() << " | answered: " << answered->account()
                     << " | unknown: " << unknown->account());
    CHECK(refused->settled() == "outcome:dispatch-refused");
    REQUIRE(refused->last_outcome_.has_value());
    CHECK(refused->last_outcome_->reason == "CapabilityDenied");
    CHECK(refused->last_outcome_->attempt != 0);
    CHECK(answered->settled() == "echo:yes");
    CHECK(second->settled() == "echo:second");
    CHECK(unknown->settled() == "outcome:refused");
    REQUIRE(unknown->last_outcome_.has_value());
    CHECK(unknown->last_outcome_->reason.find("unknown schema: NearOnly") != std::string::npos);
    CHECK(near.link->open() == 0);
}

TEST_CASE("link: a settle-requested ask is answered only once what it set in motion far away is done") {
    FarHost far([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_roles("agent", {"busy"}));
    });
    far.host.server->set_bounded_dispatch(); // one far turn per step, so the far work spans steps
    BusyAnswerer* busy = far.mount<BusyAnswerer>("busy");
    NearHost near(far.host.port);
    link_to(near, far);
    loom::WeaveId plain_id{};
    WitnessAsker* plain = near.mount<WitnessAsker>(&plain_id, loom::link::role_of("far"), "busy");
    near.kick(plain_id, "plain");
    bool done_when_plain_answered = true;
    REQUIRE(turn_until(far, near, [&] {
        if (!plain->settled().empty()) {
            done_when_plain_answered = busy->done;
            return true;
        }
        return false;
    }));
    // Without settlement the answer is the far owner's word alone: it came while its work ran.
    CHECK_FALSE(done_when_plain_answered);
    REQUIRE(turn_until(far, near, [&] { return busy->done; }));
    loom::WeaveId settled_id{};
    WitnessAsker* settled = near.mount<WitnessAsker>(&settled_id, loom::link::role_of("far"), "busy");
    settled->settle = true;
    near.kick(settled_id, "settled");
    bool done_when_settled_answered = false;
    REQUIRE(turn_until(far, near, [&] {
        if (!settled->settled().empty()) {
            done_when_settled_answered = busy->done;
            return true;
        }
        return false;
    }));
    CHECK(settled->settled() == "echo:settled");
    CHECK(done_when_settled_answered);
    CHECK(near.link->open() == 0);
}

TEST_CASE("link: a duplicate, an unknown attempt and a reply for an ended session settle nothing") {
    ScriptedFar far;
    NearHost near(far.port);
    link_to(near, far);
    loom::WeaveId first_id{};
    WitnessAsker* first = near.mount<WitnessAsker>(&first_id, loom::link::role_of("far"), "echo");
    near.kick(first_id, "one");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 1; }));
    const std::uint64_t attempt = far.sends[0].correlation;
    far.deliver(attempt, kDeliveredAnswersAsk, to_value(loom::Result{"echo:one"}));
    far.deliver(attempt, kDeliveredAnswersAsk, to_value(loom::Result{"echo:again"})); // duplicate
    far.deliver(attempt + 1000, kDeliveredAnswersAsk, to_value(loom::Result{"echo:nobody"}));
    REQUIRE(turn_until(far, near, [&] { return !first->settled().empty(); }));
    near.bus.pump_pending();
    CHECK(first->settled() == "echo:one");
    CHECK(first->heard_.size() == 1);
    // A NEW SESSION: whatever is open on the old one is lost, and the old one's numbers name
    // nothing on the new.
    loom::WeaveId waiting_id{};
    WitnessAsker* waiting = near.mount<WitnessAsker>(&waiting_id, loom::link::role_of("far"), "echo");
    near.kick(waiting_id, "two");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 2; }));
    const std::uint64_t old_attempt = far.sends[1].correlation;
    far.drop();
    link_to(near, far); // the link reconnects: a new epoch
    REQUIRE(turn_until(far, near, [&] { return !waiting->settled().empty(); }));
    CHECK(waiting->settled() == "outcome:lost");
    loom::WeaveId third_id{};
    WitnessAsker* third = near.mount<WitnessAsker>(&third_id, loom::link::role_of("far"), "echo");
    near.kick(third_id, "three");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 3; }));
    const std::uint64_t new_attempt = far.sends[2].correlation;
    CHECK(new_attempt != old_attempt);
    far.deliver(old_attempt, kDeliveredAnswersAsk, to_value(loom::Result{"echo:stale"}));
    far.deliver(new_attempt, kDeliveredAnswersAsk, to_value(loom::Result{"echo:three"}));
    REQUIRE(turn_until(far, near, [&] { return !third->settled().empty(); }));
    CHECK(third->settled() == "echo:three");
    CHECK(waiting->heard_.size() == 1); // the stale answer reached nobody
}

TEST_CASE("link: settlement and the answer may come in either order; the answer waits for both") {
    ScriptedFar far;
    NearHost near(far.port);
    link_to(near, far);
    loom::WeaveId a_id{};
    WitnessAsker* a = near.mount<WitnessAsker>(&a_id, loom::link::role_of("far"), "echo");
    a->settle = true;
    near.kick(a_id, "a");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 1; }));
    CHECK((far.sends[0].flags & kSendSettle) != 0);
    // The answer first: held.
    far.deliver(far.sends[0].correlation, kDeliveredAnswersAsk, to_value(loom::Result{"echo:a"}));
    (void)turn_until(far, near, [] { return false; }, 200);
    CHECK(a->heard_.empty());
    far.settled(far.sends[0].correlation);
    REQUIRE(turn_until(far, near, [&] { return !a->settled().empty(); }));
    CHECK(a->settled() == "echo:a");
    // Settlement first -- a far owner that answers later: the answer is handed on when it comes.
    loom::WeaveId b_id{};
    WitnessAsker* b = near.mount<WitnessAsker>(&b_id, loom::link::role_of("far"), "echo");
    b->settle = true;
    near.kick(b_id, "b");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 2; }));
    far.settled(far.sends[1].correlation);
    (void)turn_until(far, near, [] { return false; }, 200);
    CHECK(b->heard_.empty());
    far.deliver(far.sends[1].correlation, kDeliveredAnswersAsk, to_value(loom::Result{"echo:b"}));
    REQUIRE(turn_until(far, near, [&] { return !b->settled().empty(); }));
    CHECK(b->settled() == "echo:b");
    // A session that ends while an answer waits on its settlement: lost, and saying so.
    loom::WeaveId c_id{};
    WitnessAsker* c = near.mount<WitnessAsker>(&c_id, loom::link::role_of("far"), "echo");
    c->settle = true;
    near.kick(c_id, "c");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 3; }));
    far.deliver(far.sends[2].correlation, kDeliveredAnswersAsk, to_value(loom::Result{"echo:c"}));
    (void)turn_until(far, near, [] { return false; }, 200);
    far.drop();
    REQUIRE(turn_until(far, near, [&] { return !c->settled().empty(); }));
    CHECK(c->settled() == "outcome:lost");
    REQUIRE(c->last_outcome_.has_value());
    CHECK(c->last_outcome_->reason.find("had answered") != std::string::npos);
}

TEST_CASE("link: an answer in the link's own vocabulary is refused, never spoken in the link's voice") {
    ScriptedFar far;
    NearHost near(far.port);
    link_to(near, far);
    loom::WeaveId a_id{};
    WitnessAsker* a = near.mount<WitnessAsker>(&a_id, loom::link::role_of("far"), "echo");
    near.kick(a_id, "a");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 1; }));
    loom::link::Outcome fake;
    fake.state = loom::link::kOutcomeLost;
    fake.reason = "the far owner pretending to be the link";
    far.deliver(far.sends[0].correlation, kDeliveredAnswersAsk, to_value(fake));
    REQUIRE(turn_until(far, near, [&] { return !a->settled().empty(); }));
    REQUIRE(a->last_outcome_.has_value());
    CHECK(a->last_outcome_->state == loom::link::kOutcomeRefused);
    CHECK(a->last_outcome_->reason.find("this link's own vocabulary") != std::string::npos);
}

TEST_CASE("link: only the link's own account of a crossing acts") {
    ScriptedFar far;
    NearHost near(far.port);
    link_to(near, far);
    loom::WeaveId a_id{};
    WitnessAsker* a = near.mount<WitnessAsker>(&a_id, loom::link::role_of("far"), "echo");
    near.kick(a_id, "a");
    REQUIRE(turn_until(far, near, [&] { return far.sends.size() == 1; }));
    // A local participant allowed to speak to the link tells it the far owner answered.
    loom::Grant may_tell;
    may_tell.allow(loom::link::Crossed::zen_name, loom::link::Crossed::zen_version, near.link_id);
    auto teller = std::make_unique<Storyteller>();
    Storyteller* raw = teller.get();
    const loom::WeaveId teller_id = near.bus.register_weave(std::move(teller), may_tell);
    raw->zen_set_self(teller_id);
    raw->link = near.link_id;
    raw->story.kind = loom::link::kCrossedAnswer;
    raw->story.epoch = static_cast<std::int64_t>(near.link->epoch());
    raw->story.attempt = static_cast<std::int64_t>(far.sends[0].correlation);
    const std::string forged = serialize(to_value(loom::Result{"forged"}));
    raw->story.payload.assign(forged.begin(), forged.end());
    (void)near.bus.send(teller_id, loom::Message(loom::to_value(Echo{"tell"})));
    (void)turn_until(far, near, [] { return false; }, 200);
    CHECK(a->heard_.empty());
    far.deliver(far.sends[0].correlation, kDeliveredAnswersAsk, to_value(loom::Result{"echo:a"}));
    REQUIRE(turn_until(far, near, [&] { return !a->settled().empty(); }));
    CHECK(a->settled() == "echo:a");
}

TEST_CASE("link: an asker replaced before its answer came is answered nothing its predecessor earned") {
    FarHost far([](const loom::ConnectionRequest&) {
        return loom::ConnectionVerdict::admit(echo_roles("agent", {"hold"}));
    });
    far.mount<ReverseAnswerer>("hold");
    NearHost near(far.host.port);
    link_to(near, far);
    loom::WeaveId old_id{};
    WitnessAsker* old_asker = near.mount<WitnessAsker>(&old_id, loom::link::role_of("far"), "hold");
    near.kick(old_id, "old");
    REQUIRE(turn_until(far, near, [&] { return near.link->open() == 1 && far.host.bus.pending() == 0; }));
    (void)old_asker;
    // The asker goes away; a new one takes its place and asks under the same correlation.
    REQUIRE(near.bus.unregister_weave(old_id) != nullptr);
    loom::WeaveId new_id{};
    WitnessAsker* fresh = near.mount<WitnessAsker>(&new_id, loom::link::role_of("far"), "hold");
    near.kick(new_id, "new");
    // The far side answers the new ask first, then the old one's.
    REQUIRE(turn_until(far, near, [&] { return !fresh->settled().empty() && near.link->open() == 0; }));
    near.bus.pump_pending();
    INFO("the new asker heard: " << fresh->account());
    CHECK(fresh->settled() == "echo:new");
    CHECK(fresh->heard_.size() == 1);
}

// THE DOOR DIES WITH THE BUS. Workshop mounts its guest door AS A WEAVE that owns a
// BridgeServer, so the server's destructor -- which unregisters its proxies and removes its
// tap observer -- runs from inside the Switchboard's own destructor. Before the registry was
// emptied first, that was an erase from a map already being torn down (a SIGSEGV in
// Workshop's guests suite, at teardown, with a guest still admitted). This case is that
// exact shape: a weave-owned server with one admitted, still-connected guest, destroyed by
// the bus alone.
struct DoorWeave final : public loom::WeaveBase<DoorWeave, EchoState, loom::Accept<>, loom::Emit<>> {
    std::unique_ptr<loom::BridgeServer> server;
    explicit DoorWeave(std::unique_ptr<loom::BridgeServer> s) : server(std::move(s)) {}
};

TEST_CASE("teardown: a weave that owns a BridgeServer may die with the bus, guest still connected") {
    std::unique_ptr<loom::BridgeClient> guest;
    {
        loom::Switchboard bus;
        auto e = std::make_unique<EchoAnswerer>();
        EchoAnswerer* echo = e.get();
        const loom::WeaveId echo_id =
            bus.register_weave(std::move(e), loom::Grant{}.allow_any(), std::string("echo"));
        echo->zen_set_self(echo_id);
        std::string err;
        const socket_t listener = bridge_listen_tcp(0, &err);
        REQUIRE_MESSAGE(listener != kInvalidSocket, err);
        const std::uint16_t port = bridge_socket_port(listener);
        auto server = std::make_unique<loom::BridgeServer>(
            bus, listener, [](const loom::ConnectionRequest&) {
                return loom::ConnectionVerdict::admit(echo_only("agent"));
            });
        loom::BridgeServer* raw_server = server.get();
        auto door = std::make_unique<DoorWeave>(std::move(server));
        DoorWeave* raw_door = door.get();
        const loom::WeaveId door_id = bus.register_weave(std::move(door), loom::Grant{});
        raw_door->zen_set_self(door_id);

        (void)connect_client(guest, port, "guest", "any");
        for (int i = 0; i < 200 && raw_server->connections().empty(); ++i) {
            raw_server->step();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int i = 0; i < 200; ++i) {
            raw_server->step();
            if (!raw_server->connections().empty() &&
                raw_server->connections().front().state == loom::ConnectionState::Admitted) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(raw_server->connections().size() == 1);
        CHECK(raw_server->connections().front().state == loom::ConnectionState::Admitted);
        // ...and the bus goes out of scope with the door, the server and the proxy inside it.
    }
    // Reaching here without a crash IS the witness; the guest simply finds its peer gone.
    CHECK(true);
}

} // TEST_SUITE("bridge")
