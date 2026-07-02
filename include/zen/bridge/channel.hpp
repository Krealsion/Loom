#ifndef ZEN_BRIDGE_CHANNEL_HPP
#define ZEN_BRIDGE_CHANNEL_HPP

// A portable, non-blocking, length-framed byte channel over a stream socket — the transport the
// remote-operator bridge speaks the operator-protocol over. It is the cross-platform sibling of the
// isolation Channel (same framing, same bounded/non-blocking/EOF-observable discipline), but it
// works on BOTH a POSIX fd and a Winsock SOCKET, because the Windows console client connects to the
// WSL-hosted bus over the real host boundary. The framing logic is shared; only the raw
// recv/send/close/set-nonblocking differ per platform (in channel.cpp).
//
// Transport-agnostic by construction: the same Channel + protocol run over an AF_UNIX socketpair or
// path (the fast WSL<->WSL inner loop) and over AF_INET 127.0.0.1 (the real Windows->WSL crossing —
// WSL2 forwards localhost). A frame over the cap, or an undrained backlog over the cap, marks the
// channel failed (a misbehaving peer is contained, never allowed to block/hang/OOM). EOF (peer
// closed) is observable and signals disconnect — the event a synchronous block-on-read could not
// represent, which is why the bridge's loop is an event-driven multiplexer.

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

/// Listen on an AF_UNIX path (the fast local WSL<->WSL transport). Unlinks a stale path first.
/// Returns a listening, non-blocking socket or kInvalidSocket (+ sets *err). POSIX-only (Windows
/// AF_UNIX interop is unreliable; the crossing uses TCP). The path is removed on close.
socket_t bridge_listen_unix(const std::string& path, std::string* err);

/// Listen on AF_INET 127.0.0.1:`port` (the real Windows<->WSL crossing — WSL2 forwards localhost).
/// port 0 asks the OS to choose; read it back with bridge_socket_port(). Returns a listening,
/// non-blocking socket or kInvalidSocket (+ sets *err).
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

/// Close a socket (platform close/closesocket). Safe on kInvalidSocket.
void bridge_close(socket_t sock);

/// Block in select() until any socket in `socks` is readable, or `timeout_ms` elapses (negative =
/// indefinite). Returns true if at least one is ready, false on timeout. This is the event-driven
/// wait — a socket becomes readable when the FAR side decides, and a closed peer is readable-then-EOF,
/// so the caller never spins or blocks past an event. The single-threaded multiplexer the bridge
/// server runs: wait here, then poll every socket (non-blocking) and dispatch whatever fired. (On
/// POSIX a plain fd such as stdin is a valid socket_t too, so a client can wait on {socket, stdin}.)
bool bridge_wait_readable(const std::vector<socket_t>& socks, int timeout_ms);

} // namespace loom

#endif // ZEN_BRIDGE_CHANNEL_HPP
