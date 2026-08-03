// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The host side of the bridge: the connect-authority chokepoint, the per-operator proxy-participant,
// and the single-threaded multiplexer. The send path mirrors the out-of-process weave's Emit path in
// IsolationHost::handle_child_frame exactly — re-admit the operator's bytes through the one gate and
// STAMP the sender from the connection (the proxy's id), never the wire.

#include <zen/bridge/server.hpp>

#include <zen/kernel/schema_codec.hpp> // encode_schema
#include <zen/schema.hpp>
#include <zen/serialize.hpp> // serialize, parse, admit
#include <zen/value.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom {

namespace {

// The proxy's trivial state (it is in-process, never crash-revived). A 1-field count keeps
// register_weave's snapshot-seeding happy — exactly as the in-process console's state does.
std::shared_ptr<const loom::Schema> operator_state_schema() {
    static const auto s =
        loom::SchemaBuilder("zen.OperatorState", 1).field("n", loom::Kind::Int).build();
    return s;
}

} // namespace

// ---- the proxy: a Weave, on the bus, backed by a socket connection ------------------------------
//
// To the Switchboard this is an ordinary Weave — the operator's identity on the bus, and the SENDER
// the bridge stamps onto every send it makes on the operator's behalf. handle() is called when a
// reply is delivered to this operator (reply_to == this proxy's id); it ships the reply's bytes to
// the client as a Delivered frame (which fills the client-side buffer). AcceptMode::AnyRegistered
// (set at registration) lets it receive a reply of any shape, exactly like the in-process console.
class OperatorProxy final : public loom::Weave {
public:
    explicit OperatorProxy(BridgeChannel* ch) : ch_(ch) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {}; // no listed shapes; AnyRegistered widens this at delivery
    }
    void handle(const loom::Message& in, loom::Bus& /*bus*/) override {
        if (ch_ != nullptr) {
            ch_->queue(BridgeOp::Delivered, loom::serialize(in.payload));
        }
    }
    loom::Value snapshot() const override {
        loom::Value v(operator_state_schema());
        v.set("n", loom::Cell::integer(0));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

    /// Sever the channel link before the connection is torn down, so any stray delivery is a no-op.
    void detach() noexcept { ch_ = nullptr; }

private:
    BridgeChannel* ch_;
};

// ---- the chokepoint -----------------------------------------------------------------------------

OperatorGrant authorize_connection(socket_t /*connection*/) {
    // Model A: the WSL host and the connecting side are ONE trust domain the operator controls, so
    // reachability of the bridge socket IS authority. Full grant, always — today.
    //
    // The B/C future-proofing is this function PLUS the one call in accept_new that consumes its
    // result — the `register_weave(..., Grant{}.allow_any(), AnyRegistered)` line. Together they are
    // the whole seam: model B (a bearer token — possession-is-authorization, no identity) makes the
    // *unauthorized-connection forge* sayable, and its pin must prove a connect that FAILS the token
    // registers NO proxy and processes NO frame; model C (per-connection graduated/differential
    // grants) narrows the `allow_any()` per operator — where "Weaver" is born (the first differential,
    // persistent, per-person authority, i.e. the first place authorization needs authentication).
    // Note under model A: operator proxies are hidden from discovery but ARE addressable (their
    // small-integer WeaveIds) — harmless while every operator is the same principal, but a
    // cross-principal channel the identity phase must treat as surface.
    return OperatorGrant{/*authorized=*/true};
}

// ---- BridgeServer -------------------------------------------------------------------------------

BridgeServer::BridgeServer(loom::Switchboard& bus, socket_t listener)
    : bus_(bus), listener_(listener) {
    tap_obs_ = bus_.add_observer([this](const loom::BusEvent& e) { on_tap(e); });
}

BridgeServer::~BridgeServer() {
    bus_.remove_observer(tap_obs_); // stop the callback before our members die
    for (auto& c : conns_) {
        if (c->proxy != nullptr) {
            c->proxy->detach();
        }
        if (c->id.value != 0) {
            std::unique_ptr<loom::Weave> removed = bus_.unregister_weave(c->id);
            removed.reset(); // destroy the proxy now, before its channel
        }
    }
    conns_.clear();
    bridge_close(listener_);
}

void BridgeServer::accept_new() {
    for (;;) {
        bool would_block = false;
        std::string err;
        const socket_t s = bridge_accept(listener_, &would_block, &err);
        if (s == kInvalidSocket) {
            break; // nothing pending (would_block) or a transient error: try again next step
        }
        if (conns_.size() >= kMaxOperatorConnections) {
            // Shed past the cap: accept (so the OS pending queue clears) then close, and count it. A
            // reconnecting fd-hog is contained — greedy, per the threat tier, includes this — and the
            // shedding is observable via declined_count(), never silent.
            bridge_close(s);
            ++declined_;
            continue;
        }
        const OperatorGrant grant = authorize_connection(s);
        if (!grant.authorized) {
            bridge_close(s); // the chokepoint declined (never happens today; sayable under model B)
            continue;
        }
        auto conn = std::make_unique<Conn>();
        conn->ch = std::make_unique<BridgeChannel>(s);
        auto proxy = std::make_unique<OperatorProxy>(conn->ch.get());
        OperatorProxy* raw = proxy.get();
        loom::WeaveId id;
        try {
            // The most-granted participant — broad send + accept-any — but a GRANT, not host root.
            // Its id is the sender the bridge stamps onto the operator's every send.
            id = bus_.register_weave(std::move(proxy), loom::Grant{}.allow_any(),
                                     loom::AcceptMode::AnyRegistered);
        } catch (...) {
            continue; // register refused: drop the connection (conn + its channel die here)
        }
        conn->id = id;
        conn->proxy = raw;
        proxy_ids_.insert(id.value);
        conns_.push_back(std::move(conn));
    }
}

void BridgeServer::push_weaves(Conn& c) {
    // [u32 n]{[u64 id][u32 m]{[bytes name][u32 version]}} — the live weave set, EXCLUDING operator
    // proxies (they are operators' hands on the bus, not send targets — as the in-process console
    // excludes itself from its own weaves() list).
    std::vector<loom::WeaveId> targets;
    for (loom::WeaveId id : bus_.list_weaves()) {
        if (proxy_ids_.count(id.value) == 0) {
            targets.push_back(id);
        }
    }
    std::string body;
    put_u32(body, static_cast<std::uint32_t>(targets.size()));
    for (loom::WeaveId id : targets) {
        put_u64(body, id.value);
        const auto schemas = bus_.accepted_schemas(id);
        put_u32(body, static_cast<std::uint32_t>(schemas.size()));
        for (const auto& s : schemas) {
            put_bytes(body, s->name());
            put_u32(body, s->version());
        }
    }
    c.ch->queue(BridgeOp::Weaves, body);
}

void BridgeServer::send_refused(Conn& c, std::uint64_t correlation, const std::string& reason) {
    // A Send dropped BEFORE the bus (no tap event exists for it) — surface its fate. Per-frame and
    // non-fatal: the connection stays alive; only a pre-Hello violation severs.
    std::string body;
    put_u64(body, correlation);
    put_bytes(body, reason);
    c.ch->queue(BridgeOp::SendRefused, body);
}

void BridgeServer::on_frame(Conn& c, const BridgeIncoming& f) {
    // The handshake is load-bearing: a frame before Hello completes is broken or hostile — sever
    // (mark the channel failed; the existing done()/reap_dead path tears it down). Anti-Postel.
    if (!c.handshook && f.op != BridgeOp::Hello) {
        c.ch->fail();
        return;
    }
    switch (f.op) {
    case BridgeOp::Hello: {
        Cursor cur(f.payload);
        std::uint32_t proto = 0;
        (void)cur.u32(proto); // version negotiation is a future concern; today we just acknowledge
        c.handshook = true;
        std::string welcome;
        put_u64(welcome, c.id.value); // the operator's stamped bus id
        put_u32(welcome, kBridgeProtocolVersion);
        c.ch->queue(BridgeOp::Welcome, welcome);
        push_weaves(c); // initial discovery, pushed so the client's weave list starts populated
        break;
    }
    case BridgeOp::ListWeaves:
        push_weaves(c);
        break;
    case BridgeOp::Describe: {
        Cursor cur(f.payload);
        std::string_view name;
        std::uint32_t version = 0;
        if (!cur.bytes(name) || !cur.u32(version)) {
            break; // malformed -> drop
        }
        std::shared_ptr<const loom::Schema> schema = bus_.resolve_schema(name, version);
        if (!schema) {
            std::string body;
            put_bytes(body, name);
            put_u32(body, version);
            c.ch->queue(BridgeOp::SchemaNone, body);
        } else {
            // Ship the schema AS BYTES (the IPC currency): encode it to its descriptor Value and
            // serialize. The client re-admits + reconstructs it (decode_schema), exactly as the
            // isolation host reconstructs a child's schemas from its manifest.
            c.ch->queue(BridgeOp::Schema, loom::serialize(loom::encode_schema(*schema)));
        }
        break;
    }
    case BridgeOp::Send: {
        // [u8 kind][u64 wire_sender][u64 target][u64 wire_reply_to][u64 correlation][bytes payload]
        Cursor cur(f.payload);
        std::uint8_t kind = 0;
        std::uint64_t wire_sender = 0;
        std::uint64_t target = 0;
        std::uint64_t wire_reply_to = 0;
        std::uint64_t correlation = 0;
        if (!cur.u8(kind) || !cur.u64(wire_sender) || !cur.u64(target) || !cur.u64(wire_reply_to) ||
            !cur.u64(correlation)) {
            // The header didn't parse far enough to yield a correlation -> 0. No dark drop.
            send_refused(c, 0, "malformed Send header");
            break;
        }
        const std::string_view payload = cur.rest();

        // Re-admit the operator's output through the ONE gate, host-side, exactly as the kernel does
        // for a loaded library's emitted message and the isolation host does for a child's Emit. Each
        // of the three drop paths emits SendRefused (per-frame, non-fatal) so no fate is dark.
        loom::Unverified u = loom::parse(payload);
        std::shared_ptr<const loom::Schema> door =
            bus_.resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            send_refused(c, correlation,
                         "unknown schema: " + u.claimed_name() + " v" +
                             std::to_string(u.claimed_version()));
            break; // a schema the system does not know -> cannot be gated
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            send_refused(c, correlation, "gate refused: " + a.first_error().message());
            break; // gate-refused (malformed/hostile operator output)
        }
        // STAMP the sender + reply_to from the CONNECTION (c.id), NEVER the wire. An honest client
        // sets wire_sender/wire_reply_to to 0; a malicious one forges them, and the bridge stamps
        // over both — provenance stays honest even for the most-granted participant. reply_to is the
        // operator itself, so its replies route back to it (and a forged reply_to cannot redirect
        // them — the confused-deputy guard, the same posture as EmitRole).
        (void)wire_sender;
        (void)wire_reply_to;
        loom::Message msg(std::move(a).value(), loom::WeaveId{}, c.id, correlation);
        if (kind == kEmitPublish) {
            (void)bus_.publish_as(c.id, std::move(msg));
        } else {
            (void)bus_.send_as(c.id, loom::WeaveId{target}, std::move(msg));
        }
        break;
    }
    // host->client opcodes and unknowns: a client does not send these; ignore them inbound.
    case BridgeOp::Welcome:
    case BridgeOp::Weaves:
    case BridgeOp::Schema:
    case BridgeOp::SchemaNone:
    case BridgeOp::Delivered:
    case BridgeOp::Tap:
    case BridgeOp::SendRefused:
        break;
    }
}

void BridgeServer::on_tap(const loom::BusEvent& e) {
    std::uint8_t kind = kTapDelivered;
    switch (e.kind) {
    case loom::EventKind::Delivered:
        kind = kTapDelivered;
        break;
    case loom::EventKind::Refused:
        kind = kTapRefused;
        break;
    case loom::EventKind::Died:
        kind = kTapDied;
        break;
    case loom::EventKind::Revived:
        kind = kTapRevived;
        break;
    }
    std::string body;
    put_u8(body, kind);
    put_u64(body, e.target.value);
    put_u64(body, e.sender.value);
    put_bytes(body, e.schema_name);
    put_u32(body, e.schema_version);
    const std::string refusal =
        e.kind == loom::EventKind::Refused ? std::string(loom::name_of(e.refusal.reason)) : "";
    put_bytes(body, refusal);

    // Every operator sees the whole-bus tap (each is the most-granted participant; one trust domain).
    // This is COPY-ONLY (event fields already in `e`) — safe from inside a pump() observer callback,
    // exactly as the in-process record_tap is.
    for (auto& c : conns_) {
        c->ch->queue(BridgeOp::Tap, body);
    }
    // A Died/Revived changes who is on the bus -> the weave list needs a refresh. But push_weaves
    // READS the bus registry (list_weaves/accepted_schemas), and reading the registry from inside an
    // observer callback mid-pump() is NOT a stated Switchboard guarantee (record_tap deliberately
    // only copies). So DEFER: flag it here, push after pump() returns (step()), where the reads are
    // plainly outside dispatch. The bridge does not lean on an unstated bus property.
    if (e.kind == loom::EventKind::Died || e.kind == loom::EventKind::Revived) {
        weaves_dirty_ = true;
    }
}

void BridgeServer::reap_dead() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        Conn& c = **it;
        if (c.ch && c.ch->done()) {
            // Unregister the proxy FIRST (no further delivery lands on it), destroy it before its
            // channel, then drop the connection. Disconnect handled as an event — not a hang.
            if (c.proxy != nullptr) {
                c.proxy->detach();
            }
            std::unique_ptr<loom::Weave> removed = bus_.unregister_weave(c.id);
            removed.reset();
            proxy_ids_.erase(c.id.value);
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void BridgeServer::step() {
    accept_new();
    for (auto& c : conns_) {
        if (!c->ch) {
            continue;
        }
        std::vector<BridgeIncoming> frames;
        c->ch->poll(frames);
        for (const BridgeIncoming& f : frames) {
            on_frame(*c, f);
            if (c->ch->done()) {
                break; // a pre-Hello severance (B) failed this channel — stop processing its batch
            }
        }
    }
    bus_.pump(); // proxies fire-and-continue (ship Delivered); the tap observer streams Tap
    // Drain the deferred weave-list refresh (E): the registry reads happen HERE, outside dispatch.
    if (weaves_dirty_) {
        for (auto& c : conns_) {
            push_weaves(*c);
        }
        weaves_dirty_ = false;
    }
    for (auto& c : conns_) {
        c->ch->flush();
    }
    reap_dead();
}

void BridgeServer::wait_and_step(int timeout_ms) {
    std::vector<socket_t> socks;
    socks.reserve(conns_.size() + 1);
    socks.push_back(listener_);
    for (auto& c : conns_) {
        if (c->ch) {
            socks.push_back(c->ch->fd());
        }
    }
    (void)bridge_wait_readable(socks, timeout_ms);
    step();
}

void BridgeServer::run(int tick_ms) {
    stop_ = false;
    while (!stop_) {
        wait_and_step(tick_ms);
    }
}

} // namespace loom
