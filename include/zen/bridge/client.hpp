// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_CLIENT_HPP
#define ZEN_BRIDGE_CLIENT_HPP

// THE CONNECTING SIDE OF THE CROSSING, WITH NO CONSOLE IN IT.
//
// `RemoteConsole` is a `Console`: it carries the operator's assumption ladder, a reply buffer with
// labels and a tap window, and it links the console engine to do so -- which is exactly why it
// cannot be exported. A host that LINKS to another host needs none of that. It needs to connect,
// say who it claims to be, learn whether it was admitted and as whom, send bytes to a target or
// an office under a correlation, and be handed every frame the far host ships -- typed enough to
// act on, and never decoded here, because the shapes belong to the participant that asked.
//
// So this is a socket, a handshake and a frame reader, and nothing else: no ladder, no buffer,
// no window, no registry. Everything it hands out is a value the caller owns; nothing blocks
// past `connect`'s bounded handshake.

#include <zen/bridge/channel.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// One frame the far host shipped, decoded to its fields. Which fields are meaningful depends
/// on `kind`; the rest are left at their defaults.
struct BridgeEvent {
    enum class Kind {
        Welcome,     ///< admitted: `session` and `established_name`
        Denied,      ///< refused: `reason`; the connection is over
        Delivered,   ///< a delivery to this session: `sender`, `correlation`, `answers_ask`,
                     ///< `dispatch_refused`, `authored_role`, `payload` (serialized value bytes)
        SendRefused, ///< a send dropped before the far bus: `correlation`, `reason`
        Settled,     ///< what a settle-requested send set in motion on the far bus has all been
                     ///< dispatched: `correlation`. Not an answer; see protocol.hpp
        Weaves,      ///< the far weave set: `weaves`
        Schema,      ///< an encoded schema: `payload`
        SchemaNone,  ///< no such far schema: `shape`, `version`
        Tap,         ///< a copied far bus event (only for an observing session): `tap_kind`,
                     ///< `target`, `sender`, `shape`, `version`, `reason`
        Disconnected ///< the socket is done; emitted once, last
    };
    struct WeaveEntry {
        std::uint64_t id = 0;
        std::vector<std::pair<std::string, std::uint32_t>> accepts;
    };

    Kind kind = Kind::Disconnected;
    std::uint64_t session = 0;
    std::string established_name;
    std::uint64_t sender = 0;
    std::uint64_t target = 0;
    std::uint64_t correlation = 0;
    bool answers_ask = false;
    bool dispatch_refused = false;
    std::string authored_role;
    std::string payload;
    std::string reason;
    std::string shape;
    std::uint32_t version = 0;
    std::uint8_t tap_kind = 0;
    std::vector<WeaveEntry> weaves;
};

class BridgeClient {
public:
    /// Take ownership of a connected socket (from bridge_connect_*). Nothing is said until
    /// `hello`.
    explicit BridgeClient(socket_t sock);
    ~BridgeClient();
    BridgeClient(const BridgeClient&) = delete;
    BridgeClient& operator=(const BridgeClient&) = delete;

    /// Say who this side claims to be, and present a credential. Queued and flushed; the answer
    /// (Welcome or Denied) arrives through `poll`. Returns false when the socket is already done.
    bool hello(std::string_view claimed_name, std::string_view credential);

    /// Block (bounded) until Welcome, Denied or disconnect. Returns true only on Welcome; the
    /// frames that arrived meanwhile -- the Denied, an initial Weaves -- are kept for `poll`.
    bool await_admission(int timeout_ms);

    bool admitted() const noexcept { return admitted_; }
    bool denied() const noexcept { return denied_; }
    const std::string& denial() const noexcept { return denial_; }
    std::uint64_t session() const noexcept { return session_; }
    const std::string& established_name() const noexcept { return established_; }
    bool disconnected() const noexcept { return !ch_ || ch_->done(); }
    socket_t socket() const noexcept { return ch_ ? ch_->fd() : kInvalidSocket; }

    /// Queue a Send to a far WeaveId. `payload` is serialized value bytes. The far host stamps
    /// the sender and reply target from this connection; `correlation` is echoed on the answer.
    /// `settle` asks the far host to say `Settled` under `correlation`, once, when everything the
    /// send set in motion on its bus has been dispatched (or `SendRefused` when it cannot follow
    /// one more). QUEUED, NOT WRITTEN: the bytes leave on the next `poll` or `flush`, which is
    /// where a caller that expects an answer must go anyway.
    void send(std::uint64_t target, std::uint64_t correlation, std::string_view payload,
              bool settle = false);
    /// Queue a Send to whoever holds `role` on the far bus at delivery.
    void send_to_role(std::string_view role, std::uint64_t correlation, std::string_view payload,
                      bool settle = false);
    /// Queue a publication on the far bus.
    void publish(std::uint64_t correlation, std::string_view payload);
    /// Ask the far host to describe a shape (answered by a Schema or SchemaNone event).
    void describe(std::string_view name, std::uint32_t version);
    /// Ask the far host for its weave set again.
    void list_weaves();

    /// Flush what is queued, read what arrived, and append every complete frame -- decoded -- to
    /// `out`. Non-blocking. A finished socket appends one `Disconnected`, once.
    void poll(std::vector<BridgeEvent>& out);
    /// Flush only.
    void flush();

private:
    void send_frame(std::uint8_t kind, std::uint8_t flags, std::uint64_t target,
                    std::string_view role, std::uint64_t correlation, std::string_view payload);
    static bool decode(const BridgeIncoming& f, BridgeEvent& out);

    std::unique_ptr<BridgeChannel> ch_;
    bool admitted_ = false;
    bool denied_ = false;
    bool said_disconnected_ = false;
    std::uint64_t session_ = 0;
    std::string established_;
    std::string denial_;
    std::vector<BridgeEvent> held_; ///< frames `await_admission` read, kept for `poll`
};

} // namespace loom

#endif // ZEN_BRIDGE_CLIENT_HPP
