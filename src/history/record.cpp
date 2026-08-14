// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/history/record.hpp>

#include <algorithm>

namespace loom {

const char* name_of(RecordKind k) noexcept {
    switch (k) {
    case RecordKind::Delivery:
        return "Delivery";
    case RecordKind::Lifecycle:
        return "Lifecycle";
    case RecordKind::RecorderPolicy:
        return "RecorderPolicy";
    }
    return "?";
}

const char* name_of(RecordedOutcome o) noexcept {
    switch (o) {
    case RecordedOutcome::None:
        return "None";
    case RecordedOutcome::Delivered:
        return "Delivered";
    case RecordedOutcome::Refused:
        return "Refused";
    case RecordedOutcome::HandlerFailed:
        return "HandlerFailed";
    }
    return "?";
}

const char* name_of(PayloadDisposition d) noexcept {
    switch (d) {
    case PayloadDisposition::None:
        return "None";
    case PayloadDisposition::Retained:
        return "Retained";
    case PayloadDisposition::TooLarge:
        return "TooLarge";
    case PayloadDisposition::NotRetained:
        return "NotRetained";
    }
    return "?";
}

const char* name_of(PayloadState s) noexcept {
    switch (s) {
    case PayloadState::Absent:
        return "Absent";
    case PayloadState::Retained:
        return "Retained";
    case PayloadState::Evicted:
        return "Evicted";
    case PayloadState::Declined:
        return "Declined";
    }
    return "?";
}

std::string describe_held(HeldMask m) {
    std::string out;
    const auto add = [&out](const char* s) {
        if (!out.empty()) {
            out += "|";
        }
        out += s;
    };
    if (held_in(m, Held::LastCall)) {
        add("LastCall");
    }
    if (held_in(m, Held::Recent)) {
        add("Recent");
    }
    if (held_in(m, Held::Protected)) {
        add("Protected");
    }
    return out.empty() ? std::string("none") : out;
}

void fill_from_event(HistoryRecord& rec, const BusEvent& e) {
    rec.kind = e.kind == EventKind::Died || e.kind == EventKind::Revived
                   ? RecordKind::Lifecycle
                   : RecordKind::Delivery;
    rec.seq = e.seq;
    rec.sender = e.sender;
    rec.target = e.target;
    rec.addressed_role = e.addressed_role;
    rec.authored_role = e.authored_role;
    rec.shape = e.schema_name;
    rec.shape_version = e.schema_version;
    rec.correlation = e.correlation;
    rec.dispatch_parent = e.dispatch_parent;
    rec.handler_elapsed_ns = e.handler_elapsed_ns;
    rec.from_last_known_good = e.from_last_known_good;
    switch (e.kind) {
    case EventKind::Delivered:
        rec.outcome = RecordedOutcome::Delivered;
        break;
    case EventKind::Refused:
        rec.outcome = RecordedOutcome::Refused;
        break;
    case EventKind::HandlerFailed:
        rec.outcome = RecordedOutcome::HandlerFailed;
        break;
    case EventKind::Died:
    case EventKind::Revived:
        rec.outcome = RecordedOutcome::None;
        break;
    }
    rec.refusal = e.refusal.reason;
    if (e.refusal.reason == RefusalReason::GateRefused) {
        // The gate's field path / expected / actual — the half the console's tap
        // window drops, and the half that says WHICH FIELD was wrong.
        rec.refusal_detail = e.refusal.error.message();
    }
}

void RecorderBlacklist::declare_participant(WeaveId id) {
    if (id.valid() &&
        std::find(participants_.begin(), participants_.end(), id) == participants_.end()) {
        participants_.push_back(id);
    }
}

void RecorderBlacklist::declare_shape(std::string name) {
    if (!name.empty() && std::find(shapes_.begin(), shapes_.end(), name) == shapes_.end()) {
        shapes_.push_back(std::move(name));
    }
}

bool RecorderBlacklist::excludes(const BusEvent& e) const noexcept {
    for (const WeaveId& id : participants_) {
        // BOTH ENDS. A recorder's own storage exchange is recorder machinery
        // whichever direction it travels, and excluding only the sender would let
        // the answer into the history the question was kept out of.
        if (e.sender == id || e.target == id) {
            return true;
        }
    }
    for (const std::string& s : shapes_) {
        if (e.schema_name == s) {
            return true;
        }
    }
    return false;
}

} // namespace loom
