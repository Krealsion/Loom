// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The connecting side, decoded frame by frame. Nothing here interprets a payload: the bytes
// belong to the participant that asked and are handed back as they arrived.

#include <zen/bridge/client.hpp>

#include <chrono>
#include <utility>

namespace loom {

BridgeClient::BridgeClient(socket_t sock) : ch_(std::make_unique<BridgeChannel>(sock)) {}

BridgeClient::~BridgeClient() = default;

bool BridgeClient::hello(std::string_view claimed_name, std::string_view credential) {
    if (!ch_ || ch_->done()) {
        return false;
    }
    std::string body;
    put_u32(body, kBridgeProtocolVersion);
    put_bytes(body, claimed_name);
    put_bytes(body, credential);
    ch_->queue(BridgeOp::Hello, body);
    ch_->flush();
    return !ch_->done();
}

bool BridgeClient::await_admission(int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        std::vector<BridgeEvent> events;
        poll(events);
        for (BridgeEvent& e : events) {
            held_.push_back(std::move(e));
        }
        if (admitted_ || denied_) {
            return admitted_;
        }
        if (disconnected()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        (void)bridge_wait_readable({ch_->fd()}, left > 0 ? static_cast<int>(left) : 0);
    }
}

void BridgeClient::send_frame(std::uint8_t kind, std::uint8_t flags, std::uint64_t target,
                              std::string_view role, std::uint64_t correlation,
                              std::string_view payload) {
    if (!ch_ || ch_->done()) {
        return;
    }
    std::string frame;
    put_u8(frame, kind);
    put_u8(frame, flags);
    put_u64(frame, 0); // wire_sender: the honest client sets 0 -- the host stamps the session
    put_u64(frame, target);
    put_u64(frame, 0); // wire_reply_to: likewise
    put_u64(frame, correlation);
    put_bytes(frame, role);
    frame.append(payload);
    ch_->queue(BridgeOp::Send, frame);
}

void BridgeClient::send(std::uint64_t target, std::uint64_t correlation, std::string_view payload,
                        bool settle) {
    send_frame(kEmitSend, settle ? kSendSettle : 0, target, {}, correlation, payload);
}

void BridgeClient::send_to_role(std::string_view role, std::uint64_t correlation,
                                std::string_view payload, bool settle) {
    send_frame(kSendToRole, settle ? kSendSettle : 0, 0, role, correlation, payload);
}

void BridgeClient::publish(std::uint64_t correlation, std::string_view payload) {
    send_frame(kEmitPublish, 0, 0, {}, correlation, payload);
}

void BridgeClient::describe(std::string_view name, std::uint32_t version) {
    if (!ch_ || ch_->done()) {
        return;
    }
    std::string body;
    put_bytes(body, name);
    put_u32(body, version);
    ch_->queue(BridgeOp::Describe, body);
}

void BridgeClient::list_weaves() {
    if (ch_ && !ch_->done()) {
        ch_->queue(BridgeOp::ListWeaves, {});
    }
}

void BridgeClient::flush() {
    if (ch_) {
        ch_->flush();
    }
}

bool BridgeClient::decode(const BridgeIncoming& f, BridgeEvent& out) {
    Cursor cur(f.payload);
    switch (f.op) {
    case BridgeOp::Welcome: {
        std::uint64_t id = 0;
        std::uint32_t proto = 0;
        if (!cur.u64(id) || !cur.u32(proto)) {
            return false;
        }
        std::string_view name;
        (void)cur.bytes(name); // absent on an older host: no established name
        out.kind = BridgeEvent::Kind::Welcome;
        out.session = id;
        out.version = proto;
        out.established_name = std::string(name);
        return true;
    }
    case BridgeOp::Denied: {
        std::string_view why;
        (void)cur.bytes(why);
        out.kind = BridgeEvent::Kind::Denied;
        out.reason = std::string(why);
        return true;
    }
    case BridgeOp::Delivered: {
        std::uint64_t sender = 0;
        std::uint64_t correlation = 0;
        std::uint8_t flags = 0;
        std::string_view role;
        if (!cur.u64(sender) || !cur.u64(correlation) || !cur.u8(flags) || !cur.bytes(role)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::Delivered;
        out.sender = sender;
        out.correlation = correlation;
        out.answers_ask = (flags & kDeliveredAnswersAsk) != 0;
        out.dispatch_refused = (flags & kDeliveredDispatchRefused) != 0;
        out.authored_role = std::string(role);
        out.payload = std::string(cur.rest());
        return true;
    }
    case BridgeOp::SendRefused: {
        std::uint64_t correlation = 0;
        std::string_view why;
        if (!cur.u64(correlation) || !cur.bytes(why)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::SendRefused;
        out.correlation = correlation;
        out.reason = std::string(why);
        return true;
    }
    case BridgeOp::Settled: {
        std::uint64_t correlation = 0;
        if (!cur.u64(correlation)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::Settled;
        out.correlation = correlation;
        return true;
    }
    case BridgeOp::Weaves: {
        std::uint32_t n = 0;
        if (!cur.u32(n)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::Weaves;
        for (std::uint32_t i = 0; i < n; ++i) {
            BridgeEvent::WeaveEntry w;
            std::uint32_t m = 0;
            if (!cur.u64(w.id) || !cur.u32(m)) {
                return false;
            }
            for (std::uint32_t j = 0; j < m; ++j) {
                std::string_view name;
                std::uint32_t ver = 0;
                if (!cur.bytes(name) || !cur.u32(ver)) {
                    return false;
                }
                w.accepts.emplace_back(std::string(name), ver);
            }
            out.weaves.push_back(std::move(w));
        }
        return true;
    }
    case BridgeOp::Schema:
        out.kind = BridgeEvent::Kind::Schema;
        out.payload = f.payload;
        return true;
    case BridgeOp::SchemaNone: {
        std::string_view name;
        std::uint32_t ver = 0;
        if (!cur.bytes(name) || !cur.u32(ver)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::SchemaNone;
        out.shape = std::string(name);
        out.version = ver;
        return true;
    }
    case BridgeOp::Tap: {
        std::uint8_t kind = 0;
        std::uint64_t target = 0;
        std::uint64_t sender = 0;
        std::string_view schema;
        std::uint32_t ver = 0;
        std::string_view refusal;
        if (!cur.u8(kind) || !cur.u64(target) || !cur.u64(sender) || !cur.bytes(schema) ||
            !cur.u32(ver) || !cur.bytes(refusal)) {
            return false;
        }
        out.kind = BridgeEvent::Kind::Tap;
        out.tap_kind = kind;
        out.target = target;
        out.sender = sender;
        out.shape = std::string(schema);
        out.version = ver;
        out.reason = std::string(refusal);
        return true;
    }
    case BridgeOp::Hello:
    case BridgeOp::ListWeaves:
    case BridgeOp::Describe:
    case BridgeOp::Send:
        return false; // client->host opcodes never arrive at a client
    }
    return false;
}

void BridgeClient::poll(std::vector<BridgeEvent>& out) {
    for (BridgeEvent& e : held_) {
        out.push_back(std::move(e));
    }
    held_.clear();
    if (!ch_) {
        return;
    }
    ch_->flush();
    std::vector<BridgeIncoming> frames;
    ch_->poll(frames);
    for (const BridgeIncoming& f : frames) {
        BridgeEvent e;
        if (!decode(f, e)) {
            continue; // a malformed host frame is dropped; the transport already bounds it
        }
        if (e.kind == BridgeEvent::Kind::Welcome) {
            admitted_ = true;
            session_ = e.session;
            established_ = e.established_name;
        } else if (e.kind == BridgeEvent::Kind::Denied) {
            denied_ = true;
            denial_ = e.reason;
        }
        out.push_back(std::move(e));
    }
    if (ch_->done() && !said_disconnected_) {
        said_disconnected_ = true;
        BridgeEvent gone;
        gone.kind = BridgeEvent::Kind::Disconnected;
        out.push_back(std::move(gone));
    }
}

} // namespace loom
