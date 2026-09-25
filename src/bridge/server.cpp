// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The host side of the crossing: the admission seam, the per-connection proxy-participant, and
// the single-threaded multiplexer. The send path mirrors the out-of-process weave's Emit path in
// IsolationHost::handle_child_frame exactly -- re-admit the connection's bytes through the one
// gate and STAMP the sender from the connection (the proxy's id), never the wire.

#include <zen/bridge/server.hpp>

#include <zen/kernel/schema_codec.hpp> // encode_schema
#include <zen/schema.hpp>
#include <zen/serialize.hpp> // serialize, parse, admit
#include <zen/value.hpp>
#include <zen/weave/describe.hpp> // describe_answer_schemas
#include <zen/weave/dispatch_refusal.hpp>
#include <zen/weave/shape.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom {

namespace {

// The proxy's trivial state (it is in-process, never crash-revived). A 1-field count keeps
// register_weave's snapshot-seeding happy -- exactly as the in-process console's state does.
std::shared_ptr<const loom::Schema> operator_state_schema() {
    static const auto s =
        loom::SchemaBuilder("zen.OperatorState", 1).field("n", loom::Kind::Int).build();
    return s;
}

} // namespace

const char* name_of(ConnectionState s) noexcept {
    switch (s) {
    case ConnectionState::AwaitingHello:
        return "awaiting-hello";
    case ConnectionState::AwaitingDecision:
        return "awaiting-decision";
    case ConnectionState::Admitted:
        return "admitted";
    case ConnectionState::Refused:
        return "refused";
    case ConnectionState::Closed:
        return "closed";
    }
    return "?";
}

const char* name_of(PayloadEncoding e) noexcept {
    switch (e) {
    case PayloadEncoding::Native:
        return "native";
    case PayloadEncoding::Compat:
        return "compat";
    }
    return "?";
}

namespace {

/// A value, in the serialization the session was admitted to speak.
std::string encode_for(PayloadEncoding e, const loom::Value& v) {
    return e == PayloadEncoding::Compat ? loom::compat::serialize(v) : loom::serialize(v);
}

} // namespace

BridgeAdmission operator_admission() {
    return [](const ConnectionRequest& r) {
        ConnectionAdmitted a;
        a.grant = loom::Grant{}.allow_any();
        a.accept = AcceptMode::AnyRegistered;
        a.established_name = r.claimed_name;
        a.observe = true;
        return ConnectionVerdict::admit(std::move(a));
    };
}

// ---- the proxy: a Weave, on the bus, backed by a socket connection ------------------------------
//
// To the Switchboard this is an ordinary Weave -- the session's identity on the bus, and the SENDER
// the bridge stamps onto every send it makes on the connection's behalf. handle() is called when a
// message is delivered to this proxy (a reply routed to reply_to == this id, an answer, a
// dispatch-refusal notice); it ships the delivery to the peer as a Delivered frame WITH the facts
// the bus stamped on it, because a peer that only got the bytes could not tell an attested answer
// from any admitted participant's helpful `zen.Result`.
class OperatorProxy final : public loom::Weave {
public:
    /// `slot` is the connection's non-owning pointer back to this proxy, cleared when the proxy
    /// dies -- which the BUS decides, not the server: at teardown the bus destroys its weaves in
    /// registry order, so a proxy may be gone before the weave that owns the server is, and the
    /// server's own destructor must find a null rather than a freed object.
    OperatorProxy(BridgeChannel* ch, OperatorProxy** slot, PayloadEncoding encoding)
        : ch_(ch), slot_(slot), encoding_(encoding) {}
    ~OperatorProxy() override {
        if (slot_ != nullptr && *slot_ == this) {
            *slot_ = nullptr;
        }
    }

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        // THE LISTED SHAPES, beside whatever the admission's accept mode adds at delivery: the
        // dispatch-refusal notice (MSG-12) is sent only to a sender whose DECLARED doors name
        // it, so a session that could not hear its own refusals would be a session whose
        // sends vanished; and the construction layer's answer to `zen.DescribeAccepted`, which
        // no ordinary participant declares, so a session allowed to ask what a weave accepts
        // has somewhere for the answer to land. Listing them also registers the shapes, so
        // they resolve on a bus where no other participant declared them.
        std::vector<std::shared_ptr<const Schema>> doors{schema_of<DispatchRefused>()};
        for (const auto& s : describe_answer_schemas()) {
            doors.push_back(s);
        }
        return doors;
    }
    void handle(const loom::Message& in, loom::Bus& /*bus*/) override {
        if (ch_ == nullptr) {
            return;
        }
        std::string body;
        put_u64(body, in.sender.value);
        put_u64(body, in.correlation);
        std::uint8_t flags = 0;
        if (in.provenance.answers_ask()) {
            flags |= kDeliveredAnswersAsk;
        }
        if (in.provenance.dispatch_refused()) {
            flags |= kDeliveredDispatchRefused;
        }
        put_u8(body, flags);
        put_bytes(body, in.provenance.authored_role());
        // The admitted value, in the serialization this session was admitted to speak. Both
        // are total for a conforming value, and what the bus delivered has already conformed.
        body.append(encode_for(encoding_, in.payload));
        ch_->queue(BridgeOp::Delivered, body);
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
    void detach() noexcept {
        ch_ = nullptr;
        slot_ = nullptr; // the connection is letting go first; nothing to clear later
    }

private:
    BridgeChannel* ch_;
    OperatorProxy** slot_;
    PayloadEncoding encoding_;
};

// ---- BridgeServer -------------------------------------------------------------------------------

BridgeServer::BridgeServer(loom::Switchboard& bus, socket_t listener, BridgeAdmission admission)
    : bus_(bus), listener_(listener), admission_(std::move(admission)) {
    if (!admission_) {
        admission_ = operator_admission();
    }
    tap_obs_ = bus_.add_observer([this](const loom::BusEvent& e) { on_tap(e); });
}

BridgeServer::~BridgeServer() {
    bus_.remove_observer(tap_obs_); // stop the callback before our members die
    for (auto& c : conns_) {
        for (const Conn::Settling& s : c->settling) {
            bus_.release_fence(s.fence);
        }
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

Connection BridgeServer::view(const Conn& c) const {
    Connection v;
    v.connection = c.number;
    v.state = c.ch && c.ch->done() && c.state != ConnectionState::Refused ? ConnectionState::Closed
                                                                            : c.state;
    if (c.state == ConnectionState::Refused && c.ch && c.ch->done()) {
        v.state = ConnectionState::Closed;
    }
    v.claimed_name = c.claimed;
    v.established_name = c.established;
    v.peer = c.peer;
    v.session = c.id;
    v.observes = c.observes;
    v.encoding = c.encoding;
    v.refusal = c.refusal;
    return v;
}

void BridgeServer::tell(const Conn& c) {
    if (tell_) {
        tell_(view(c));
    }
}

BridgeServer::Conn* BridgeServer::find(std::uint64_t number) {
    for (auto& c : conns_) {
        if (c->number == number) {
            return c.get();
        }
    }
    return nullptr;
}

std::vector<Connection> BridgeServer::connections() const {
    std::vector<Connection> out;
    out.reserve(conns_.size());
    for (const auto& c : conns_) {
        out.push_back(view(*c));
    }
    return out;
}

void BridgeServer::accept_new() {
    for (;;) {
        bool would_block = false;
        std::string err;
        const socket_t s = bridge_accept(listener_, &would_block, &err);
        if (s == kInvalidSocket) {
            break; // nothing pending (would_block) or a transient error: try again next service
        }
        if (conns_.size() >= kMaxOperatorConnections) {
            // Shed past the cap: accept (so the OS pending queue clears) then close, and count it.
            bridge_close(s);
            ++declined_;
            continue;
        }
        auto conn = std::make_unique<Conn>();
        conn->number = next_number_++;
        conn->peer = bridge_peer_name(s);
        conn->ch = std::make_unique<BridgeChannel>(s);
        conn->state = ConnectionState::AwaitingHello;
        tell(*conn);
        conns_.push_back(std::move(conn));
    }
}

void BridgeServer::push_weaves(Conn& c) {
    // [u32 n]{[u64 id][u32 m]{[bytes name][u32 version]}} -- the live weave set, EXCLUDING proxies
    // (they are sessions' hands on the bus, not send targets -- as the in-process console excludes
    // itself from its own weaves() list).
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
    // A Send dropped BEFORE the bus (no tap event exists for it) -- surface its fate. Per-frame and
    // non-fatal: the connection stays alive; only a protocol violation severs.
    std::string body;
    put_u64(body, correlation);
    put_bytes(body, reason);
    c.ch->queue(BridgeOp::SendRefused, body);
}

void BridgeServer::admit(Conn& c, const ConnectionAdmitted& a) {
    auto proxy = std::make_unique<OperatorProxy>(c.ch.get(), &c.proxy, a.encoding);
    OperatorProxy* raw = proxy.get();
    loom::WeaveId id;
    try {
        // Under the POLICY'S grant -- a grant, not host root -- and its accept mode. Its id is the
        // sender the bridge stamps onto this session's every send.
        id = bus_.register_weave(std::move(proxy), a.grant, a.accept);
    } catch (...) {
        refuse(c, "this host could not register a session for you");
        return;
    }
    c.id = id;
    c.proxy = raw;
    c.established = a.established_name;
    c.observes = a.observe;
    c.encoding = a.encoding;
    c.state = ConnectionState::Admitted;
    proxy_ids_.insert(id.value);
    std::string welcome;
    put_u64(welcome, c.id.value); // the session's stamped bus id
    put_u32(welcome, kBridgeProtocolVersion);
    put_bytes(welcome, c.established);
    c.ch->queue(BridgeOp::Welcome, welcome);
    push_weaves(c); // initial discovery, pushed so the client's weave list starts populated
    tell(c);
}

void BridgeServer::refuse(Conn& c, const std::string& why) {
    std::string body;
    put_bytes(body, why);
    c.refusal = why;
    c.state = ConnectionState::Refused;
    ++refused_;
    c.ch->queue(BridgeOp::Denied, body);
    c.ch->flush();
    tell(c); // REFUSED is told as itself, before the severance turns the row into CLOSED
    // Severed: the Denied is on its way and nothing more is read. The reap sees done().
    c.ch->fail();
}

void BridgeServer::apply(Conn& c, const ConnectionVerdict& verdict) {
    switch (verdict.kind) {
    case ConnectionVerdict::Kind::Admit:
        admit(c, verdict.admitted);
        return;
    case ConnectionVerdict::Kind::Refuse:
        refuse(c, verdict.reason.empty() ? std::string("admission refused") : verdict.reason);
        return;
    case ConnectionVerdict::Kind::Defer:
        c.state = ConnectionState::AwaitingDecision;
        tell(c);
        return;
    }
}

bool BridgeServer::decide(std::uint64_t connection, const ConnectionVerdict& verdict) {
    Conn* c = find(connection);
    if (c == nullptr || c->state != ConnectionState::AwaitingDecision || !c->ch || c->ch->done() ||
        verdict.kind == ConnectionVerdict::Kind::Defer) {
        return false;
    }
    apply(*c, verdict);
    return true;
}

bool BridgeServer::disconnect(std::uint64_t connection) {
    Conn* c = find(connection);
    if (c == nullptr || !c->ch || c->ch->done()) {
        return false;
    }
    c->ch->flush();
    c->ch->fail();
    return true;
}

std::optional<BridgeServer::SettleOrigin> BridgeServer::settle_origin(loom::Fence fence) const {
    if (!fence.valid()) {
        return std::nullopt;
    }
    for (const auto& c : conns_) {
        for (const Conn::Settling& s : c->settling) {
            if (s.fence.value == fence.value) {
                return SettleOrigin{c->id, s.correlation};
            }
        }
    }
    return std::nullopt;
}

void BridgeServer::on_hello(Conn& c, const BridgeIncoming& f) {
    Cursor cur(f.payload);
    std::uint32_t proto = 0;
    if (!cur.u32(proto)) {
        refuse(c, "malformed Hello");
        return;
    }
    // The two identity fields are read as empty when absent (a v3-shaped Hello), and bounded
    // when present: a peer has earned nothing yet, so what it can make this host hold is small.
    std::string_view claimed;
    std::string_view credential;
    (void)cur.bytes(claimed);
    (void)cur.bytes(credential);
    if (claimed.size() > kMaxHelloFieldBytes || credential.size() > kMaxHelloFieldBytes) {
        refuse(c, "Hello fields over " + std::to_string(kMaxHelloFieldBytes) + " bytes");
        return;
    }
    c.claimed = std::string(claimed);
    c.protocol = proto;
    if (proto != kBridgeProtocolVersion) {
        // Said in words, not severed silently: a peer built against another version learns
        // which one this host speaks, and nothing else happens on the connection.
        refuse(c, "protocol v" + std::to_string(proto) + " is not spoken here; this host speaks v" +
                      std::to_string(kBridgeProtocolVersion));
        return;
    }
    ConnectionRequest request;
    request.connection = c.number;
    request.protocol = proto;
    request.claimed_name = c.claimed;
    request.credential = std::string(credential);
    request.peer = c.peer;
    ConnectionVerdict verdict = ConnectionVerdict::refuse("no admission policy");
    try {
        verdict = admission_(request);
    } catch (...) {
        verdict = ConnectionVerdict::refuse("the admission policy failed");
    }
    apply(c, verdict);
}

void BridgeServer::on_frame(Conn& c, const BridgeIncoming& f) {
    // The handshake is load-bearing: a frame before Hello completes is broken or hostile -- sever
    // (mark the channel failed; the existing done()/reap_dead path tears it down). Anti-Postel.
    if (c.state == ConnectionState::AwaitingHello) {
        if (f.op != BridgeOp::Hello) {
            c.ch->fail();
            return;
        }
        on_hello(c, f);
        return;
    }
    if (c.state == ConnectionState::Refused) {
        return; // severed; nothing more is read
    }
    switch (f.op) {
    case BridgeOp::Hello:
        // A second Hello on an admitted or waiting connection is a protocol violation.
        c.ch->fail();
        break;
    case BridgeOp::ListWeaves:
        if (c.state == ConnectionState::Admitted) {
            push_weaves(c);
        }
        break;
    case BridgeOp::Describe: {
        Cursor cur(f.payload);
        std::string_view name;
        std::uint32_t version = 0;
        if (!cur.bytes(name) || !cur.u32(version)) {
            break; // malformed -> drop
        }
        if (c.state != ConnectionState::Admitted) {
            break; // a waiting connection learns nothing about this bus
        }
        std::shared_ptr<const loom::Schema> schema = bus_.resolve_schema(name, version);
        if (!schema) {
            std::string body;
            put_bytes(body, name);
            put_u32(body, version);
            c.ch->queue(BridgeOp::SchemaNone, body);
        } else {
            // Ship the schema AS BYTES (the IPC currency): encode it to its descriptor Value and
            // serialize, in the session's own encoding. The client re-admits + reconstructs it
            // (decode_schema).
            c.ch->queue(BridgeOp::Schema, encode_for(c.encoding, loom::encode_schema(*schema)));
        }
        break;
    }
    case BridgeOp::Send: {
        // [u8 kind][u8 flags][u64 wire_sender][u64 target][u64 wire_reply_to][u64 correlation]
        // [bytes role][bytes payload]
        Cursor cur(f.payload);
        std::uint8_t kind = 0;
        std::uint8_t flags = 0;
        std::uint64_t wire_sender = 0;
        std::uint64_t target = 0;
        std::uint64_t wire_reply_to = 0;
        std::uint64_t correlation = 0;
        std::string_view role;
        if (!cur.u8(kind) || !cur.u8(flags) || !cur.u64(wire_sender) || !cur.u64(target) ||
            !cur.u64(wire_reply_to) || !cur.u64(correlation) || !cur.bytes(role)) {
            // The header didn't parse far enough to yield a correlation -> 0. No dark drop.
            send_refused(c, 0, "malformed Send header");
            break;
        }
        if (c.state != ConnectionState::Admitted) {
            // WAITING IS NOT ADMITTED. Nothing is queued for later: the peer is told, and may ask
            // again once it has been admitted.
            send_refused(c, correlation, "not admitted: this connection is awaiting a decision");
            break;
        }
        const std::string_view payload = cur.rest();
        const bool settle = (flags & kSendSettle) != 0;
        if (settle && kind == kEmitPublish) {
            send_refused(c, correlation,
                         "a publication cannot be settled: it has no one delivery to follow; send it "
                         "to one target or office");
            break;
        }
        if (settle && c.settling.size() >= kMaxSettlingPerConnection) {
            // Refused BEFORE the bus, so the peer knows nothing acted: a settle-requested send is
            // never quietly downgraded to an unfenced one.
            send_refused(c, correlation,
                         "this session already waits on " +
                             std::to_string(kMaxSettlingPerConnection) +
                             " settlements; wait for one before asking for another");
            break;
        }

        // Re-admit the session's output through the ONE gate, host-side, exactly as the kernel does
        // for a loaded library's emitted message and the isolation host does for a child's Emit.
        // Parsed in the encoding the session was admitted to speak, and only that one.
        loom::Unverified u = c.encoding == PayloadEncoding::Compat ? loom::compat::parse(payload)
                                                                   : loom::parse(payload);
        if (c.encoding == PayloadEncoding::Compat && !u.well_formed()) {
            send_refused(c, correlation,
                         "malformed payload: this session's values cross as Zen's compat JSON "
                         "envelope ({\"zen\":1,\"schema\":...,\"version\":...,\"fields\":{...}})");
            break;
        }
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
            break; // gate-refused (malformed/hostile output)
        }
        // STAMP the sender + reply_to from the CONNECTION (c.id), NEVER the wire. A forged
        // reply_to cannot redirect replies to a third party; a forged sender cannot impersonate
        // another participant (MSG-02). The grant checked at delivery is the proxy's own.
        (void)wire_sender;
        (void)wire_reply_to;
        loom::Message msg(std::move(a).value(), loom::WeaveId{}, c.id, correlation);
        if (settle) {
            // FENCED AT THE ENQUEUE: this envelope and what its dispatch sets in motion are the
            // session's, counted by the bus, and the peer is told once when all of it has been
            // dispatched (report_settled). The fence bound is the bus's; at it, nothing is queued.
            loom::Fence fence;
            const loom::Ticket t =
                kind == kSendToRole
                    ? bus_.send_as_to_role_fenced(c.id, role, std::move(msg), &fence)
                    : bus_.send_as_fenced(c.id, loom::WeaveId{target}, std::move(msg), &fence);
            if (!t.valid() || !fence.valid()) {
                send_refused(c, correlation,
                             "this host is following as many settlements as it can; nothing was "
                             "sent");
                break;
            }
            c.settling.push_back(Conn::Settling{correlation, fence});
            break;
        }
        if (kind == kEmitPublish) {
            (void)bus_.publish_as(c.id, std::move(msg));
        } else if (kind == kSendToRole) {
            (void)bus_.send_as_to_role(c.id, role, std::move(msg));
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
    case BridgeOp::Denied:
    case BridgeOp::Settled:
        break;
    }
}

void BridgeServer::report_settled() {
    for (auto& c : conns_) {
        for (auto it = c->settling.begin(); it != c->settling.end();) {
            if (bus_.fence_state(it->fence) == loom::FenceState::Open) {
                ++it;
                continue;
            }
            // Settled: said once, then the fence is forgotten. (Unknown would mean this server
            // released it already, which only this loop and the reap do.)
            if (c->ch && !c->ch->done()) {
                std::string body;
                put_u64(body, it->correlation);
                c->ch->queue(BridgeOp::Settled, body);
            }
            bus_.release_fence(it->fence);
            it = c->settling.erase(it);
        }
    }
}

void BridgeServer::on_tap(const loom::BusEvent& e) {
    bool anyone = false;
    for (auto& c : conns_) {
        if (c->observes && c->state == ConnectionState::Admitted) {
            anyone = true;
            break;
        }
    }
    if (e.kind == loom::EventKind::Died || e.kind == loom::EventKind::Revived) {
        weaves_dirty_ = true; // deferred: the registry is read outside dispatch (in service())
    }
    if (!anyone) {
        return;
    }
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
    case loom::EventKind::HandlerFailed:
        kind = kTapHandlerFailed;
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

    // COPY-ONLY (event fields already in `e`) -- safe from inside a pump() observer callback,
    // exactly as the in-process record_tap is. Only a connection admitted WITH observation.
    for (auto& c : conns_) {
        if (c->observes && c->state == ConnectionState::Admitted) {
            c->ch->queue(BridgeOp::Tap, body);
        }
    }
}

void BridgeServer::reap_dead() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        Conn& c = **it;
        if (c.ch && c.ch->done()) {
            // Unregister the proxy FIRST (no further delivery lands on it), destroy it before its
            // channel, then drop the connection. Disconnect handled as an event -- not a hang.
            if (c.proxy != nullptr) {
                c.proxy->detach();
            }
            for (const Conn::Settling& s : c.settling) {
                bus_.release_fence(s.fence); // nobody is left to tell
            }
            c.settling.clear();
            if (c.id.value != 0) {
                std::unique_ptr<loom::Weave> removed = bus_.unregister_weave(c.id);
                removed.reset();
                proxy_ids_.erase(c.id.value);
            }
            if (!c.told_closed) {
                c.told_closed = true;
                c.state = ConnectionState::Closed;
                tell(c);
            }
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void BridgeServer::service() {
    accept_new();
    for (auto& c : conns_) {
        if (!c->ch || c->ch->done()) {
            continue;
        }
        std::vector<BridgeIncoming> frames;
        c->ch->poll(frames);
        for (const BridgeIncoming& f : frames) {
            on_frame(*c, f);
            if (c->ch->done()) {
                break; // a severance failed this channel -- stop processing its batch
            }
        }
    }
    // Drain the deferred weave-list refresh: the registry reads happen HERE, outside dispatch.
    if (weaves_dirty_) {
        for (auto& c : conns_) {
            if (c->state == ConnectionState::Admitted && c->ch && !c->ch->done()) {
                push_weaves(*c);
            }
        }
        weaves_dirty_ = false;
    }
    report_settled();
    for (auto& c : conns_) {
        if (c->ch) {
            c->ch->flush();
        }
    }
    reap_dead();
}

void BridgeServer::step() {
    accept_new();
    for (auto& c : conns_) {
        if (!c->ch || c->ch->done()) {
            continue;
        }
        std::vector<BridgeIncoming> frames;
        c->ch->poll(frames);
        for (const BridgeIncoming& f : frames) {
            on_frame(*c, f);
            if (c->ch->done()) {
                break;
            }
        }
    }
    // Proxies fire-and-continue (ship Delivered); the tap observer streams Tap.
    // Drain-to-idle by default -- the contract every existing caller has. A host composing with a
    // perpetual in-process service asks for a bounded turn so this returns (MSG-09).
    if (bounded_dispatch_) {
        bus_.pump_pending();
    } else {
        bus_.drain_until_idle();
    }
    if (weaves_dirty_) {
        for (auto& c : conns_) {
            if (c->state == ConnectionState::Admitted && c->ch && !c->ch->done()) {
                push_weaves(*c);
            }
        }
        weaves_dirty_ = false;
    }
    report_settled(); // after the turn, so what the turn settled is told in the same step
    for (auto& c : conns_) {
        if (c->ch) {
            c->ch->flush();
        }
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
