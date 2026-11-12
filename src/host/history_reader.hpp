// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_HISTORY_READER_HPP
#define ZEN_HOST_HISTORY_READER_HPP

// A scoped reader over the host's own Recorder, answered by message: role `loom.history`,
// asked `DeliveriesTo{participant}` and `Delivery{seq}` (zen/session/vocabulary.hpp) by a
// client that is not the console and has no tap. It adds no memory: every answer is read off
// the Recorder in its own four words, and a row descended from a link's own `loom.link.Crossed`
// carries that crossing, read from its retained payload. The host blacklists this reader in the
// Recorder, so reading never writes history; who may ask is the session door's grant.

#include <zen/bridge/link.hpp>
#include <zen/history/recorder.hpp>
#include <zen/serialize.hpp>
#include <zen/session/vocabulary.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace loom::host {

struct HistoryReaderState {
    std::int64_t asked = 0;
    ZEN_SHAPE(HistoryReaderState, 1, ZEN_FIELD(asked));
};

class HistoryReader final
    : public WeaveBase<HistoryReader, HistoryReaderState,
                       Accept<session::DeliveriesTo, session::Delivery>,
                       Emit<session::Records, Refused>> {
public:
    explicit HistoryReader(const Recorder& recorder) : recorder_(&recorder) {}

    /// Host wiring: the WeaveIds of the links this host mounted -- the only senders whose
    /// self-said `loom.link.Crossed` is a crossing.
    void set_links(std::set<std::uint64_t> links) { links_ = std::move(links); }

    void on(const session::DeliveriesTo& q, Mail& mail) {
        ++state_.asked;
        const std::int64_t limit = q.limit <= 0 || q.limit > session::kMaxHistoryRows
                                       ? session::kMaxHistoryRows
                                       : q.limit;
        session::Records out = header();
        const std::vector<HistoryRecord> all = recorder_->snapshot(); // oldest first
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->kind != RecordKind::Delivery ||
                it->target.value != static_cast<std::uint64_t>(q.participant) ||
                (!q.shape.empty() && it->shape != q.shape)) {
                continue;
            }
            if (static_cast<std::int64_t>(out.rows.size()) >= limit) {
                out.truncated = true;
                break;
            }
            out.rows.push_back(row_of(*it));
        }
        (void)mail.answer(out);
    }

    void on(const session::Delivery& q, Mail& mail) {
        ++state_.asked;
        session::Records out = header();
        out.asked_seq = q.seq;
        const Lookup l = recorder_->find(static_cast<std::uint64_t>(q.seq));
        out.asked_horizon = horizon_word(l.horizon);
        if (l.record != nullptr) {
            out.rows.push_back(row_of(*l.record));
        }
        (void)mail.answer(out);
    }

    /// The row for one retained record, and what can be said about its dispatch parent.
    session::Record row_of(const HistoryRecord& r) const {
        session::Record row;
        row.record = static_cast<std::int64_t>(r.record_seq);
        row.seq = static_cast<std::int64_t>(r.seq);
        row.sender = static_cast<std::int64_t>(r.sender.value);
        row.target = static_cast<std::int64_t>(r.target.value);
        row.shape = r.shape;
        row.version = static_cast<std::int64_t>(r.shape_version);
        row.correlation = static_cast<std::int64_t>(r.correlation);
        row.parent = static_cast<std::int64_t>(r.dispatch_parent);
        row.outcome = outcome_word(r.outcome);
        if (r.outcome == RecordedOutcome::Refused) {
            row.refusal = name_of(r.refusal);
        }
        row.payload = payload_word(recorder_->payload(r.record_seq).state);
        if (r.dispatch_parent == 0) {
            row.parent_horizon = "none";
            return row;
        }
        const Lookup p = recorder_->find(r.dispatch_parent);
        row.parent_horizon = horizon_word(p.horizon);
        if (p.record == nullptr) {
            return row;
        }
        const HistoryRecord& parent = *p.record;
        row.parent_shape = parent.shape;
        const bool crossing = parent.shape == link::Crossed::zen_name &&
                              parent.sender == parent.target &&
                              links_.count(parent.sender.value) != 0;
        if (!crossing) {
            return row;
        }
        const PayloadLookup bytes = recorder_->payload(parent.record_seq);
        if (bytes.state != PayloadState::Retained) {
            row.crossing.is = session::CrossingUnread{payload_word(bytes.state)};
            return row;
        }
        Admission a = admit(loom::parse(bytes.bytes), schema_of<link::Crossed>());
        if (!a.ok()) {
            row.crossing.is = session::CrossingUnread{"unreadable"};
            return row;
        }
        const link::Crossed x = from_value<link::Crossed>(a.value());
        session::CrossingRead read;
        read.link = x.link;
        read.epoch = x.epoch;
        read.far_session = x.session;
        read.established = x.established;
        read.kind = schema_of<link::Frame>()->fields()[x.frame()].name;
        read.attempt = x.attempt();
        if (const link::FarWord* w = x.word()) {
            read.far_sender = w->far_sender;
            read.far_role = w->far_role;
            read.far_shape = w->shape;
            read.far_version = w->version;
        }
        row.crossing.is = std::move(read);
        return row;
    }

    static std::string horizon_word(Horizon h) {
        switch (h) {
        case Horizon::Retained:
            return "retained";
        case Horizon::Forgotten:
            return "forgotten";
        case Horizon::NotRecorded:
            return "not-recorded";
        case Horizon::Unobserved:
            return "unobserved";
        }
        return "unknown";
    }

    static std::string payload_word(PayloadState s) {
        switch (s) {
        case PayloadState::Absent:
            return "absent";
        case PayloadState::Retained:
            return "retained";
        case PayloadState::Evicted:
            return "evicted";
        case PayloadState::Declined:
            return "declined";
        }
        return "unknown";
    }

    static std::string outcome_word(RecordedOutcome o) {
        switch (o) {
        case RecordedOutcome::None:
            return "none";
        case RecordedOutcome::Delivered:
            return "delivered";
        case RecordedOutcome::Refused:
            return "refused";
        case RecordedOutcome::HandlerFailed:
            return "handler-failed";
        }
        return "unknown";
    }

private:
    session::Records header() const {
        session::Records out;
        const RecorderBounds b = recorder_->bounds();
        out.newest_seq = static_cast<std::int64_t>(b.newest_observed_seq);
        out.oldest_retained_seq = static_cast<std::int64_t>(b.oldest_retained_seq);
        out.forgotten_horizon = static_cast<std::int64_t>(b.forgotten_horizon_seq);
        out.forgotten = static_cast<std::int64_t>(b.forgotten);
        return out;
    }

    const Recorder* recorder_;
    std::set<std::uint64_t> links_;
};

} // namespace loom::host

#endif // ZEN_HOST_HISTORY_READER_HPP
