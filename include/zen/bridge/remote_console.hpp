// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_REMOTE_CONSOLE_HPP
#define ZEN_BRIDGE_REMOTE_CONSOLE_HPP

// The client side of the bridge: a Console (and LadderHost) implemented over the operator-protocol
// socket instead of a Switchboard&. This IS "a remote console cannot hold a Switchboard& across a
// socket" made concrete — the SAME frontend (ConsoleUi + emit_ui_tree + draw) drives this exactly as
// it drives the in-process ConsoleEngine; only the transport differs (decision #2's unification).
//
// The engine logic runs CLIENT-side: compose runs the SHARED assumption ladder (run_compose_ladder)
// against the schema fetched over the wire (Describe -> Schema, decoded with schema_codec — the IPC
// currency) and the local reply buffer (filled by Delivered frames); the assembled message is
// serialized and shipped as a Send frame, which the host re-admits + stamps. Discovery and the tap
// are answered/streamed by the host and cached here. The bus stays entirely host-side.

#include <zen/bridge/channel.hpp>
#include <zen/console/console.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

class RemoteConsole : public Console, public LadderHost {
public:
    /// Take ownership of a connected socket (from bridge_connect_*) and perform the Hello/Welcome
    /// handshake (blocking up to handshake_timeout_ms). connected() reports whether it completed.
    explicit RemoteConsole(socket_t sock, int handshake_timeout_ms = 5000);
    ~RemoteConsole() override;

    RemoteConsole(const RemoteConsole&) = delete;
    RemoteConsole& operator=(const RemoteConsole&) = delete;

    bool connected() const noexcept { return connected_; }
    /// The operator's stamped bus id (from Welcome) — the sender the host attributes its sends to.
    loom::WeaveId operator_id() const noexcept { return operator_id_; }
    /// True once the host closed the connection (or it failed): the disconnect-as-an-event signal.
    bool disconnected() const noexcept { return !ch_ || ch_->done(); }
    /// The underlying socket, for a client-side multiplexer that waits on {input, this socket}.
    socket_t socket() const noexcept { return ch_ ? ch_->fd() : kInvalidSocket; }

    // ---- Console (the frontend-facing surface, served over the wire) ----
    std::vector<WeaveInfo> weaves() const override;
    std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const override;
    Composed compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::vector<Arg>& args) override;
    std::size_t buffer_size() const override;
    std::optional<BufferEntry> buffer_at(std::size_t label_number) const override;
    std::vector<TapEvent> tap() const override;
    Dirty take_dirty() override;
    Evicted evicted() const override;
    void pump() override; ///< flush + poll the socket and process every pushed frame (non-blocking)

    // ---- LadderHost (the ladder runs client-side) ----
    std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                       std::uint32_t version) const override;
    std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error) const override;
    loom::Ticket assemble_and_send(loom::WeaveId target,
                                   const std::shared_ptr<const loom::Schema>& schema,
                                   const std::map<std::string, loom::Cell>& cells) override;

    /// The most Delivered frames whose schema is not yet known that the client will hold at once. Same
    /// principle as the transport's kMaxBacklog: bound what a peer can make you hold — even a
    /// more-trusted peer (a host). Past it, a Delivered is dropped and surfaced, never silently kept.
    ///
    /// This is an ACTIVE BACKLOG, not history: each entry is a reply still owed a schema, and it
    /// leaves by being admitted (Schema) or refused aloud (SchemaNone / overflow). That is why it is
    /// bounded by refusal rather than by eviction — dropping the oldest here would discard an
    /// obligation, which the bounded history windows beside it never do.
    static constexpr std::size_t kMaxPendingDelivered = 64;

    /// The most "the host has no such schema" answers the client remembers. A pure memo: its only
    /// job is to spare a repeated Describe round-trip and to let a blocked fetch give up. A host
    /// can drive it — every Delivered naming a novel unknown shape adds one — so it cannot be a
    /// set that only ever grows. Evicting the oldest costs at most one extra Describe, and is the
    /// only bound here that also fixes a staleness: a shape the host registers later is no longer
    /// remembered as absent forever.
    static constexpr std::size_t kMaxAbsentSchemas = 64;

private:
    /// The R2F-C observation instrument, applied to the client's own caches: the absent-schema memo
    /// has no operator-visible surface (unlike the tap and the buffer, whose eviction the operator
    /// must see), so its bound is stated as an assertion about client state rather than inferred
    /// from process memory. Adds no member and no code path.
    friend struct RemoteConsoleStorageProbe;

    void pump_once() const;                      // flush + poll + process (mutates the caches)
    void process(const BridgeIncoming& f) const; // one pushed frame -> a cache update
    void push_bridge_refused(const std::string& reason) const; // a "BridgeRefused" tap line
    bool await(const std::function<bool()>& done, int timeout_ms) const;
    std::shared_ptr<const loom::Schema> fetch_schema(std::string_view name,
                                                     std::uint32_t version) const;
    bool known_absent(std::string_view name, std::uint32_t version) const;
    void remember_absent(std::string name, std::uint32_t version) const;

    // The wire + caches are an implementation detail of the "console" abstraction (its observable
    // state is the weaves/buffer/tap), so they are mutable: a const Console read may fetch-on-demand.
    mutable std::unique_ptr<BridgeChannel> ch_;
    mutable loom::Registry registry_;     ///< decoded reply/compose schemas (filled by Schema frames)
    mutable std::vector<WeaveInfo> weaves_; ///< the live weave set — REPLACED wholesale by each
                                            ///< Weaves frame, so it is bounded by who is on the
                                            ///< bus now, never by how many frames have arrived
    /// m1, m2, ... (filled by Delivered frames) — a bounded HISTORY window, the client-side mirror
    /// of ConsoleWeave::received_. Same capacity, same stable-label rule.
    mutable BoundedHistory<loom::Value, kConsoleBufferCapacity> buffer_;
    /// The bus tap (filled by Tap frames, plus this client's own BridgeRefused lines) — a bounded
    /// HISTORY window, the client-side mirror of ConsoleEngine::tap_.
    mutable BoundedHistory<TapEvent, kConsoleTapCapacity> tap_;
    mutable Dirty dirty_;
    /// Describe -> SchemaNone answers, oldest evicted past kMaxAbsentSchemas. A deque, not a set:
    /// the bound needs insertion order, and at this size a linear probe is cheaper than a tree.
    mutable std::deque<std::pair<std::string, std::uint32_t>> schema_absent_;
    mutable std::vector<std::string> pending_delivered_; ///< Delivered bytes awaiting their schema
    mutable loom::WeaveId operator_id_{}; ///< set by Welcome (processed in a const pump)
    mutable bool connected_ = false;      ///< set by Welcome (processed in a const pump)

    std::uint64_t correlation_ = 0; ///< bumped only in assemble_and_send (non-const)
};

} // namespace loom

#endif // ZEN_BRIDGE_REMOTE_CONSOLE_HPP
