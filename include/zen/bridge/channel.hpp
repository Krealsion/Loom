// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_CHANNEL_HPP
#define ZEN_BRIDGE_CHANNEL_HPP

// A portable, non-blocking, length-framed byte channel over a stream socket: the transport the
// bridge protocol (zen/bridge/protocol.hpp) runs over. It frames as the isolation Channel does and
// works on both a POSIX fd and a Winsock SOCKET, so a Windows client can reach a bus hosted
// elsewhere; only the raw socket calls differ per platform. It runs over an AF_UNIX socket and
// loopback TCP alike. A frame or an undrained backlog over the cap fails the channel, so a
// misbehaving peer cannot block, hang or exhaust memory, and a peer's close is observable (EOF).

#include <zen/bridge/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

// A platform socket handle. POSIX: an int fd. Windows: a Winsock SOCKET (UINT_PTR) — kept as an
// integer typedef here so this header pulls in no platform socket headers (winsock2.h is heavy and
// order-sensitive; it lives only in channel.cpp).
#ifdef _WIN32
using socket_t = std::uintptr_t;
inline constexpr socket_t kInvalidSocket = ~static_cast<socket_t>(0); // == INVALID_SOCKET
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = static_cast<socket_t>(-1);
#endif

struct BridgeIncoming {
    BridgeOp op = BridgeOp::Hello;
    std::string payload;
};

class BridgeChannel {
public:
    /// Takes ownership of `sock` and sets it non-blocking.
    explicit BridgeChannel(socket_t sock);
    ~BridgeChannel();

    BridgeChannel(const BridgeChannel&) = delete;
    BridgeChannel& operator=(const BridgeChannel&) = delete;

    socket_t fd() const noexcept { return fd_; }
    bool failed() const noexcept { return failed_; }
    bool eof() const noexcept { return eof_; }
    bool done() const noexcept { return failed_ || eof_; }

    /// Mark the channel failed so the existing done()/reap path tears it down — the explicit
    /// affordance a protocol-level violation (a frame before the handshake) uses to sever a
    /// connection without inventing a parallel teardown.
    void fail() noexcept { failed_ = true; }

    /// Buffer a frame for sending; flush() writes it. An over-cap backlog fails the channel.
    void queue(BridgeOp op, std::string_view payload);

    /// Write as much of the outbound buffer as the socket accepts, without blocking (a full kernel
    /// buffer leaves the rest queued for next time).
    void flush();

    /// Read available bytes without blocking and append every complete frame to `out`. Sets eof()
    /// on peer close and failed() on a protocol/IO error.
    void poll(std::vector<BridgeIncoming>& out);

private:
    /// TEST SEAM ONLY: reads the retained transport buffers so a regression test can state the
    /// bounded-storage law (LIFE-07) as an assertion about this channel's own state rather than
    /// inferring it from process memory, which is allocator- and OS-sensitive. A friend
    /// declaration adds no member, no vtable entry and no code path -- it cannot change behavior --
    /// and the name is never declared for ordinary lookup, so it is not part of the public surface.
    /// Defined in tests/test_bridge.cpp; no production caller.
    friend struct BridgeChannelStorageProbe;

    socket_t fd_;
    std::string outbox_;
    std::size_t out_pos_ = 0;
    std::string inbox_;
    bool failed_ = false;
    bool eof_ = false;
};

// ---- Socket setup helpers (POSIX + Winsock, in channel.cpp) -------------------------------------
//
// One-time process init (Winsock WSAStartup; a no-op on POSIX). connect/listen call it; call it once
// up front in a main() to be explicit. Returns false + sets *err on failure.
bool bridge_net_init(std::string* err);

/// Listen on an AF_UNIX path, unlinking a stale one first. Returns a listening, non-blocking
/// socket or kInvalidSocket (+ sets *err). POSIX only (Windows AF_UNIX interop is unreliable, so
/// a crossing from Windows uses TCP). The path is removed on close.
socket_t bridge_listen_unix(const std::string& path, std::string* err);

/// Listen on AF_INET 127.0.0.1:`port`; port 0 asks the OS to choose, read back with
/// bridge_socket_port(). Returns a listening, non-blocking socket or kInvalidSocket (+ sets *err).
socket_t bridge_listen_tcp(std::uint16_t port, std::string* err);

/// The port a TCP listener is actually bound to (resolves a port-0 ephemeral choice). 0 on failure.
std::uint16_t bridge_socket_port(socket_t listener);

/// Accept one pending connection from a non-blocking listener. Returns the accepted (non-blocking)
/// socket, or kInvalidSocket with *would_block=true when nothing is pending (not an error), or
/// kInvalidSocket with *would_block=false + *err on a real error.
socket_t bridge_accept(socket_t listener, bool* would_block, std::string* err);

/// Connect (client side) to an AF_UNIX path. POSIX-only. Returns a non-blocking socket or kInvalidSocket.
socket_t bridge_connect_unix(const std::string& path, std::string* err);

/// Connect (client side) to AF_INET `host`:`port` (the crossing; host typically "127.0.0.1").
/// Returns a connected, non-blocking socket or kInvalidSocket (+ sets *err).
socket_t bridge_connect_tcp(const std::string& host, std::uint16_t port, std::string* err);

/// The far end of a connected socket as text ("127.0.0.1:51234", "unix"), or empty when the
/// platform will not say. A fact about the socket for an inventory to show; never an identity.
std::string bridge_peer_name(socket_t sock);

/// Close a socket (platform close/closesocket). Safe on kInvalidSocket.
void bridge_close(socket_t sock);

/// Test seam only: write raw bytes to a socket, bypassing BridgeChannel's framing, the one way to
/// forge a malformed frame (queue() always writes an honest length prefix), so the framing tests
/// can prove a lying length or a bogus frame causes no over-read, hang or desync. No production
/// caller.
void bridge_send_raw(socket_t sock, std::string_view bytes);

/// Block in select() until any socket in `socks` is readable, or `timeout_ms` elapses (negative =
/// indefinite). Returns true if at least one is ready, false on timeout. This is the event-driven
/// wait — a socket becomes readable when the FAR side decides, and a closed peer is readable-then-EOF,
/// so the caller never spins or blocks past an event. The single-threaded multiplexer the bridge
/// server runs: wait here, then poll every socket (non-blocking) and dispatch whatever fired. (On
/// POSIX a plain fd such as stdin is a valid socket_t too, so a client can wait on {socket, stdin}.)
bool bridge_wait_readable(const std::vector<socket_t>& socks, int timeout_ms);

} // namespace loom

#endif // ZEN_BRIDGE_CHANNEL_HPP
