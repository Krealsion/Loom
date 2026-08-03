// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The client side: a Console/LadderHost over the operator-protocol socket. The frontend drives this
// exactly as it drives the in-process ConsoleEngine; only the transport differs. Compose runs the
// SHARED ladder client-side against a wire-fetched schema and the local buffer, then ships the
// assembled message bytes as a Send frame the host re-admits + stamps.

#include <zen/bridge/remote_console.hpp>

#include <zen/kernel/schema_codec.hpp> // decode_schema, schema_desc_schema
#include <zen/serialize.hpp>           // serialize, parse, admit

#include <chrono>
#include <thread>
#include <utility>

namespace loom {

RemoteConsole::RemoteConsole(socket_t sock, int handshake_timeout_ms) {
    ch_ = std::make_unique<BridgeChannel>(sock);
    std::string hello;
    put_u32(hello, kBridgeProtocolVersion);
    ch_->queue(BridgeOp::Hello, hello);
    ch_->flush();
    // Block (bounded) until Welcome arrives; the initial Weaves push usually rides in the same batch.
    connected_ = await([this]() { return connected_; }, handshake_timeout_ms);
    pump_once(); // drain any frames that landed right after Welcome
}

RemoteConsole::~RemoteConsole() = default;

void RemoteConsole::pump_once() const {
    if (!ch_) {
        return;
    }
    ch_->flush();
    std::vector<BridgeIncoming> frames;
    ch_->poll(frames);
    for (const BridgeIncoming& f : frames) {
        process(f);
    }
}

void RemoteConsole::pump() { pump_once(); }

bool RemoteConsole::await(const std::function<bool()>& done, int timeout_ms) const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        pump_once();
        if (done()) {
            return true;
        }
        if (!ch_ || ch_->done()) {
            return done(); // disconnected: one last check, then give up (never an indefinite block)
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return done();
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        // Wait for the socket to become readable (the client's little multiplexer over one source).
        (void)bridge_wait_readable({ch_->fd()}, left > 0 ? static_cast<int>(left) : 0);
    }
}

void RemoteConsole::push_bridge_refused(const std::string& reason) const {
    // An honestly-named NON-bus tap kind: the send (or reply) never entered the bus, so we do NOT
    // forge a bus event for it — the distinct kind string IS the honesty. It renders in the existing
    // tap log with zero new UI surface. (target/sender/schema stay empty: there is no bus event.)
    TapEvent t;
    t.kind = "BridgeRefused";
    t.refusal = reason;
    tap_.push_back(std::move(t));
    dirty_.tap = true;
}

void RemoteConsole::process(const BridgeIncoming& f) const {
    switch (f.op) {
    case BridgeOp::Welcome: {
        Cursor cur(f.payload);
        std::uint64_t id = 0;
        std::uint32_t proto = 0;
        if (cur.u64(id) && cur.u32(proto)) {
            operator_id_ = loom::WeaveId{id};
            connected_ = true;
        }
        break;
    }
    case BridgeOp::Weaves: {
        Cursor cur(f.payload);
        std::uint32_t n = 0;
        if (!cur.u32(n)) {
            break;
        }
        std::vector<WeaveInfo> fresh;
        bool ok = true;
        for (std::uint32_t i = 0; i < n && ok; ++i) {
            std::uint64_t id = 0;
            std::uint32_t m = 0;
            if (!cur.u64(id) || !cur.u32(m)) {
                ok = false;
                break;
            }
            WeaveInfo info;
            info.id = loom::WeaveId{id};
            for (std::uint32_t j = 0; j < m && ok; ++j) {
                std::string_view name;
                std::uint32_t ver = 0;
                if (!cur.bytes(name) || !cur.u32(ver)) {
                    ok = false;
                    break;
                }
                info.accepts.push_back({std::string(name), ver});
            }
            if (ok) {
                fresh.push_back(std::move(info));
            }
        }
        if (ok) {
            weaves_ = std::move(fresh);
            dirty_.weaves = true;
        }
        break;
    }
    case BridgeOp::Schema: {
        // [bytes encoded_schema] — re-admit against the meta-schema, reconstruct, register.
        loom::Unverified u = loom::parse(f.payload);
        loom::Admission a = loom::admit(u, loom::schema_desc_schema());
        if (!a.ok()) {
            break;
        }
        try {
            std::shared_ptr<const loom::Schema> s = loom::decode_schema(a.value(), registry_);
            if (!registry_.lookup(s->name(), s->version())) {
                registry_.register_schema(s);
            }
        } catch (...) {
            break; // a malformed/unresolvable descriptor: ignore it
        }
        // A newly known schema may unblock Delivered replies that were waiting for it.
        std::vector<std::string> still;
        for (std::string& bytes : pending_delivered_) {
            loom::Unverified pu = loom::parse(bytes);
            std::shared_ptr<const loom::Schema> sc =
                registry_.lookup(pu.claimed_name(), pu.claimed_version());
            if (!sc) {
                still.push_back(std::move(bytes));
                continue;
            }
            loom::Admission pa = loom::admit(pu, sc);
            if (pa.ok()) {
                buffer_.push_back(std::move(pa).value());
                dirty_.buffer = true;
            }
        }
        pending_delivered_.swap(still);
        break;
    }
    case BridgeOp::SchemaNone: {
        Cursor cur(f.payload);
        std::string_view name;
        std::uint32_t ver = 0;
        if (!(cur.bytes(name) && cur.u32(ver))) {
            break;
        }
        const std::string sname(name);
        schema_absent_.insert({sname, ver});
        // Drain any pending Delivered now known-absent — its Value can never be built. Surface each.
        std::vector<std::string> keep;
        for (std::string& bytes : pending_delivered_) {
            loom::Unverified pu = loom::parse(bytes);
            if (pu.claimed_name() == sname && pu.claimed_version() == ver) {
                push_bridge_refused("dropped a reply for no-such-schema " + sname + " v" +
                                    std::to_string(ver));
            } else {
                keep.push_back(std::move(bytes));
            }
        }
        pending_delivered_.swap(keep);
        break;
    }
    case BridgeOp::Delivered: {
        // The payload IS a serialized reply Value. Admit it if its schema is known; otherwise fetch
        // the schema (async) and stash the bytes to admit once the Schema reply lands.
        loom::Unverified u = loom::parse(f.payload);
        std::shared_ptr<const loom::Schema> schema =
            registry_.lookup(u.claimed_name(), u.claimed_version());
        if (schema) {
            loom::Admission a = loom::admit(u, schema);
            if (a.ok()) {
                buffer_.push_back(std::move(a).value());
                dirty_.buffer = true;
            }
        } else if (ch_) {
            if (pending_delivered_.size() >= kMaxPendingDelivered) {
                // Bound what the host can make us hold: past the cap, drop this reply and surface it
                // rather than fetch-and-stash unboundedly (even a host is not trusted to be finite).
                push_bridge_refused("dropped a reply: pending-overflow (>" +
                                    std::to_string(kMaxPendingDelivered) + " unknown-schema replies)");
                break;
            }
            std::string body;
            put_bytes(body, u.claimed_name());
            put_u32(body, u.claimed_version());
            ch_->queue(BridgeOp::Describe, body);
            pending_delivered_.push_back(f.payload);
        }
        break;
    }
    case BridgeOp::Tap: {
        Cursor cur(f.payload);
        std::uint8_t kind = 0;
        std::uint64_t target = 0;
        std::uint64_t sender = 0;
        std::string_view schema;
        std::uint32_t ver = 0;
        std::string_view refusal;
        if (!cur.u8(kind) || !cur.u64(target) || !cur.u64(sender) || !cur.bytes(schema) ||
            !cur.u32(ver) || !cur.bytes(refusal)) {
            break;
        }
        TapEvent t;
        t.kind = kind == kTapDelivered  ? "Delivered"
                 : kind == kTapRefused  ? "Refused"
                 : kind == kTapDied      ? "Died"
                                         : "Revived";
        t.target = loom::WeaveId{target};
        t.sender = loom::WeaveId{sender};
        t.schema = std::string(schema);
        t.refusal = std::string(refusal);
        tap_.push_back(std::move(t));
        dirty_.tap = true;
        break;
    }
    case BridgeOp::SendRefused: {
        // A Send the host dropped before the bus (no tap event exists) — surface it honestly.
        Cursor cur(f.payload);
        std::uint64_t correlation = 0;
        std::string_view reason;
        if (cur.u64(correlation) && cur.bytes(reason)) {
            (void)correlation; // reserved: a future consumer correlates a send to its refusal
            push_bridge_refused(std::string(reason));
        }
        break;
    }
    // client->host opcodes never arrive at the client; ignore.
    case BridgeOp::Hello:
    case BridgeOp::ListWeaves:
    case BridgeOp::Describe:
    case BridgeOp::Send:
        break;
    }
}

std::vector<WeaveInfo> RemoteConsole::weaves() const { return weaves_; }

std::shared_ptr<const loom::Schema> RemoteConsole::fetch_schema(std::string_view name,
                                                                std::uint32_t version) const {
    if (std::shared_ptr<const loom::Schema> s = registry_.lookup(name, version)) {
        return s;
    }
    const std::pair<std::string, std::uint32_t> key{std::string(name), version};
    if (schema_absent_.count(key) != 0) {
        return nullptr;
    }
    if (!ch_ || ch_->done()) {
        return nullptr;
    }
    std::string body;
    put_bytes(body, name);
    put_u32(body, version);
    ch_->queue(BridgeOp::Describe, body);
    ch_->flush();
    (void)await(
        [&]() { return registry_.lookup(name, version) != nullptr || schema_absent_.count(key) != 0; },
        5000);
    return registry_.lookup(name, version);
}

std::shared_ptr<const loom::Schema> RemoteConsole::resolve_schema(std::string_view name,
                                                                  std::uint32_t version) const {
    return fetch_schema(name, version);
}

std::optional<ShapeDesc> RemoteConsole::describe(std::string_view name,
                                                 std::uint32_t version) const {
    std::shared_ptr<const loom::Schema> schema = fetch_schema(name, version);
    if (!schema) {
        return std::nullopt;
    }
    return describe_schema(*schema);
}

Composed RemoteConsole::compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                                const std::vector<Arg>& args) {
    return run_compose_ladder(*this, target, name, version, args);
}

std::optional<loom::Cell> RemoteConsole::resolve_ref(const Ref& ref, std::string* error) const {
    return resolve_ref_from(*this, ref, error);
}

loom::Ticket RemoteConsole::assemble_and_send(loom::WeaveId target,
                                              const std::shared_ptr<const loom::Schema>& schema,
                                              const std::map<std::string, loom::Cell>& cells) {
    loom::Value v =
        loom::construct_blind(schema, [&](const loom::Field& f) -> std::optional<loom::Cell> {
            auto it = cells.find(f.name);
            if (it == cells.end()) {
                return std::nullopt;
            }
            return it->second;
        });
    std::string frame;
    put_u8(frame, kEmitSend);
    put_u64(frame, 0);            // wire_sender: the honest client sets 0 — the bridge stamps c.id
    put_u64(frame, target.value);
    put_u64(frame, 0);            // wire_reply_to: the honest client sets 0 — the bridge stamps c.id
    put_u64(frame, ++correlation_);
    try {
        frame.append(loom::serialize(v)); // a Ready compose is complete + type-checked, so this holds
    } catch (...) {
        return loom::Ticket{}; // defensive: never crash the client on an unexpected serialize throw
    }
    if (ch_) {
        ch_->queue(BridgeOp::Send, frame);
        ch_->flush();
    }
    // The remote frontend reads the compose status + the tap/buffer (the reply or a Refused tap),
    // never this ticket — the send's fate crosses back as bus events, not a local outcome.
    return loom::Ticket{};
}

std::size_t RemoteConsole::buffer_size() const { return buffer_.size(); }

std::optional<BufferEntry> RemoteConsole::buffer_at(std::size_t one_based_index) const {
    if (one_based_index == 0 || one_based_index > buffer_.size()) {
        return std::nullopt;
    }
    const loom::Value& v = buffer_[one_based_index - 1];
    return BufferEntry{"m" + std::to_string(one_based_index), v.schema().name(), v.schema().version(),
                       v};
}

std::vector<TapEvent> RemoteConsole::tap() const { return tap_; }

Dirty RemoteConsole::take_dirty() {
    Dirty d = dirty_;
    dirty_ = Dirty{};
    return d;
}

} // namespace loom
