// The NetworkBroker: an ecosystem Shard (not host code), shipped as a .so and mounted
// out-of-process at the TCB tier with os_cap::Network (so it runs in the host netns, with
// the real network), FsAccess::None (it needs no disk), bounded resources, registered
// under role "net". It is a HIGHER-TRUST broker than the StorageBroker: network is binary
// — there is no OS-scoped network — so the OS gives it the *whole* host network, and the
// only thing between a mod and an arbitrary host is the broker's own allow-list. That
// validation is therefore kept tiny and obviously-correct (loopback only, v1). It
// performs raw TCP on a mod's behalf (no HTTP/TLS/DNS, no dependency) and replies to the
// stamped sender. It scopes by its allow-list, never a payload-supplied address.

#include "net_protocol.hpp"

#include <zen/author/shard.hpp>
#include <zen/kernel/export.hpp>

#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace zen::author;
using namespace net;

namespace {

// The broker's allow-list — its software scoping, the policy-enforcement point the OS
// cannot back up for a binary network grant. v1: loopback only, simple and auditable.
// (An OS-level tightening — nftables inside the broker's netns — is a named future
// hardening, not built.)
bool allowed(const std::string& host) { return host == "127.0.0.1"; }

// Raw TCP connect/send/recv (native POSIX — the same calls the B3 net-probe uses). On
// success, `out` holds what the peer returned. No DNS: host must be a dotted-quad IPv4.
bool tcp_exchange(const std::string& host, std::int64_t port, const zen::Bytes& payload,
                  zen::Bytes& out) {
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

class NetBroker : public ShardBase<NetBroker, BrokerState, Accept<NetRequest>, Emit<NetResponse>> {
public:
    void on(const NetRequest& m, Mail& mail) {
        ++state_.served;
        if (!allowed(m.host)) {
            // Refused by the allow-list BEFORE any socket call — the broker is the policy
            // enforcement point, not a passthrough. Replies to the stamped sender.
            mail.reply(NetResponse{false, {}});
            return;
        }
        zen::Bytes data;
        const bool ok = tcp_exchange(m.host, m.port, m.payload, data);
        mail.reply(NetResponse{ok, ok ? std::move(data) : zen::Bytes{}});
    }
};

} // namespace

ZEN_EXPORT_SHARD(NetBroker)
