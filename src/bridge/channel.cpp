// The portable bridge transport: BridgeChannel's framing (shared) + the per-platform raw socket I/O
// and the connect/listen/accept helpers. The framing mirrors the proven isolation Channel exactly;
// only recv/send/close/set-nonblocking and the socket setup differ between POSIX and Winsock.

#include <zen/bridge/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
// WSAPoll needs Windows Vista+ (_WIN32_WINNT >= 0x0600); set a floor if the toolchain left it lower
// or unset, BEFORE winsock2.h so the declaration is visible. (MinGW's default varies by version.)
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace loom {

namespace {

constexpr std::size_t kMaxBacklog = 64u * 1024u * 1024u; // a peer that won't drain is contained

// ---- the per-platform raw-I/O seam (the ONLY platform-specific behavior) -----------------------

#ifdef _WIN32

bool last_would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool last_interrupted() { return false; } // Winsock has no EINTR

long raw_send(socket_t s, const char* p, std::size_t n) {
    const int chunk = n > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(n);
    return ::send(static_cast<SOCKET>(s), p, chunk, 0);
}
long raw_recv(socket_t s, char* p, std::size_t n) {
    const int chunk = n > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(n);
    return ::recv(static_cast<SOCKET>(s), p, chunk, 0);
}
void set_nonblocking(socket_t s) {
    u_long mode = 1;
    (void)::ioctlsocket(static_cast<SOCKET>(s), static_cast<long>(FIONBIO), &mode);
}
SOCKET native(socket_t s) { return static_cast<SOCKET>(s); } // the native arg type for socket calls

#else

bool last_would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool last_interrupted() { return errno == EINTR; }

long raw_send(socket_t s, const char* p, std::size_t n) {
    return static_cast<long>(::send(s, p, n, MSG_NOSIGNAL)); // no SIGPIPE on a gone peer
}
long raw_recv(socket_t s, char* p, std::size_t n) {
    return static_cast<long>(::recv(s, p, n, 0));
}
void set_nonblocking(socket_t s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
}
int native(socket_t s) { return s; } // on POSIX a socket IS an int fd

#endif

} // namespace

// ---- BridgeChannel (framing — platform-agnostic over the raw-I/O seam) -------------------------

BridgeChannel::BridgeChannel(socket_t sock) : fd_(sock) { set_nonblocking(fd_); }

BridgeChannel::~BridgeChannel() { bridge_close(fd_); }

void BridgeChannel::queue(BridgeOp op, std::string_view payload) {
    if (failed_) {
        return;
    }
    if (payload.size() > kMaxFrameLen) {
        failed_ = true;
        return;
    }
    put_u32(outbox_, static_cast<std::uint32_t>(payload.size()));
    put_u8(outbox_, static_cast<std::uint8_t>(op));
    outbox_.append(payload);
    if (outbox_.size() - out_pos_ > kMaxBacklog) {
        failed_ = true;
    }
}

void BridgeChannel::flush() {
    if (failed_ || fd_ == kInvalidSocket) {
        return;
    }
    while (out_pos_ < outbox_.size()) {
        const long n = raw_send(fd_, outbox_.data() + out_pos_, outbox_.size() - out_pos_);
        if (n > 0) {
            out_pos_ += static_cast<std::size_t>(n);
        } else if (n < 0 && last_would_block()) {
            break; // kernel buffer full; the rest stays queued for next flush
        } else if (n < 0 && last_interrupted()) {
            continue;
        } else {
            failed_ = true; // EPIPE / ECONNRESET / other: peer gone
            return;
        }
    }
    if (out_pos_ == outbox_.size()) {
        outbox_.clear();
        out_pos_ = 0;
    }
}

void BridgeChannel::poll(std::vector<BridgeIncoming>& out) {
    if (failed_ || fd_ == kInvalidSocket) {
        return;
    }
    char buf[8192];
    for (;;) {
        const long n = raw_recv(fd_, buf, sizeof(buf));
        if (n > 0) {
            inbox_.append(buf, static_cast<std::size_t>(n));
            if (inbox_.size() > kMaxBacklog) {
                failed_ = true;
                return;
            }
        } else if (n == 0) {
            eof_ = true;
            break;
        } else if (last_would_block()) {
            break;
        } else if (last_interrupted()) {
            continue;
        } else {
            failed_ = true;
            return;
        }
    }

    std::size_t pos = 0;
    while (inbox_.size() - pos >= 5) {
        Cursor header(std::string_view(inbox_).substr(pos, 5));
        std::uint32_t len = 0;
        std::uint8_t op = 0;
        (void)header.u32(len);
        (void)header.u8(op);
        if (len > kMaxFrameLen) {
            failed_ = true;
            break;
        }
        if (inbox_.size() - pos < static_cast<std::size_t>(5) + len) {
            break; // frame not fully arrived yet
        }
        out.push_back(BridgeIncoming{static_cast<BridgeOp>(op), inbox_.substr(pos + 5, len)});
        pos += static_cast<std::size_t>(5) + len;
    }
    if (pos > 0) {
        inbox_.erase(0, pos);
    }
}

// ---- socket setup (POSIX + Winsock) ------------------------------------------------------------

bool bridge_net_init(std::string* err) {
#ifdef _WIN32
    WSADATA wsa{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        if (err != nullptr) {
            *err = "WSAStartup failed";
        }
        return false;
    }
#else
    (void)err;
#endif
    return true;
}

void bridge_close(socket_t sock) {
    if (sock == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(sock));
#else
    ::close(sock);
#endif
}

namespace {

void set_err(std::string* err, const char* msg) {
    if (err != nullptr) {
        *err = msg;
    }
}

} // namespace

socket_t bridge_listen_tcp(std::uint16_t port, std::string* err) {
    if (!bridge_net_init(err)) {
        return kInvalidSocket;
    }
    const socket_t s = static_cast<socket_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (s == kInvalidSocket) {
        set_err(err, "socket(AF_INET) failed");
        return kInvalidSocket;
    }
    int yes = 1;
    (void)::setsockopt(native(s), SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 — never expose the bridge off-host
    if (::bind(native(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        set_err(err, "bind(127.0.0.1) failed");
        bridge_close(s);
        return kInvalidSocket;
    }
    if (::listen(native(s), 16) != 0) {
        set_err(err, "listen failed");
        bridge_close(s);
        return kInvalidSocket;
    }
    set_nonblocking(s);
    return s;
}

std::uint16_t bridge_socket_port(socket_t listener) {
    sockaddr_in addr{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(addr));
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(native(listener), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

socket_t bridge_connect_tcp(const std::string& host, std::uint16_t port, std::string* err) {
    if (!bridge_net_init(err)) {
        return kInvalidSocket;
    }
    const socket_t s = static_cast<socket_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (s == kInvalidSocket) {
        set_err(err, "socket(AF_INET) failed");
        return kInvalidSocket;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        set_err(err, "inet_pton: bad host address");
        bridge_close(s);
        return kInvalidSocket;
    }
    // Blocking connect (a one-time client setup); BridgeChannel sets non-blocking afterwards.
    if (::connect(native(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        set_err(err, "connect failed (is the bridge listening on that 127.0.0.1 port?)");
        bridge_close(s);
        return kInvalidSocket;
    }
    return s;
}

#ifndef _WIN32

socket_t bridge_listen_unix(const std::string& path, std::string* err) {
    const socket_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        set_err(err, "socket(AF_UNIX) failed");
        return kInvalidSocket;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        set_err(err, "AF_UNIX path too long");
        bridge_close(s);
        return kInvalidSocket;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    (void)::unlink(path.c_str()); // remove a stale socket file
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        set_err(err, "bind(AF_UNIX) failed");
        bridge_close(s);
        return kInvalidSocket;
    }
    if (::listen(s, 16) != 0) {
        set_err(err, "listen failed");
        bridge_close(s);
        return kInvalidSocket;
    }
    set_nonblocking(s);
    return s;
}

socket_t bridge_connect_unix(const std::string& path, std::string* err) {
    const socket_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        set_err(err, "socket(AF_UNIX) failed");
        return kInvalidSocket;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        set_err(err, "AF_UNIX path too long");
        bridge_close(s);
        return kInvalidSocket;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        set_err(err, "connect(AF_UNIX) failed");
        bridge_close(s);
        return kInvalidSocket;
    }
    return s;
}

#else // _WIN32: AF_UNIX is POSIX-only here (the crossing uses TCP); provide failing stubs.

socket_t bridge_listen_unix(const std::string&, std::string* err) {
    set_err(err, "AF_UNIX listen is POSIX-only (the Windows<->WSL crossing uses TCP)");
    return kInvalidSocket;
}
socket_t bridge_connect_unix(const std::string&, std::string* err) {
    set_err(err, "AF_UNIX connect is POSIX-only (the Windows<->WSL crossing uses TCP)");
    return kInvalidSocket;
}

#endif

bool bridge_wait_readable(const std::vector<socket_t>& socks, int timeout_ms) {
    // poll/WSAPoll, not select: no FD_SETSIZE ceiling, so a reconnecting fd-hog cannot walk the
    // server into undefined behavior (greedy is in the threat tier). timeout is in milliseconds
    // directly (negative = indefinite) — poll's own contract, no timeval dance.
#ifdef _WIN32
    using poll_fd_t = WSAPOLLFD;
#else
    using poll_fd_t = struct pollfd;
#endif
    std::vector<poll_fd_t> fds;
    fds.reserve(socks.size());
    for (socket_t s : socks) {
        if (s == kInvalidSocket) {
            continue;
        }
        poll_fd_t p{};
        p.fd = native(s); // int on POSIX, SOCKET on Winsock — matches each pollfd.fd type
        p.events = POLLIN;
        p.revents = 0;
        fds.push_back(p);
    }
    if (fds.empty()) {
        return false; // nothing to wait on (WSAPoll rejects a zero-length set; the callers never do)
    }
#ifdef _WIN32
    const int n = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
#endif
    return n > 0;
}

socket_t bridge_accept(socket_t listener, bool* would_block, std::string* err) {
    if (would_block != nullptr) {
        *would_block = false;
    }
#ifdef _WIN32
    const SOCKET a = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (a == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            if (would_block != nullptr) {
                *would_block = true;
            }
        } else {
            set_err(err, "accept failed");
        }
        return kInvalidSocket;
    }
    return static_cast<socket_t>(a);
#else
    const int a = ::accept(listener, nullptr, nullptr);
    if (a < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (would_block != nullptr) {
                *would_block = true;
            }
        } else if (errno != EINTR) {
            set_err(err, "accept failed");
        } else if (would_block != nullptr) {
            *would_block = true; // EINTR: treat as "nothing this round", retry next poll
        }
        return kInvalidSocket;
    }
    return a;
#endif
}

} // namespace loom
