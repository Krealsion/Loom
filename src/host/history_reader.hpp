// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_HISTORY_READER_HPP
#define ZEN_HOST_HISTORY_READER_HPP

// A SCOPED READER OVER THE HOST'S OWN RECORDER, ANSWERED BY MESSAGE.
//
// The console reads the Recorder directly (`history ...`). A client attached to a session host is
// not the console and is not given a tap, so it asks this participant instead -- office
// `loom.history`, two questions (`zen/session/vocabulary.hpp`):
//
//   DeliveriesTo{participant}   the deliveries TO one participant this host still remembers,
//                               newest first -- e.g. a run worker's session, to find the answers
//                               it was handed and where they came from
//   Delivery{seq}               what became of one bus delivery
//
// and, for every row whose dispatch parent is a LINK's own `loom.link.Crossed` record, the
// crossing it descends from: the link's name and epoch, the far session and the name the far host
// established, the far bus's stamp and office, the attempt, the kind of frame. That is the link's
// account of what ARRIVED, recorded before the link acted -- never an observation of the far
// execution -- and the reader says so by what it names it.
//
// IT ADDS NO MEMORY. Every answer is read off the Recorder at the moment of the question, in the
// Recorder's own four words -- retained, forgotten, not-recorded, unobserved -- so a record the
// window released is reported as forgotten, never as "nothing happened". A crossing's fields need
// its record's PAYLOAD, which is the host's retention choice (`history.retain` in the boot plan):
// kept, the fields are read back through the gate; declined or evicted, the reader says which and
// names no field it could not read.
//
// A CROSSING IS A LINK'S OWN WORD, AND ONLY THAT. A `loom.link.Crossed` counts when its record was
// said by a link this host mounted, to itself -- the host tells the reader which WeaveIds are its
// links. Any other participant that sends itself something shaped like a crossing is ordinary
// traffic here, exactly as the link itself ignores it.
//
// READING IS NOT WRITING. The host declares this participant in the Recorder's structural
// blacklist, so its questions and answers never enter the history they are about.
//
// WHO MAY ASK is the grant's business, not this reader's: the session door gives its owner's
// client sessions these two shapes, and gives run workers neither.

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
        row.crossing = parent.shape == link::Crossed::zen_name && parent.sender == parent.target &&
                       links_.count(parent.sender.value) != 0;
        if (!row.crossing) {
            return row;
        }
        const PayloadLookup bytes = recorder_->payload(parent.record_seq);
        row.crossing_payload = payload_word(bytes.state);
        if (bytes.state != PayloadState::Retained) {
            return row;
        }
        Admission a = admit(loom::parse(bytes.bytes), schema_of<link::Crossed>());
        if (!a.ok()) {
            row.crossing_payload = "unreadable";
            return row;
        }
        const Value& v = a.value();
        const auto text = [&v](const char* f) {
            const Cell* c = v.get(f);
            return c != nullptr ? c->as_text() : std::string();
        };
        const auto number = [&v](const char* f) {
            const Cell* c = v.get(f);
            return c != nullptr ? c->as_int() : std::int64_t{0};
        };
        row.link = text("link");
        row.epoch = number("epoch");
        row.far_session = number("session");
        row.established = text("established");
        row.kind = text("kind");
        row.attempt = number("attempt");
        row.far_sender = number("far_sender");
        row.far_role = text("far_role");
        row.far_shape = text("shape");
        row.far_version = number("version");
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
