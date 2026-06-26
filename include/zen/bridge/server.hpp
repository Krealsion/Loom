#ifndef ZEN_BRIDGE_SERVER_HPP
#define ZEN_BRIDGE_SERVER_HPP

// The host side of the remote-operator bridge. A BridgeServer accepts operator connections and, per
// connection, registers a proxy-participant on the bus (the EXACT pattern an out-of-process weave
// uses — a proxy that IS a participant, bridging a socket to the bus, here pointed at an operator)
// and serves the operator-protocol: discovery answered, the tap streamed, and the operator's sends
// re-admitted through the one gate with the sender STAMPED FROM THE CONNECTION (never the wire).
//
// Single-threaded by construction: the multiplexer is the SERVER'S readiness-to-receive-from-many-
// sources (a select over {listener, connection fds}), NOT bus concurrency. Inbound operator sends
// enter through the one gated path and pump() processes them in FIFO order — the bus's FIFO and
// reentrancy guarantees are untouched, and no threads are added. The Switchboard must outlive the
// server.
//
// Honest containment: the security boundary is the REACHABILITY of the bridge socket — a party that
// can reach it holds operator power, exactly as a local operator at the host already does. Securing
// that reachability (don't expose the bridge to untrusted networks) is a DEPLOYMENT responsibility,
// stated here plainly; the bridge does NOT authenticate connectors. Threat tier: abuse, not escape.

#include <zen/bridge/channel.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace loom {

/// The operator's authority on the bus, acquired through ONE chokepoint (authorize_connection).
/// Today: full operator grant, always. The ENTIRE connect-authority future-proofing lives in that
/// one function — when the trigger fires (model B: a bearer capability token; model C: per-connection
/// graduated/differential grants, where "Weaver" is born at the differential-power trigger), ONLY
/// authorize_connection changes and everything downstream wields this grant unchanged. Build the
/// chokepoint, not the token.
struct OperatorGrant {
    bool authorized = true; ///< model A: reachability IS authority — every reachable connection passes
    // (model B adds a verified token; model C adds a scoped capability/role set — deferred to the trigger)
};

/// The connect-authority chokepoint. The WSL host and the connecting side are ONE trust domain the
/// operator controls, so reachability of the bridge socket IS authority — stated honestly, never
/// implied otherwise. Today it returns full grant for every connection. This is the seam (a single
/// function), not the feature.
OperatorGrant authorize_connection(socket_t connection);

class OperatorProxy; // a per-operator Weave on the bus (the proxy-participant); defined in server.cpp

class BridgeServer {
public:
    /// Serve operators arriving on `listener` (a listening socket from bridge_listen_*). The
    /// Switchboard is the bus the operators drive; it must outlive the server.
    BridgeServer(loom::Switchboard& bus, socket_t listener);
    ~BridgeServer();

    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    /// One server iteration (non-blocking): accept pending connections (each through the chokepoint
    /// → a registered proxy-participant), drain + dispatch each connection's inbound frames, pump the
    /// bus (proxies ship replies, the tap observer streams events), flush every connection, then reap
    /// any disconnected connection (eof/failed → unregister its proxy). Compose this with a pure
    /// in-process system's own pump(); the bridge never blocks the bus on a slow/hung operator.
    void step();

    /// Block in select() over {listener, all connection fds} until any is ready OR `timeout_ms`
    /// elapses (negative = indefinite), then step() once. This is the event-driven loop: bytes arrive
    /// when the FAR side decides, replies/tap push unbidden through pump(), and disconnect is an
    /// absence the wait surfaces as a readable-then-EOF socket — none of which a synchronous
    /// block-on-read could represent.
    void wait_and_step(int timeout_ms);

    /// Serve until stop(). Each turn waits with a periodic tick so the bus still drains when no socket
    /// is ready (a freshly-spawned in-process weave's first reply, say).
    void run(int tick_ms = 50);
    void stop() noexcept { stop_ = true; }

    std::size_t connection_count() const noexcept { return conns_.size(); }

private:
    struct Conn {
        std::unique_ptr<BridgeChannel> ch;
        loom::WeaveId id{};             ///< the proxy's bus id == the operator's STAMPED sender
        OperatorProxy* proxy = nullptr; ///< owned by the bus; non-owning here
        bool handshook = false;
    };

    void accept_new();
    void on_frame(Conn& c, const BridgeIncoming& f);
    void push_weaves(Conn& c);
    void on_tap(const loom::BusEvent& e);
    void reap_dead();

    loom::Switchboard& bus_;
    socket_t listener_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::set<std::uint64_t> proxy_ids_; ///< operator proxies are hands on the bus, not send targets
    loom::ObserverId tap_obs_ = 0;
    bool stop_ = false;
};

} // namespace loom

#endif // ZEN_BRIDGE_SERVER_HPP
