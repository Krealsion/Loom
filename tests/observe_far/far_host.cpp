// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE OBSERVATION JOURNEY'S FAR HOST: a second real process, with its own bus, a bridge server
// that admits two named guests, an observation relay whose policy admits exactly one of them to
// exactly two shapes, and a producer it can be told to drive (probe_protocol.hpp). The session
// host links to it; tests/session/observe_journey.py drives both through real runs.
//
//   zen-observe-far-host --port-file <file> [--port <n>]
//
// It writes the port it listens on to the file once it listens, and runs until told `quit` --
// which ends it abruptly, as a host that dies does: nothing is said to anybody first. Test-only.

#include "probe_protocol.hpp"

#include <zen/bridge/channel.hpp>
#include <zen/bridge/server.hpp>
#include <zen/observe/relay.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace ob = loom::observe;
using namespace zen_tests::observe_probe;

/// What the producer was told to do that only the host can: done after the turn that asked.
struct Request {
    std::string verb;
    loom::WeaveId asker{};
};

struct Host {
    std::vector<Request> requests;
    std::int64_t said = 0; ///< the office's own numbering, carried to a replacing producer
};

struct TickerState {
    std::int64_t heard = 0;
    ZEN_SHAPE(TickerState, 1, ZEN_FIELD(heard));
};

/// THE PRODUCER, in the office `far.ticker`. It knows nothing of observers: it publishes.
class Ticker : public loom::WeaveBase<Ticker, TickerState, loom::Accept<ObserveProbeCommand>,
                                      loom::Emit<ProbeTick, ProbeState>> {
public:
    explicit Ticker(Host* host) : host_(host) {}

    void on(const ObserveProbeCommand& c, loom::Mail& mail) {
        ++state_.heard;
        if (c.verb == "tick" || c.verb == "burst") {
            for (std::int64_t i = 0; i < c.count; ++i) {
                (void)mail.publish(ProbeTick{++host_->said});
            }
            (void)mail.publish(ProbeState{host_->said});
        } else if (c.verb == "foreign" || c.verb == "replace" || c.verb == "revoke" ||
                   c.verb == "quit") {
            host_->requests.push_back(Request{c.verb, mail.sender()});
        } else {
            (void)mail.answer(loom::Refused{"the ticker knows tick, burst, foreign, replace, "
                                            "revoke and quit -- not `" + c.verb + "`"});
            return;
        }
        (void)mail.answer(loom::Ack{});
    }

private:
    Host* host_;
};

/// A participant that does NOT hold the office, publishing the office's shape.
class Forger : public loom::WeaveBase<Forger, TickerState, loom::Accept<ObserveProbeCommand>,
                                      loom::Emit<ProbeTick>> {
public:
    void on(const ObserveProbeCommand&, loom::Mail& mail) {
        ++state_.heard;
        (void)mail.publish(ProbeTick{-1});
    }
};

loom::Grant ticker_grant() {
    loom::Grant g;
    g.allow_to_any(ProbeTick::zen_name, ProbeTick::zen_version);
    g.allow_to_any(ProbeState::zen_name, ProbeState::zen_version);
    g.allow_to_any(loom::Ack::zen_name, loom::Ack::zen_version);
    g.allow_to_any(loom::Refused::zen_name, loom::Refused::zen_version);
    return g;
}

/// What either guest may SAY here: ask the relay, and command the producer. Being answered yes
/// by the relay is its policy's decision, below -- and it is not the same for the two.
loom::Grant guest_grant() {
    loom::Grant g;
    for (const char* shape : {ob::Subscribe::zen_name, ob::Release::zen_name,
                              ob::Acknowledge::zen_name, ob::StatusRequested::zen_name}) {
        g.allow_to_role(shape, 1, ob::kObserveRole);
    }
    g.allow_to_role(ObserveProbeCommand::zen_name, ObserveProbeCommand::zen_version, kTicker);
    return g;
}

std::string established(const loom::BridgeServer& server, loom::WeaveId session) {
    for (const loom::Connection& c : server.connections()) {
        if (c.session == session && c.state == loom::ConnectionState::Admitted) {
            return c.established_name;
        }
    }
    return {};
}

loom::WeaveId mount_ticker(loom::Switchboard& bus, Host& host) {
    auto t = std::make_unique<Ticker>(&host);
    Ticker* raw = t.get();
    const loom::WeaveId id = bus.register_weave(std::move(t), ticker_grant(), kTicker);
    raw->zen_set_self(id);
    return id;
}

} // namespace

int main(int argc, char** argv) {
    std::string port_file;
    std::uint16_t port = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--port-file") {
            port_file = argv[i + 1];
        } else if (flag == "--port") {
            port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        }
    }
    if (port_file.empty()) {
        std::fprintf(stderr, "usage: zen-observe-far-host --port-file <file> [--port <n>]\n");
        return 2;
    }
    std::string err;
    if (!loom::bridge_net_init(&err)) {
        std::fprintf(stderr, "far host: %s\n", err.c_str());
        return 1;
    }
    const loom::socket_t listener = loom::bridge_listen_tcp(port, &err);
    if (listener == loom::kInvalidSocket) {
        std::fprintf(stderr, "far host: cannot listen: %s\n", err.c_str());
        return 1;
    }

    loom::Switchboard bus;
    Host host;
    const auto admission = [](const loom::ConnectionRequest& r) {
        loom::ConnectionAdmitted a;
        if (r.credential == "agent-cred") {
            a.established_name = "agent";
        } else if (r.credential == "other-cred") {
            a.established_name = "other";
        } else {
            return loom::ConnectionVerdict::refuse("no guest of this far host presents that credential");
        }
        a.grant = guest_grant();
        return loom::ConnectionVerdict::admit(std::move(a));
    };
    loom::BridgeServer server(bus, listener, admission);
    // THE RELAY'S POLICY: 'agent' may observe the ticker's two shapes, and nothing else may.
    const auto policy = [&server](const ob::ObserveRequest& r) {
        const std::string name = established(server, r.subscriber);
        if (name != "agent") {
            return ob::ObserveVerdict::refuse("this far host lets only 'agent' observe; the asker is " +
                                              (name.empty() ? std::string("no guest") : "'" + name + "'"));
        }
        if (r.producer != kTicker) {
            return ob::ObserveVerdict::refuse("only " + std::string(kTicker) + " is observable here");
        }
        for (const ob::ShapeRef& s : r.shapes) {
            const bool ok = (s.name == ProbeTick::zen_name || s.name == ProbeState::zen_name) &&
                            s.version == 1;
            if (!ok) {
                return ob::ObserveVerdict::refuse("not observable here: " + s.name + " v" +
                                                  std::to_string(s.version));
            }
        }
        return ob::ObserveVerdict::allow();
    };
    ob::Relay* relay = ob::mount_relay(bus, policy, [&server](loom::Fence f)
                                                        -> std::optional<ob::FenceOrigin> {
        const auto from = server.settle_origin(f);
        if (!from) {
            return std::nullopt;
        }
        return ob::FenceOrigin{from->session, from->correlation};
    });
    server.on_connection([relay](const loom::Connection& c) {
        if (c.state == loom::ConnectionState::Closed && c.session.valid()) {
            (void)relay->forget(c.session);
        }
    });
    (void)mount_ticker(bus, host);
    auto forger = std::make_unique<Forger>();
    Forger* raw_forger = forger.get();
    loom::Grant fg;
    fg.allow_to_any(ProbeTick::zen_name, ProbeTick::zen_version);
    const loom::WeaveId forger_id = bus.register_weave(std::move(forger), fg);
    raw_forger->zen_set_self(forger_id);

    {
        std::ofstream out(port_file, std::ios::trunc);
        out << loom::bridge_socket_port(listener) << "\n";
    }
    std::printf("far host: listening on 127.0.0.1:%u\n", static_cast<unsigned>(loom::bridge_socket_port(listener)));
    std::fflush(stdout);

    for (;;) {
        server.service();
        (void)bus.pump_pending();
        std::vector<Request> todo;
        todo.swap(host.requests);
        for (const Request& r : todo) {
            if (r.verb == "foreign") {
                (void)bus.send(forger_id, loom::Message(loom::to_value(ObserveProbeCommand{"tick", 1})));
            } else if (r.verb == "replace") {
                // THE OFFICE CHANGES HANDS: a new participant holds it, with a new id, from now on.
                (void)bus.unregister_weave(bus.role_holder(kTicker));
                (void)mount_ticker(bus, host);
            } else if (r.verb == "revoke") {
                (void)relay->revoke(r.asker, "the far host withdrew it");
            } else if (r.verb == "quit") {
                std::printf("far host: quitting abruptly, as a host that dies does\n");
                std::fflush(stdout);
                std::_Exit(0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
