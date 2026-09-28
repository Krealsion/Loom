// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A net-client "mod", woven with WeaveBase and mounted on the floor, that reaches the network
// ONLY through the NetworkBroker: a recorded `net` delta gives it the net role's send rule,
// never os_cap::Network, so it stays OS-network-denied. On DoNet it first attempts a DIRECT
// connect (the negative control, failing at the syscall) and carries that errno THROUGH the
// broker's echo, so one round trip proves "powerless directly" and "useful via the broker".

#include "net_protocol.hpp"

#include <zen/weave/weave.hpp>
#include <zen/kernel/export.hpp>

#include <cerrno>
#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace loom;
using namespace net;

namespace {

std::int64_t direct_connect_errno(const std::string& host, std::int64_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return errno != 0 ? errno : -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const std::int64_t code = rc == 0 ? 0 : (errno != 0 ? errno : -1);
    ::close(fd);
    return code;
}

struct ClientState {
    std::int64_t replies = 0;
    ZEN_SHAPE(ClientState, 1, ZEN_FIELD(replies));
};

class NetClient : public WeaveBase<NetClient, ClientState, Accept<DoNet, NetResponse>,
                                   Emit<NetRequest>> {
public:
    // A net-needing mod declares it wants network + the net role. Advice only: without a
    // recorded delta it stays on the floor — OS-network-denied, no net role-rule.
    ZEN_ASK(.network = true, .filesystem = "", .roles = {"net"});

    void on(const DoNet& m, Mail& mail) {
        // Negative control: a DIRECT connect must fail (no-interface netns). Carry the
        // errno through the broker's echo so the round-trip witnesses both halves.
        const std::int64_t code = direct_connect_errno(m.host, m.port);
        const std::string s = std::to_string(code);
        mail.send_to_role("net", NetRequest{m.host, m.port, loom::Bytes(s.begin(), s.end())});
    }
    void on(const NetResponse&, Mail&) { ++state_.replies; }
};

} // namespace

ZEN_EXPORT_WEAVE(NetClient)
