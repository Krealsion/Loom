// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_SERVER_HPP
#define ZEN_BRIDGE_SERVER_HPP

// The host side of the crossing. A BridgeServer accepts connections and, for each one its
// host's admission policy admits, registers a proxy participant on the bus under the grant that
// policy chose, then serves it: discovery answered, sends re-admitted through the one gate with
// the sender stamped from the connection (never the wire), and every delivery to the proxy
// shipped back with the facts Loom stamped on it. docs/reference/bridge.md
//
// Reaching the socket is not authority. A connection has no proxy, and acts on nothing, until
// the host's `BridgeAdmission` policy answers its Hello: Admit (a grant, an accept mode, the
// name the host establishes, whether it may observe the bus), Refuse (a reason the peer is told
// before it is severed), or Defer (decided later through `decide()`; its sends are refused
// aloud meanwhile, never queued). The proxy's WeaveId is the session's identity and is never
// reused, so a reconnect is a new session. docs/reference/bridge.md#admission-who-decides-and-when
//
// Single-threaded: `service()` does the I/O and takes no bus turn, so a host whose loop is a
// drain can service the crossing from inside a delivery; `step()` adds one bus turn. A
// misbehaving peer is contained (bounded frames and backlog, a loop that never blocks on it),
// and a grant bounds what a session may say, never what this process's native code may touch.

#include <zen/bridge/channel.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace loom {

/// WHAT A PEER SAID ABOUT ITSELF, and where it said it from. Data for a policy to judge;
/// nothing in it is trusted by the server.
struct ConnectionRequest {
    std::uint64_t connection = 0; ///< the server's own number for this connection
    std::uint32_t protocol = 0;   ///< the version the peer's Hello carried
    std::string claimed_name;     ///< the name the peer gave itself (unverified)
    std::string credential;       ///< whatever the peer presented (a token, a passphrase, nothing)
    std::string peer;             ///< the socket's peer address as text, where the platform reports one
};

/// WHICH OF ZEN'S TWO SERIALIZATIONS A SESSION'S VALUES CROSS IN. Both are Zen's own
/// (`zen/serialize.hpp`) and neither is trusted: a Send's payload is parsed in the session's
/// encoding and then re-admitted through the ONE gate against the shape this bus resolves, with
/// the same decode budget either way; a Delivered payload is the admitted value, serialized in
/// the session's encoding.
///
///   Native  the canonical binary (the default, and what every Loom host speaks to another)
///   Compat  the self-describing JSON envelope (`compat::serialize` / `compat::parse`), for a
///           peer that is not written in C++ and should not have to reproduce the binary's
///           positional body or its schema content ids to take part -- a Python client, say
///
/// THE HOST CHOOSES, PER SESSION, AS PART OF ADMITTING IT, and the frames do not change: this is
/// which bytes ride in a Send's and a Delivered's payload, not a new frame. A session is held to
/// its encoding -- a compat session's native payload is refused before the bus, and a native
/// session's JSON is refused exactly as any malformed payload always was -- so one connection
/// never speaks two formats. Nothing about authority depends on it.
enum class PayloadEncoding : std::uint8_t { Native, Compat };

const char* name_of(PayloadEncoding e) noexcept;

/// What an ADMITTED session is: its grant, its doors, its established name, whether it may watch
/// the bus, and which serialization its values cross in. The grant is the whole of its speech
/// authority -- checked at every send, at the bus, under the proxy's own id -- and is never host
/// root.
struct ConnectionAdmitted {
    Grant grant;
    AcceptMode accept = AcceptMode::AnyRegistered; ///< what the proxy may be TOLD (replies route here)
    std::string established_name;                  ///< the host's word for this session; may be empty
    bool observe = false;                          ///< copy every bus event to this connection (the operator model)
    PayloadEncoding encoding = PayloadEncoding::Native; ///< the serialization its payloads cross in
};

/// The three answers a policy can give.
struct ConnectionVerdict {
    enum class Kind { Admit, Refuse, Defer };
    Kind kind = Kind::Refuse;
    ConnectionAdmitted admitted; ///< meaningful when Admit
    std::string reason; ///< meaningful when Refuse: told to the peer, verbatim

    static ConnectionVerdict admit(ConnectionAdmitted a) {
        ConnectionVerdict v;
        v.kind = Kind::Admit;
        v.admitted = std::move(a);
        return v;
    }
    static ConnectionVerdict refuse(std::string why) {
        ConnectionVerdict v;
        v.kind = Kind::Refuse;
        v.reason = std::move(why);
        return v;
    }
    static ConnectionVerdict defer() {
        ConnectionVerdict v;
        v.kind = Kind::Defer;
        return v;
    }
};

/// THE ADMISSION SEAM: the host's decision over a Hello. Called once per connection, on the
/// server's thread, after the handshake frame parsed and before anything else happens on that
/// socket. A later interactive decision attaches by returning Defer here and calling
/// `BridgeServer::decide` when the answer is known.
using BridgeAdmission = std::function<ConnectionVerdict(const ConnectionRequest&)>;

/// Every connection is the operator: `allow_any()`, accept-any, observing the whole bus, and
/// established under whatever name it claimed. Reaching the socket is authority under this
/// policy, so a listener served with it must not be exposed to an untrusted network. Loom's own
/// consoles and bridge host use it; a host that serves guests writes a policy of its own.
BridgeAdmission operator_admission();

/// Where a connection stands. Exactly one of these at a time, and the order is the lifecycle.
enum class ConnectionState : std::uint8_t {
    AwaitingHello,    ///< accepted; no frame yet
    AwaitingDecision, ///< Hello parsed; the policy deferred; nothing it sends can act
    Admitted,         ///< a proxy is on the bus under the policy's grant
    Refused,          ///< Denied was queued; severed once it is flushed
    Closed,           ///< the socket is done; reaped at the next service
};

const char* name_of(ConnectionState s) noexcept;

/// ONE CONNECTION AS THE HOST SEES IT -- the inventory a presentation lists. The claimed name
/// is the peer's word; the established name is the policy's; the session is the proxy's WeaveId
/// (invalid until admitted) and is the only one of the three that is an identity on this bus.
struct Connection {
    std::uint64_t connection = 0;
    ConnectionState state = ConnectionState::AwaitingHello;
    std::string claimed_name;
    std::string established_name;
    std::string peer;
    WeaveId session{};
    bool observes = false;
    PayloadEncoding encoding = PayloadEncoding::Native; ///< as admitted; Native until then
    std::string refusal; ///< why, when Refused
};

class OperatorProxy; // a per-connection Weave on the bus (the proxy-participant); defined in server.cpp

class BridgeServer {
public:
    /// Serve connections arriving on `listener` (a listening socket from bridge_listen_*), each
    /// admitted or refused by `admission`. The Switchboard must outlive the server.
    BridgeServer(loom::Switchboard& bus, socket_t listener, BridgeAdmission admission);
    /// The operator model (`operator_admission()`).
    BridgeServer(loom::Switchboard& bus, socket_t listener)
        : BridgeServer(bus, listener, operator_admission()) {}
    ~BridgeServer();

    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    /// THE I/O HALF, AND ONLY THAT: accept pending connections, read and dispatch every complete
    /// inbound frame (a Send is one gated `send_as` under the proxy's grant -- fenced when it asks
    /// to be told its settlement; a Hello is one admission decision), push a deferred discovery
    /// refresh, tell each connection the settlements it is owed, flush every connection, then
    /// reap any that finished. It takes NO bus turn, so it may be called from inside a delivery by
    /// a host whose loop is a drain -- the crossing is then serviced on that host's own beat.
    void service();

    /// One server iteration: the I/O of `service()` with one bus turn after the frames are
    /// dispatched. The turn drains to idle unless `set_bounded_dispatch()` was called.
    void step();

    /// Make `step()`'s bus turn dispatch only the backlog that existed when it began
    /// (`Switchboard::pump_pending`, MSG-09). Off by default. A host serving a perpetual
    /// in-process service, one that re-arms itself inside its own handler so the queue never
    /// empties, must set it, or `step()` never returns to poll its sockets. It takes no number:
    /// the bound is the turn's snapshot of the queue.
    void set_bounded_dispatch() noexcept { bounded_dispatch_ = true; }
    bool bounded_dispatch() const noexcept { return bounded_dispatch_; }

    /// Block in select() over {listener, all connection fds} until any is ready OR `timeout_ms`
    /// elapses (negative = indefinite), then step() once.
    void wait_and_step(int timeout_ms);

    /// Serve until stop(). Each turn waits with a periodic tick so the bus still drains when no
    /// socket is ready.
    void run(int tick_ms = 50);
    void stop() noexcept { stop_ = true; }

    // ---- the decision seam, after the fact ----------------------------------------------

    /// SETTLE A DEFERRED CONNECTION. Admit registers its proxy under the verdict's grant and sends
    /// Welcome; Refuse sends Denied and severs. Returns false when `connection` is not awaiting a
    /// decision (unknown, already decided, or gone) -- a late decision about a session that no
    /// longer exists changes nothing. Defer is not a decision and is refused the same way.
    bool decide(std::uint64_t connection, const ConnectionVerdict& verdict);

    /// Sever an admitted or waiting connection now: its proxy leaves the bus at the next service,
    /// so no further delivery can land on it. Returns false when there is no such connection.
    bool disconnect(std::uint64_t connection);

    // ---- which session set a delivery in motion ------------------------------------------

    /// The session that opened a fence (its proxy's id, the sender the bus stamps on its sends)
    /// and the correlation it put on that settle-requested Send.
    struct SettleOrigin {
        loom::WeaveId session{};
        std::uint64_t correlation = 0;
    };

    /// WHO OPENED `fence`, if a session of this server did and the server still holds it -- from
    /// the Send reaching the bus until its settlement has been told, which is the whole time
    /// anything that Send set in motion can be dispatched. Empty for every other fence. Read-only:
    /// an observation relay (zen/observe/relay.hpp) uses it to tell a session which of ITS OWN
    /// sends set a publication in motion, and must never tell one session another's.
    std::optional<SettleOrigin> settle_origin(loom::Fence fence) const;

    // ---- the inventory ---------------------------------------------------------------------

    /// Every live connection, in accept order, as the host sees it right now.
    std::vector<Connection> connections() const;
    std::size_t connection_count() const noexcept { return conns_.size(); }
    /// How many connections were shed for exceeding the cap (accepted-then-closed). A reconnecting
    /// fd-hog is contained (greedy is in the threat tier) and its shedding is observable, not silent.
    std::size_t declined_count() const noexcept { return declined_; }
    /// How many connections the policy refused (Denied), all time.
    std::size_t refused_count() const noexcept { return refused_; }

    /// Told on every state change of any connection, with the connection as it stands after the
    /// change -- including `Closed`, so a presentation can drop the row rather than show a dead
    /// session as live. Called from inside `service()`/`decide()`; do not step the server from it.
    void on_connection(std::function<void(const Connection&)> tell) { tell_ = std::move(tell); }

    /// The most connections served at once. A stated, pinned bound far below every platform limit.
    static constexpr std::size_t kMaxOperatorConnections = 32;
    /// The most settle-requested sends one connection may have waiting on their settlement. Past
    /// it a settle-requested Send is refused before the bus (SendRefused), never queued unfenced.
    static constexpr std::size_t kMaxSettlingPerConnection = 8;

private:
    struct Conn {
        std::uint64_t number = 0;
        std::unique_ptr<BridgeChannel> ch;
        ConnectionState state = ConnectionState::AwaitingHello;
        std::string claimed;
        std::string established;
        std::string peer;
        std::string refusal;
        std::uint32_t protocol = 0;
        bool observes = false;
        PayloadEncoding encoding = PayloadEncoding::Native; ///< the admission verdict's choice
        loom::WeaveId id{};             ///< the proxy's bus id == the session's STAMPED sender
        OperatorProxy* proxy = nullptr; ///< owned by the bus; non-owning here
        bool told_closed = false;
        /// Settle-requested sends that reached the bus, each waiting on its fence (bounded by
        /// kMaxSettlingPerConnection). Released when told, and when the connection ends.
        struct Settling {
            std::uint64_t correlation = 0;
            loom::Fence fence{};
        };
        std::vector<Settling> settling;
    };

    void accept_new();
    void on_frame(Conn& c, const BridgeIncoming& f);
    void on_hello(Conn& c, const BridgeIncoming& f);
    void apply(Conn& c, const ConnectionVerdict& verdict);
    void admit(Conn& c, const ConnectionAdmitted& a);
    void refuse(Conn& c, const std::string& why);
    void send_refused(Conn& c, std::uint64_t correlation, const std::string& reason);
    void push_weaves(Conn& c);
    void on_tap(const loom::BusEvent& e);
    /// Tell each connection, once, every settle-requested send whose fence has settled.
    void report_settled();
    void reap_dead();
    void tell(const Conn& c);
    Connection view(const Conn& c) const;
    Conn* find(std::uint64_t number);

    loom::Switchboard& bus_;
    socket_t listener_;
    BridgeAdmission admission_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::set<std::uint64_t> proxy_ids_; ///< proxies are hands on the bus, not send targets
    loom::ObserverId tap_obs_ = 0;
    std::function<void(const Connection&)> tell_;
    std::uint64_t next_number_ = 1;
    std::size_t declined_ = 0;  ///< connections shed for the cap
    std::size_t refused_ = 0;   ///< connections the policy refused
    bool weaves_dirty_ = false; ///< a Died/Revived seen in the tap; push a fresh Weaves after pump
    bool stop_ = false;
    bool bounded_dispatch_ = false; ///< false: the turn drains to empty
};

} // namespace loom

#endif // ZEN_BRIDGE_SERVER_HPP
