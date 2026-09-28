// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The NetworkBroker: an ecosystem Weave (not host code), shipped as a .so and mounted
// out-of-process at the TCB tier with os_cap::Network (the host netns), FsAccess::None and
// bounded resources, under role "net". Network is binary (no OS-scoped network), so the broker's
// own allow-list is the only thing between a mod and an arbitrary host, and it is kept tiny:
// loopback only. It performs raw TCP on a mod's behalf (no HTTP, TLS or DNS) and replies to the
// stamped sender, scoping by its allow-list, never by a payload-supplied address.

#include "net_protocol.hpp"

#include <zen/weave/weave.hpp>
#include <zen/kernel/export.hpp>

#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace loom;
using namespace net;

namespace {

// The broker's allow-list, the policy-enforcement point the OS cannot back up for a binary
// network grant: loopback only, simple and auditable. Seam: nftables inside the broker's netns.
bool allowed(const std::string& host) { return host == "127.0.0.1"; }

// Raw TCP connect/send/recv (native POSIX — the same calls the net-probe uses). On
// success, `out` holds what the peer returned. No DNS: host must be a dotted-quad IPv4.
bool tcp_exchange(const std::string& host, std::int64_t port, const loom::Bytes& payload,
                  loom::Bytes& out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    if (!payload.empty()) {
        if (::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) < 0) {
            ::close(fd);
            return false;
        }
    }
    std::uint8_t buf[4096];
    const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    ::close(fd);
    if (r < 0) {
        return false;
    }
    out.assign(buf, buf + r);
    return true;
}

struct BrokerState {
    std::int64_t served = 0;
    ZEN_SHAPE(BrokerState, 1, ZEN_FIELD(served));
};

class NetBroker : public WeaveBase<NetBroker, BrokerState, Accept<NetRequest>, Emit<NetResponse>> {
public:
    void on(const NetRequest& m, Mail& mail) {
        ++state_.served;
        if (!allowed(m.host)) {
            // Refused by the allow-list BEFORE any socket call — the broker is the policy
            // enforcement point, not a passthrough. Replies to the stamped sender.
            mail.reply(NetResponse{false, {}});
            return;
        }
        loom::Bytes data;
        const bool ok = tcp_exchange(m.host, m.port, m.payload, data);
        mail.reply(NetResponse{ok, ok ? std::move(data) : loom::Bytes{}});
    }
};

} // namespace

ZEN_EXPORT_WEAVE(NetBroker)
