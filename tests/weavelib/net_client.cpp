// A net-client "mod": woven with WeaveBase, mounted on the floor. A mod that needs
// network (a multiplayer mod, say) reaches it ONLY through the NetworkBroker — never as
// a raw grant. With a recorded `net` delta it holds the net role send-rule (it may *talk
// to* the broker) but NOT os_cap::Network: it stays OS-network-denied (no-interface
// netns) and reaches the network solely via the broker. On a DoNet trigger it first
// attempts a DIRECT connect (the negative control — must fail at the syscall level) and
// carries that errno THROUGH the broker's echo, so one round-trip proves both
// "powerless directly" and "useful via the broker".

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
