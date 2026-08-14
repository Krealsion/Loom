// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/history/logger.hpp>

#include <zen/admission.hpp>
#include <zen/serialize.hpp>

#include <fstream>
#include <ios>
#include <utility>

namespace loom {

const char* name_of(LogOrigin o) noexcept {
    switch (o) {
    case LogOrigin::BusObservation:
        return "BusObservation";
    case LogOrigin::Diagnostic:
        return "Diagnostic";
    case LogOrigin::PolicyChange:
        return "PolicyChange";
    }
    return "?";
}

const char* name_of(Severity s) noexcept {
    switch (s) {
    case Severity::Info:
        return "Info";
    case Severity::Warning:
        return "Warning";
    case Severity::Error:
        return "Error";
    }
    return "?";
}

namespace {

/// The reverse of `name_of`, for reading a stream back. A SCAN rather than a
/// table, because a table is a second list of the spellings and would go stale the
/// first time one is added; `name_of` is the single owner and this asks it.
RefusalReason refusal_from_name(std::string_view s) noexcept {
    for (unsigned i = 0; i < 64; ++i) {
        const RefusalReason r = static_cast<RefusalReason>(static_cast<std::uint8_t>(i));
        const char* n = name_of(r);
        if (n[0] == '?') {
            break;
        }
        if (s == n) {
            return r;
        }
    }
    return RefusalReason::None;
}

LogOrigin origin_from_name(std::string_view s) noexcept {
    if (s == "Diagnostic") {
        return LogOrigin::Diagnostic;
    }
    if (s == "PolicyChange") {
        return LogOrigin::PolicyChange;
    }
    return LogOrigin::BusObservation;
}

Severity severity_from_name(std::string_view s) noexcept {
    if (s == "Warning") {
        return Severity::Warning;
    }
    if (s == "Error") {
        return Severity::Error;
    }
    return Severity::Info;
}

RecordKind record_kind_from_name(std::string_view s) noexcept {
    if (s == "Lifecycle") {
        return RecordKind::Lifecycle;
    }
    if (s == "RecorderPolicy") {
        return RecordKind::RecorderPolicy;
    }
    return RecordKind::Delivery;
}

RecordedOutcome outcome_from_name(std::string_view s) noexcept {
    if (s == "Delivered") {
        return RecordedOutcome::Delivered;
    }
    if (s == "Refused") {
        return RecordedOutcome::Refused;
    }
    if (s == "HandlerFailed") {
        return RecordedOutcome::HandlerFailed;
    }
    return RecordedOutcome::None;
}

PayloadDisposition payload_from_name(std::string_view s) noexcept {
    if (s == "Retained") {
        return PayloadDisposition::Retained;
    }
    if (s == "TooLarge") {
        return PayloadDisposition::TooLarge;
    }
    if (s == "NotRetained") {
        return PayloadDisposition::NotRetained;
    }
    return PayloadDisposition::None;
}

std::int64_t as_i64(std::uint64_t v) noexcept { return static_cast<std::int64_t>(v); }
std::uint64_t as_u64(std::int64_t v) noexcept { return static_cast<std::uint64_t>(v); }

const std::string& text_at(const Value& v, const char* field) {
    static const std::string kEmpty;
    const Cell* c = v.get(field);
    return c != nullptr && c->is(Kind::Text) ? c->as_text() : kEmpty;
}

std::int64_t int_at(const Value& v, const char* field) {
    const Cell* c = v.get(field);
    return c != nullptr && c->is(Kind::Int) ? c->as_int() : 0;
}

bool bool_at(const Value& v, const char* field) {
    const Cell* c = v.get(field);
    return c != nullptr && c->is(Kind::Bool) && c->as_bool();
}

} // namespace

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

const LogRule* LoggerSelection::rule_for(std::string_view shape) const noexcept {
    for (const LogRule& r : shapes) {
        if (r.shape == shape) {
            return &r;
        }
    }
    return nullptr;
}

LoggerSelection default_selection() {
    LoggerSelection s;
    // WHAT CODE IS LOADED. The control door's four verbs that CHANGE it, and the
    // weave manager's three. Each is measured-rare: RTH-1's live Workshop run saw
    // exactly three LoadLibrary deliveries in a whole session, all at boot.
    for (const char* shape : {"LoadLibrary", "ReloadLibrary", "UnloadLibrary", "UnloadRole",
                              "zen.LoadWeave", "zen.SwapWeave", "zen.ReloadWeave"}) {
        s.shapes.push_back(LogRule{shape, 0});
    }
    // WHO MAY SPEAK. The Weaver's authority conversation — an ask, its two answers,
    // a revocation, and the grant that resulted. Rare by construction: a grant is
    // not a lease and is not renewed on a timer.
    for (const char* shape : {"zen.RequestAuthority", "zen.ApproveAuthority",
                              "zen.RefuseAuthority", "zen.RevokeAuthority",
                              "zen.AuthorityGranted"}) {
        s.shapes.push_back(LogRule{shape, 0});
    }
    return s;
}

// ---------------------------------------------------------------------------
// The durable stream
// ---------------------------------------------------------------------------

/// AN ORDINARY FILE HANDLE, and that is the whole of it. A record is appended on
/// the dispatch that produced it, and closing flushes.
///
/// THE CONTAINER IS A STREAM OF TOP-LEVEL VALUES, `[u32 len][bytes]`, which is
/// Arena's shape and is chosen for Arena's reason: `kMaxDecodedCells` bounds a
/// SINGLE decode, so a whole stream as one nested document could not be read back
/// at all. Each record is canonical native bytes and is re-admitted through the one
/// gate on read, so a corrupt or forged file is refused rather than trusted.
struct Logger::Stream {
    std::ofstream out;
};

const std::shared_ptr<const Schema>& Logger::record_schema() {
    static const std::shared_ptr<const Schema> schema =
        SchemaBuilder("zen.history.LogRecord", 1)
            // ---- the discriminant, and what only a non-bus record carries ------
            .field("log_seq", Kind::Int)
            .field("origin", Kind::Text)
            .field("severity", Kind::Text)
            .field("source", Kind::Text)
            .field("text", Kind::Text)
            // ---- the observation, meaningful iff origin == BusObservation ------
            .field("kind", Kind::Text)
            .field("held", Kind::Int)
            .field("seq", Kind::Int)
            .field("sender", Kind::Int)
            .field("target", Kind::Int)
            .field("addressed_role", Kind::Text)
            .field("authored_role", Kind::Text)
            .field("shape", Kind::Text)
            .field("shape_version", Kind::Int)
            .field("correlation", Kind::Int)
            .field("dispatch_parent", Kind::Int)
            .field("handler_elapsed_ns", Kind::Int)
            .field("outcome", Kind::Text)
            .field("refusal", Kind::Text)
            .field("refusal_detail", Kind::Text)
            .field("from_last_known_good", Kind::Bool)
            .field("note", Kind::Text)
            .field("payload", Kind::Text)
            .field("payload_bytes", Kind::Int)
            .field("payload_body", Kind::Bytes)
            .build();
    return schema;
}

namespace {

Value to_value(const LogRecord& r) {
    const std::string& payload_body = r.payload_body;
    const HistoryRecord& o = r.observation;
    Value v(Logger::record_schema());
    v.set("log_seq", Cell::integer(as_i64(r.log_seq)));
    v.set("origin", Cell::text(name_of(r.origin)));
    v.set("severity", Cell::text(name_of(r.severity)));
    v.set("source", Cell::text(r.source));
    v.set("text", Cell::text(r.text));
    v.set("kind", Cell::text(name_of(o.kind)));
    v.set("held", Cell::integer(static_cast<std::int64_t>(o.held)));
    v.set("seq", Cell::integer(as_i64(o.seq)));
    v.set("sender", Cell::integer(as_i64(o.sender.value)));
    v.set("target", Cell::integer(as_i64(o.target.value)));
    v.set("addressed_role", Cell::text(o.addressed_role));
    v.set("authored_role", Cell::text(o.authored_role));
    v.set("shape", Cell::text(o.shape));
    v.set("shape_version", Cell::integer(static_cast<std::int64_t>(o.shape_version)));
    v.set("correlation", Cell::integer(as_i64(o.correlation)));
    v.set("dispatch_parent", Cell::integer(as_i64(o.dispatch_parent)));
    v.set("handler_elapsed_ns", Cell::integer(as_i64(o.handler_elapsed_ns)));
    v.set("outcome", Cell::text(name_of(o.outcome)));
    v.set("refusal", Cell::text(name_of(o.refusal)));
    v.set("refusal_detail", Cell::text(o.refusal_detail));
    v.set("from_last_known_good", Cell::boolean(o.from_last_known_good));
    v.set("note", Cell::text(o.note));
    v.set("payload", Cell::text(name_of(o.payload)));
    v.set("payload_bytes", Cell::integer(static_cast<std::int64_t>(o.payload_bytes)));
    v.set("payload_body", Cell::bytes(Bytes(payload_body.begin(), payload_body.end())));
    return v;
}

LogRecord from_value(const Value& v) {
    LogRecord r;
    r.log_seq = as_u64(int_at(v, "log_seq"));
    r.origin = origin_from_name(text_at(v, "origin"));
    r.severity = severity_from_name(text_at(v, "severity"));
    r.source = text_at(v, "source");
    r.text = text_at(v, "text");
    HistoryRecord& o = r.observation;
    o.kind = record_kind_from_name(text_at(v, "kind"));
    o.held = static_cast<HeldMask>(int_at(v, "held"));
    o.seq = as_u64(int_at(v, "seq"));
    o.sender = WeaveId{as_u64(int_at(v, "sender"))};
    o.target = WeaveId{as_u64(int_at(v, "target"))};
    o.addressed_role = text_at(v, "addressed_role");
    o.authored_role = text_at(v, "authored_role");
    o.shape = text_at(v, "shape");
    o.shape_version = static_cast<std::uint32_t>(int_at(v, "shape_version"));
    o.correlation = as_u64(int_at(v, "correlation"));
    o.dispatch_parent = as_u64(int_at(v, "dispatch_parent"));
    o.handler_elapsed_ns = as_u64(int_at(v, "handler_elapsed_ns"));
    o.outcome = outcome_from_name(text_at(v, "outcome"));
    o.refusal = refusal_from_name(text_at(v, "refusal"));
    o.refusal_detail = text_at(v, "refusal_detail");
    o.from_last_known_good = bool_at(v, "from_last_known_good");
    o.note = text_at(v, "note");
    o.payload = payload_from_name(text_at(v, "payload"));
    o.payload_bytes = static_cast<std::uint32_t>(int_at(v, "payload_bytes"));
    if (const Cell* body = v.get("payload_body"); body != nullptr && body->is(Kind::Bytes)) {
        const Bytes& b = body->as_bytes();
        r.payload_body.assign(b.begin(), b.end());
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The logger
// ---------------------------------------------------------------------------

Logger::Logger(Switchboard& bus, LoggerSelection selection)
    : bus_(bus), selection_(std::move(selection)) {
    tap_ = bus_.add_observer([this](const BusEvent& e) { observe(e); });
}

Logger::~Logger() {
    bus_.remove_observer(tap_);
    close();
}

bool Logger::open(const std::string& path, std::string* error) {
    close();
    auto stream = std::make_unique<Stream>();
    stream->out.open(path, std::ios::binary | std::ios::app);
    if (!stream->out) {
        if (error != nullptr) {
            *error = "cannot open history log for append: " + path;
        }
        return false;
    }
    out_ = std::move(stream);
    path_ = path;
    return true;
}

void Logger::close() {
    if (out_) {
        out_->out.flush();
        out_->out.close();
        out_.reset();
    }
}

bool Logger::open() const noexcept { return static_cast<bool>(out_); }

void Logger::observe(const BusEvent& e) {
    ++counters_.observed;
    // THE REJECTION PATH, AND IT IS THE ONE THAT RUNS. Two branch tests and one
    // map lookup keyed on a name the event already holds; no record is built, no
    // string is copied, nothing is serialized and nothing touches a disk. That is
    // the whole cost of a Logger for the traffic it does not want.
    const bool structural =
        (selection_.log_handler_failures && e.kind == EventKind::HandlerFailed) ||
        (selection_.log_lifecycle &&
         (e.kind == EventKind::Died || e.kind == EventKind::Revived)) ||
        (selection_.log_refusals && e.kind == EventKind::Refused);
    const LogRule* rule = nullptr;
    if (!structural) {
        rule = selection_.rule_for(e.schema_name);
        if (rule == nullptr) {
            return;
        }
    }
    ++counters_.selected;
    if (!out_) {
        return; // selected, and nowhere to put it; counted, never pretended
    }
    // A PER-SHAPE CAP, NOT A GLOBAL ONE. A shape that reaches its own cap stops and
    // says so, once — and cannot consume the horizon of a shape that has none.
    if (rule != nullptr && rule->cap > 0) {
        std::uint64_t& n = appended_[e.schema_name];
        if (n >= rule->cap) {
            ++counters_.capped;
            return;
        }
        if (n + 1 == rule->cap) {
            // Ordered so the note follows the record it is about.
            LogRecord rec;
            rec.origin = LogOrigin::BusObservation;
            capture(rec, e);
            ++n;
            append(rec);
            note("cap reached: " + e.schema_name + " at " + std::to_string(rule->cap) +
                 " records; further ones are declined and counted");
            return;
        }
        ++n;
    } else {
        ++appended_[e.schema_name];
    }

    LogRecord rec;
    rec.origin = LogOrigin::BusObservation;
    // CAPTURED FROM THE OBSERVATION, ON THIS STACK. Not looked up in a Recorder
    // afterwards — a durable fact must not depend on a volatile window still
    // holding its copy. The payload especially: `BusEvent::payload` points into a
    // Message that dies when the delivery returns, so the bytes are taken here or
    // not at all.
    capture(rec, e);
    append(rec);
}

void Logger::capture(LogRecord& rec, const BusEvent& e) {
    fill_from_event(rec.observation, e);
    if (e.payload == nullptr) {
        return; // every refusal, every lifecycle transition
    }
    try {
        rec.payload_body = serialize(*e.payload);
    } catch (const std::exception&) {
        rec.observation.payload = PayloadDisposition::NotRetained;
        return;
    }
    rec.observation.payload_bytes = static_cast<std::uint32_t>(rec.payload_body.size());
    rec.observation.payload = PayloadDisposition::Retained;
}

bool Logger::append(const LogRecord& rec) {
    if (!out_) {
        return false;
    }
    LogRecord numbered = rec;
    numbered.log_seq = next_log_seq_;
    std::string bytes;
    try {
        bytes = serialize(to_value(numbered));
    } catch (const std::exception&) {
        // A record that will not serialize is a defect here, not in the traffic; it
        // is counted and the stream carries on rather than throwing out of a tap
        // callback and into somebody else's dispatch.
        ++counters_.unwritable;
        return false;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(bytes.size());
    const char header[4] = {static_cast<char>(n & 0xFFu), static_cast<char>((n >> 8) & 0xFFu),
                            static_cast<char>((n >> 16) & 0xFFu),
                            static_cast<char>((n >> 24) & 0xFFu)};
    out_->out.write(header, 4);
    out_->out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out_->out) {
        ++counters_.unwritable;
        return false;
    }
    ++next_log_seq_;
    ++counters_.appended;
    counters_.bytes += bytes.size() + 4;
    return true;
}

void Logger::note(std::string text) {
    LogRecord rec;
    rec.origin = LogOrigin::PolicyChange;
    rec.severity = Severity::Info;
    rec.source = "zen.history.Logger";
    rec.text = std::move(text);
    append(rec);
    if (out_) {
        out_->out.flush();
    }
}

void Logger::select(LoggerSelection next) {
    std::string change;
    const auto add = [&change](const std::string& s) {
        if (!change.empty()) {
            change += "; ";
        }
        change += s;
    };
    const auto flag = [&add](const char* name, bool was, bool now) {
        if (was != now) {
            add(std::string(name) + (now ? ": on" : ": off"));
        }
    };
    flag("handler failures", selection_.log_handler_failures, next.log_handler_failures);
    flag("lifecycle", selection_.log_lifecycle, next.log_lifecycle);
    flag("refusals", selection_.log_refusals, next.log_refusals);
    for (const LogRule& r : next.shapes) {
        const LogRule* before = selection_.rule_for(r.shape);
        const std::string cap = r.cap == 0 ? "uncapped" : std::to_string(r.cap);
        if (before == nullptr) {
            add("+" + r.shape + " (" + cap + ")");
        } else if (before->cap != r.cap) {
            add(r.shape + ": cap " +
                (before->cap == 0 ? std::string("uncapped") : std::to_string(before->cap)) +
                " -> " + cap);
        }
    }
    for (const LogRule& r : selection_.shapes) {
        if (next.rule_for(r.shape) == nullptr) {
            add("-" + r.shape);
        }
    }
    selection_ = std::move(next);
    // A MEANINGFUL CHANGE TO WHAT IS KEPT IS ITSELF KEPT. Written directly to the
    // stream, with nothing published: a logger that announced its own policy over
    // the bus would be manufacturing the traffic it exists to select from.
    note(change.empty() ? std::string("selection replaced: no effective change")
                        : "selection: " + change);
}

bool Logger::write(Severity sev, std::string_view source, std::string_view text) {
    LogRecord rec;
    rec.origin = LogOrigin::Diagnostic;
    rec.severity = sev;
    rec.source = std::string(source);
    rec.text = std::string(text);
    ++counters_.diagnostics;
    const bool ok = append(rec);
    if (ok && out_) {
        // A diagnostic is what a host writes when something is going wrong, so it
        // is flushed rather than left in a buffer a crash would discard. Ordinary
        // observations are not: they are frequent and the stream is closed cleanly.
        out_->out.flush();
    }
    return ok;
}

std::uint64_t Logger::appended_of(std::string_view shape) const noexcept {
    const auto it = appended_.find(shape);
    return it != appended_.end() ? it->second : 0;
}

bool Logger::read(const std::string& path, std::vector<LogRecord>* out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "cannot open history log for reading: " + path;
        }
        return false;
    }
    while (true) {
        char header[4];
        in.read(header, 4);
        if (in.gcount() == 0) {
            break; // clean end
        }
        if (in.gcount() != 4) {
            if (error != nullptr) {
                *error = "truncated record header";
            }
            return false;
        }
        const std::uint32_t n =
            static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[3])) << 24);
        std::string bytes(n, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(n));
        if (static_cast<std::uint32_t>(in.gcount()) != n) {
            if (error != nullptr) {
                *error = "truncated record body";
            }
            return false;
        }
        // THROUGH THE ONE GATE, exactly as Arena re-admits every replay record: a
        // log is bytes on a disk anybody can write, so it is admitted rather than
        // believed.
        Unverified u = ::loom::parse(bytes);
        Admission a = ::loom::admit(u, record_schema());
        if (!a.ok()) {
            if (error != nullptr) {
                *error = "record refused by the gate: " + a.first_error().message();
            }
            return false;
        }
        if (out != nullptr) {
            out->push_back(from_value(a.value()));
        }
    }
    return true;
}

} // namespace loom
