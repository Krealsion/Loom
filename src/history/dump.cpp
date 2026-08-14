// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/history/dump.hpp>

#include <ostream>

namespace loom {

namespace {

/// Bytes as hex, because a payload is arbitrary octets and a dump must not
/// pretend otherwise. Deliberately not base64 and deliberately not "decoded
/// fields": this is a witness that the bytes are there, not a value browser.
std::string hex(const std::string& bytes, std::size_t limit) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    const std::size_t n = limit == 0 || bytes.size() < limit ? bytes.size() : limit;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0x0F]);
    }
    if (n < bytes.size()) {
        out += "... (" + std::to_string(bytes.size()) + " bytes)";
    }
    return out;
}

} // namespace

std::string render_record(const HistoryRecord& rec) {
    // A RECORD NOBODY NUMBERED IS NOT WEARING A RECORDER'S IDENTITY. A Logger's
    // copy of a fact carries `record_seq == 0` and an empty `held` mask, both
    // honestly — so printing `#0 [none]` in front of it would dress a durable
    // record as a live one that had been forgotten by every window.
    std::string line;
    if (rec.record_seq != 0) {
        line = "#" + std::to_string(rec.record_seq) + " [" + describe_held(rec.held) + "] ";
    }
    if (rec.kind == RecordKind::RecorderPolicy) {
        line += "policy: " + rec.note;
        return line;
    }
    line += rec.seq != 0 ? "seq=" + std::to_string(rec.seq) : std::string("seq=-");
    if (rec.kind == RecordKind::Lifecycle) {
        line += " lifecycle weave#" + std::to_string(rec.target.value);
        line += rec.from_last_known_good ? " (from last known good)" : "";
        line += " state=" + rec.shape + " v" + std::to_string(rec.shape_version);
        if (rec.refusal != RefusalReason::None) {
            line += " refusal=" + std::string(name_of(rec.refusal));
        }
        return line;
    }
    line += " #" + std::to_string(rec.sender.value) + " -> #" +
            std::to_string(rec.target.value);
    if (!rec.addressed_role.empty()) {
        line += " @" + rec.addressed_role;
    }
    if (!rec.authored_role.empty()) {
        line += " as:" + rec.authored_role;
    }
    line += " " + rec.shape + " v" + std::to_string(rec.shape_version);
    line += " " + std::string(name_of(rec.outcome));
    if (rec.refusal != RefusalReason::None) {
        line += "(" + std::string(name_of(rec.refusal));
        if (!rec.refusal_detail.empty()) {
            line += ": " + rec.refusal_detail;
        }
        line += ")";
    }
    if (rec.correlation != 0) {
        line += " corr=" + std::to_string(rec.correlation);
    }
    if (rec.dispatch_parent != 0) {
        line += " from-dispatch=" + std::to_string(rec.dispatch_parent);
    }
    if (rec.handler_elapsed_ns != 0) {
        line += " handler=" + std::to_string(rec.handler_elapsed_ns) + "ns";
    }
    line += " payload=" + std::string(name_of(rec.payload));
    if (rec.payload_bytes != 0) {
        line += "/" + std::to_string(rec.payload_bytes) + "B";
    }
    return line;
}

namespace {

void dump_body(const std::vector<HistoryRecord>& records, std::ostream& out,
               const DumpOptions& opts, const Recorder* live) {
    std::size_t first = 0;
    if (opts.limit != 0 && records.size() > opts.limit) {
        first = records.size() - opts.limit;
        out << "  (" << first << " older retained records not shown)\n";
    }
    for (std::size_t i = first; i < records.size(); ++i) {
        const HistoryRecord& rec = records[i];
        out << "  " << render_record(rec) << "\n";
        if (!opts.payloads || live == nullptr) {
            continue;
        }
        const PayloadLookup p = live->payload(rec.record_seq);
        // The payload's own state is printed even when there are no bytes: "I had
        // it and let it go" is exactly the answer a reader most needs and is the
        // one a blank line would hide.
        out << "      payload " << name_of(p.state);
        if (p.state == PayloadState::Retained) {
            out << " " << p.shape << " v" << p.shape_version << " " << hex(p.bytes, 64);
        }
        out << "\n";
    }
}

} // namespace

void dump_history(const Recorder& rec, std::ostream& out, const DumpOptions& opts) {
    const RecorderBounds b = rec.bounds();
    const RecorderCounters c = rec.counters();
    // THE HORIZON BEFORE THE CONTENT. A reader who saw the records first would
    // have to remember to ask what was missing; this way they cannot not know.
    out << "recorder: " << b.retained << " retained, " << b.forgotten << " forgotten";
    if (b.forgotten != 0) {
        out << " (everything at or below bus seq " << b.forgotten_horizon_seq << ")";
    }
    out << "\n";
    out << "  bus seqs retained " << b.oldest_retained_seq << ".." << b.newest_observed_seq
        << "; observed " << c.observed << ", recorded " << c.recorded << ", declined by policy "
        << c.declined_by_policy << ", declined as recorder-internal " << c.declined_internal
        << "\n";
    out << "  payloads: " << b.payloads_retained << " held (" << b.payload_bytes << " bytes), "
        << b.payloads_forgotten << " forgotten\n";
    // THE THREE WINDOWS, SEPARATELY. A single "retained" number would hide the one
    // thing RTH-1a exists to make visible: that a shape kept out of recent context
    // is still kept.
    out << "  windows: recent " << b.recent_held << "/" << rec.policy().recent_capacity
        << ", protected " << b.protected_held << "/" << rec.policy().protected_capacity
        << ", last-call " << b.last_call_held << " over " << b.shapes_observed
        << " observed shapes\n";
    dump_body(rec.snapshot(), out, opts, &rec);
    if (!opts.tallies) {
        return;
    }
    out << "  by shape:\n";
    for (const ShapeTally& t : rec.tallies()) {
        out << "    " << t.shape << ": observed " << t.observed << ", recorded " << t.recorded
            << ", declined " << t.declined << ", last-call " << t.last_call_held << "\n";
    }
}

void dump_records(const std::vector<HistoryRecord>& records, std::ostream& out,
                  const DumpOptions& opts) {
    out << "records: " << records.size() << "\n";
    dump_body(records, out, opts, nullptr);
}

std::string render_log_record(const LogRecord& rec) {
    // THE ORIGIN FIRST, ALWAYS. A durable stream mixes what the bus said with what
    // the host said, and a line that led with the fact would let a reader take a
    // diagnostic for a message. Nothing here can be read without meeting the
    // discriminant first.
    std::string line = "L" + std::to_string(rec.log_seq) + " " + name_of(rec.origin) + " ";
    switch (rec.origin) {
    case LogOrigin::BusObservation:
        line += render_record(rec.observation);
        break;
    case LogOrigin::Diagnostic:
        line += std::string(name_of(rec.severity)) + " [" + rec.source + "] " + rec.text;
        break;
    case LogOrigin::PolicyChange:
        line += rec.text;
        break;
    }
    return line;
}

void dump_log(const std::vector<LogRecord>& records, std::ostream& out,
              const DumpOptions& opts) {
    out << "log: " << records.size() << " durable records\n";
    std::size_t first = 0;
    if (opts.limit != 0 && records.size() > opts.limit) {
        first = records.size() - opts.limit;
        out << "  (" << first << " older records not shown)\n";
    }
    for (std::size_t i = first; i < records.size(); ++i) {
        out << "  " << render_log_record(records[i]) << "\n";
    }
}

} // namespace loom
