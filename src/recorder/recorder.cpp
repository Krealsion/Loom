// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/recorder/recorder.hpp>

#include <zen/admission.hpp>
#include <zen/serialize.hpp>

#include <algorithm>
#include <fstream>
#include <ios>
#include <utility>

namespace loom {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

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

const char* name_of(RetentionClass c) noexcept {
    switch (c) {
    case RetentionClass::Shared:
        return "Shared";
    case RetentionClass::Dedicated:
        return "Dedicated";
    case RetentionClass::Protected:
        return "Protected";
    case RetentionClass::NotRetained:
        return "NotRetained";
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

const char* name_of(Horizon h) noexcept {
    switch (h) {
    case Horizon::Retained:
        return "Retained";
    case Horizon::Forgotten:
        return "Forgotten";
    case Horizon::NotRecorded:
        return "NotRecorded";
    case Horizon::Unobserved:
        return "Unobserved";
    }
    return "?";
}

namespace {

/// The reverse of `name_of`, for reading a log back. A SCAN rather than a table,
/// because a table is a second list of the reasons and would go stale the first
/// time one is added; `name_of` is the single owner of the spellings and this
/// asks it. The bound is generous and the enum is contiguous.
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

RetentionClass retention_from_name(std::string_view s) noexcept {
    if (s == "Dedicated") {
        return RetentionClass::Dedicated;
    }
    if (s == "Protected") {
        return RetentionClass::Protected;
    }
    if (s == "NotRetained") {
        return RetentionClass::NotRetained;
    }
    return RetentionClass::Shared;
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
// Policy
// ---------------------------------------------------------------------------

const RetentionRule* RecorderPolicy::rule_for(std::string_view shape) const noexcept {
    for (const RetentionRule& r : rules) {
        if (r.shape == shape) {
            return &r;
        }
    }
    return nullptr;
}

RecorderPolicy default_policy() {
    // Deliberately the struct's own defaults and NOT a curated list of shape
    // rules. The only opinions here are structural (a refusal, a failed handler
    // and a lifecycle transition do not compete with ordinary traffic), and they
    // are the struct's defaults too. A host that wants shape rules writes them.
    return RecorderPolicy{};
}

// ---------------------------------------------------------------------------
// Blacklist
// ---------------------------------------------------------------------------

void RecorderBlacklist::declare_participant(WeaveId id) {
    if (id.valid() && std::find(participants_.begin(), participants_.end(), id) ==
                          participants_.end()) {
        participants_.push_back(id);
    }
}

void RecorderBlacklist::declare_shape(std::string name) {
    if (!name.empty() &&
        std::find(shapes_.begin(), shapes_.end(), name) == shapes_.end()) {
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

// ---------------------------------------------------------------------------
// The persistent log
// ---------------------------------------------------------------------------

/// AN ORDINARY FILE HANDLE, and that is the whole of it. No scheduler, no
/// background thread, no idle detection, no compaction, no rotation. A record is
/// appended on the dispatch that produced it, and closing flushes.
///
/// THE CONTAINER IS A STREAM OF TOP-LEVEL VALUES, `[u32 len][bytes]`, which is
/// Arena's shape and is chosen for Arena's reason: `kMaxDecodedCells` bounds a
/// SINGLE decode, so a whole log as one nested document could not be read back
/// at all. Each record is canonical native bytes and is re-admitted through the
/// one gate on read, so a corrupt or forged log is refused rather than trusted.
struct Recorder::LogFile {
    std::ofstream out;
    std::uint64_t bytes = 0;
    bool stopped = false; ///< the budget was reached; the recorder said so, once
};

const std::shared_ptr<const Schema>& Recorder::record_schema() {
    static const std::shared_ptr<const Schema> schema =
        SchemaBuilder("zen.recorder.Record", 1)
            .field("record_seq", Kind::Int)
            .field("kind", Kind::Text)
            .field("retention", Kind::Text)
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

Value to_value(const HistoryRecord& r, const std::string& payload_body) {
    Value v(Recorder::record_schema());
    v.set("record_seq", Cell::integer(as_i64(r.record_seq)));
    v.set("kind", Cell::text(name_of(r.kind)));
    v.set("retention", Cell::text(name_of(r.retention)));
    v.set("seq", Cell::integer(as_i64(r.seq)));
    v.set("sender", Cell::integer(as_i64(r.sender.value)));
    v.set("target", Cell::integer(as_i64(r.target.value)));
    v.set("addressed_role", Cell::text(r.addressed_role));
    v.set("authored_role", Cell::text(r.authored_role));
    v.set("shape", Cell::text(r.shape));
    v.set("shape_version", Cell::integer(static_cast<std::int64_t>(r.shape_version)));
    v.set("correlation", Cell::integer(as_i64(r.correlation)));
    v.set("dispatch_parent", Cell::integer(as_i64(r.dispatch_parent)));
    v.set("handler_elapsed_ns", Cell::integer(as_i64(r.handler_elapsed_ns)));
    v.set("outcome", Cell::text(name_of(r.outcome)));
    v.set("refusal", Cell::text(name_of(r.refusal)));
    v.set("refusal_detail", Cell::text(r.refusal_detail));
    v.set("from_last_known_good", Cell::boolean(r.from_last_known_good));
    v.set("note", Cell::text(r.note));
    v.set("payload", Cell::text(name_of(r.payload)));
    v.set("payload_bytes", Cell::integer(static_cast<std::int64_t>(r.payload_bytes)));
    v.set("payload_body",
          Cell::bytes(Bytes(payload_body.begin(), payload_body.end())));
    return v;
}

HistoryRecord from_value(const Value& v) {
    HistoryRecord r;
    r.record_seq = as_u64(int_at(v, "record_seq"));
    r.kind = record_kind_from_name(text_at(v, "kind"));
    r.retention = retention_from_name(text_at(v, "retention"));
    r.seq = as_u64(int_at(v, "seq"));
    r.sender = WeaveId{as_u64(int_at(v, "sender"))};
    r.target = WeaveId{as_u64(int_at(v, "target"))};
    r.addressed_role = text_at(v, "addressed_role");
    r.authored_role = text_at(v, "authored_role");
    r.shape = text_at(v, "shape");
    r.shape_version = static_cast<std::uint32_t>(int_at(v, "shape_version"));
    r.correlation = as_u64(int_at(v, "correlation"));
    r.dispatch_parent = as_u64(int_at(v, "dispatch_parent"));
    r.handler_elapsed_ns = as_u64(int_at(v, "handler_elapsed_ns"));
    r.outcome = outcome_from_name(text_at(v, "outcome"));
    r.refusal = refusal_from_name(text_at(v, "refusal"));
    r.refusal_detail = text_at(v, "refusal_detail");
    r.from_last_known_good = bool_at(v, "from_last_known_good");
    r.note = text_at(v, "note");
    r.payload = payload_from_name(text_at(v, "payload"));
    r.payload_bytes = static_cast<std::uint32_t>(int_at(v, "payload_bytes"));
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

Recorder::Recorder(Switchboard& bus, RecorderPolicy policy)
    : bus_(bus), policy_(std::move(policy)) {
    shared_.capacity = policy_.shared_capacity;
    protected_.capacity = policy_.protected_capacity;
    tap_ = bus_.add_observer([this](const BusEvent& e) { observe(e); });
}

Recorder::~Recorder() {
    // Stop the callback before the members it captures die — the console's own
    // idiom, and the reason MSG-11 honours a removal made during a notification.
    bus_.remove_observer(tap_);
    close_log();
}

void Recorder::observe(const BusEvent& e) {
    ++counters_.observed;
    if (e.seq > newest_observed_seq_) {
        newest_observed_seq_ = e.seq;
    }
    // THE STRUCTURAL CLASS FIRST, before any counter that could be read as
    // traffic and before any policy decision. Recorder machinery is not a fact
    // about the system; it never enters the recordable universe, so there is
    // nothing for a rule to have an opinion about.
    if (blacklist_.excludes(e)) {
        ++counters_.declined_internal;
        return;
    }

    ShapeTally& tally = tallies_[e.schema_name];
    if (tally.shape.empty()) {
        tally.shape = e.schema_name;
    }
    ++tally.observed;

    HistoryRecord rec;
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

    bool retain_payload = true;
    rec.retention = classify(e, &retain_payload);
    if (rec.retention == RetentionClass::NotRetained) {
        ++counters_.declined_by_policy;
        ++tally.declined;
        return;
    }
    ++tally.recorded;
    // FOUR DISPOSITIONS, AND THEY ARE NOT THE SAME FACT. An event that carried no
    // payload at all (every refusal, every lifecycle transition) is `None`; one
    // whose payload policy declined is `NotRetained`; the difference is exactly
    // what a reader needs to tell "there was nothing to keep" from "I chose not
    // to keep it".
    const bool has_body = e.payload != nullptr;
    if (has_body && !retain_payload) {
        rec.payload = PayloadDisposition::NotRetained;
    }
    admit(std::move(rec), has_body && retain_payload ? &e : nullptr);
}

RetentionClass Recorder::classify(const BusEvent& e, bool* retain_payload) const {
    const RetentionRule* rule = policy_.rule_for(e.schema_name);
    *retain_payload = rule != nullptr ? rule->retain_payload : policy_.default_retain_payload;

    // STRUCTURAL PROTECTION OUTRANKS A SHAPE RULE. See RecorderPolicy: a rule
    // saying "not worth remembering" is about a shape's ordinary traffic, and a
    // refusal, a failed handler and a lifecycle transition are not that.
    if (policy_.protect_refusals && e.kind == EventKind::Refused) {
        return RetentionClass::Protected;
    }
    if (policy_.protect_handler_failures && e.kind == EventKind::HandlerFailed) {
        return RetentionClass::Protected;
    }
    if (policy_.protect_lifecycle &&
        (e.kind == EventKind::Died || e.kind == EventKind::Revived)) {
        return RetentionClass::Protected;
    }
    if (rule != nullptr) {
        return rule->klass;
    }
    return policy_.default_class;
}

Recorder::Window& Recorder::window_for(const HistoryRecord& rec) {
    if (rec.retention == RetentionClass::Protected) {
        return protected_;
    }
    if (rec.retention == RetentionClass::Dedicated) {
        Window& w = dedicated_[rec.shape];
        if (w.capacity == 0) {
            const RetentionRule* rule = policy_.rule_for(rec.shape);
            w.capacity = rule != nullptr ? rule->capacity : kDefaultDedicatedCapacity;
        }
        return w;
    }
    return shared_;
}

void Recorder::admit(HistoryRecord rec, const BusEvent* e) {
    rec.record_seq = next_record_seq_++;

    // THE PAYLOAD IS COPIED INSIDE THE CALLBACK OR NOT AT ALL. `BusEvent::payload`
    // points into a Message that dies when the delivery returns, so the bytes are
    // taken here, on this stack, and the pointer is never stored.
    std::string body;
    if (e != nullptr && e->payload != nullptr) {
        body = serialize(*e->payload);
        rec.payload_bytes = static_cast<std::uint32_t>(body.size());
        rec.payload = body.size() > policy_.max_payload_bytes ? PayloadDisposition::TooLarge
                                                              : PayloadDisposition::Retained;
    }
    // Otherwise the caller's disposition stands: `None` for an event that carried
    // nothing, `NotRetained` where policy declined it.

    const std::uint64_t record_seq = rec.record_seq;
    const std::string shape = rec.shape;
    const std::uint32_t version = rec.shape_version;
    const bool keep_body = rec.payload == PayloadDisposition::Retained;

    Window& w = window_for(rec);
    if (w.capacity == 0) {
        w.capacity = policy_.shared_capacity;
    }
    trim(w);
    append_to_log(rec, keep_body ? body : std::string{});
    w.records.push_back(std::move(rec));
    ++counters_.recorded;

    if (keep_body) {
        store_payload(record_seq, std::move(body), shape, version);
    }
    if (log_stop_pending_) {
        // Recorded AFTER the record that hit the budget, so the note reads as the
        // consequence it is. Deliberately not from inside the writer: a log entry
        // that could cause another log entry is the recursion this design refuses
        // everywhere else.
        log_stop_pending_ = false;
        note_log_stopped();
    }
}

void Recorder::trim(Window& w) {
    // Room for one more. A capacity a policy change LOWERED is honoured here and
    // only here, which is what makes the change prospective: applying a policy
    // destroys nothing, and the excess drains as new traffic arrives.
    while (!w.records.empty() && w.records.size() >= w.capacity) {
        release(w.records.front());
        ++w.evicted;
        w.records.pop_front();
    }
}

void Recorder::release(const HistoryRecord& rec) {
    ++forgotten_;
    if (rec.seq > forgotten_horizon_seq_) {
        forgotten_horizon_seq_ = rec.seq;
    }
    // A payload whose metadata is gone is unreachable — nothing can name it — so
    // it goes with the record rather than sitting in the budget forever. The
    // reverse is NOT true and must not be: a payload evicted by the byte budget
    // leaves its metadata exactly where it was.
    for (auto it = payloads_.begin(); it != payloads_.end(); ++it) {
        if (it->record_seq == rec.record_seq) {
            payload_bytes_ -= it->bytes.size();
            ++payloads_forgotten_;
            if (it->record_seq > payload_horizon_) {
                payload_horizon_ = it->record_seq;
            }
            payloads_.erase(it);
            break;
        }
    }
}

void Recorder::store_payload(std::uint64_t record_seq, std::string bytes, std::string shape,
                             std::uint32_t version) {
    payload_bytes_ += bytes.size();
    payloads_.push_back(PayloadSlot{record_seq, std::move(bytes), std::move(shape), version});
    trim_payloads();
}

void Recorder::trim_payloads() {
    // A BYTE BUDGET, not an entry count — the two rank Zen's traffic differently.
    // RTH-0 measured an idle application's noise at 31-47 bytes a message and one
    // interactive SurfaceCanvas at up to 2.75 KiB, so a budget in entries bounds
    // the wrong thing. The newest payload is always kept even when it alone
    // exceeds the budget: refusing it would make the budget a second, silent
    // per-payload ceiling, and there already is one.
    while (payloads_.size() > 1 && payload_bytes_ > policy_.payload_byte_budget) {
        PayloadSlot& oldest = payloads_.front();
        payload_bytes_ -= oldest.bytes.size();
        ++payloads_forgotten_;
        if (oldest.record_seq > payload_horizon_) {
            payload_horizon_ = oldest.record_seq;
        }
        payloads_.pop_front();
    }
}

// ---------------------------------------------------------------------------
// Policy changes
// ---------------------------------------------------------------------------

namespace {

std::string describe_change(const RecorderPolicy& was, const RecorderPolicy& now,
                            std::size_t over_bound) {
    std::string note;
    const auto add = [&note](const std::string& s) {
        if (!note.empty()) {
            note += "; ";
        }
        note += s;
    };
    if (was.shared_capacity != now.shared_capacity) {
        add("shared " + std::to_string(was.shared_capacity) + " -> " +
            std::to_string(now.shared_capacity));
    }
    if (was.protected_capacity != now.protected_capacity) {
        add("protected " + std::to_string(was.protected_capacity) + " -> " +
            std::to_string(now.protected_capacity));
    }
    if (was.payload_byte_budget != now.payload_byte_budget) {
        add("payload budget " + std::to_string(was.payload_byte_budget) + " -> " +
            std::to_string(now.payload_byte_budget) + " bytes");
    }
    if (was.max_payload_bytes != now.max_payload_bytes) {
        add("payload ceiling " + std::to_string(was.max_payload_bytes) + " -> " +
            std::to_string(now.max_payload_bytes) + " bytes");
    }
    // Rules, by shape, both directions. The vocabulary is deliberately small:
    // which shape, which class it left, which it joined.
    for (const RetentionRule& r : now.rules) {
        const RetentionRule* before = was.rule_for(r.shape);
        const RetentionClass from = before != nullptr ? before->klass : was.default_class;
        const bool payload_before =
            before != nullptr ? before->retain_payload : was.default_retain_payload;
        if (before == nullptr || before->klass != r.klass || before->capacity != r.capacity ||
            payload_before != r.retain_payload) {
            std::string s = r.shape + ": " + name_of(from) + " -> " + name_of(r.klass);
            if (r.klass == RetentionClass::Dedicated) {
                s += "(" + std::to_string(r.capacity) + ")";
            }
            // A RULE THAT CHANGED ONLY ITS PAYLOAD APPETITE STILL CHANGED WHAT
            // ZEN REMEMBERS. Reporting the class alone would print
            // `Shared -> Shared` and read as a change that was not one.
            if (payload_before != r.retain_payload) {
                s += r.retain_payload ? ", payloads kept" : ", payloads dropped";
            }
            add(s);
        }
    }
    for (const RetentionRule& r : was.rules) {
        if (now.rule_for(r.shape) == nullptr) {
            add(r.shape + ": " + name_of(r.klass) + " -> " + name_of(now.default_class));
        }
    }
    if (note.empty()) {
        note = "no effective change";
    }
    if (over_bound > 0) {
        // STATED, NOT DISCOVERED. The change destroyed nothing; this many records
        // are above the new bound and will be released as new traffic arrives.
        note += " [" + std::to_string(over_bound) + " retained above the new bound]";
    }
    return note;
}

} // namespace

void Recorder::apply_policy(RecorderPolicy next) {
    std::size_t over = 0;
    const auto over_by = [](const Window& w, std::size_t cap) {
        return w.records.size() > cap ? w.records.size() - cap : std::size_t{0};
    };
    over += over_by(shared_, next.shared_capacity);
    over += over_by(protected_, next.protected_capacity);
    for (const auto& entry : dedicated_) {
        const RetentionRule* rule = next.rule_for(entry.first);
        over += over_by(entry.second,
                        rule != nullptr ? rule->capacity : kDefaultDedicatedCapacity);
    }

    HistoryRecord rec;
    rec.kind = RecordKind::RecorderPolicy;
    // ALWAYS PROTECTED, and not a policy question. A recorder that could be told
    // to forget being told to forget would be able to lose the one fact that
    // explains every other absence.
    rec.retention = RetentionClass::Protected;
    rec.shape = "zen.recorder.PolicyChanged";
    rec.shape_version = 1;
    rec.note = describe_change(policy_, next, over);
    rec.payload = PayloadDisposition::None;

    policy_ = std::move(next);
    shared_.capacity = policy_.shared_capacity;
    protected_.capacity = policy_.protected_capacity;
    for (auto& entry : dedicated_) {
        const RetentionRule* rule = policy_.rule_for(entry.first);
        entry.second.capacity =
            rule != nullptr ? rule->capacity : kDefaultDedicatedCapacity;
    }
    // Recorded through the ordinary door, so a policy change is subject to the
    // same bound as everything else in its window — and NOT through the bus,
    // which is the whole point: nothing was published, so nothing observed it,
    // so there is no recursion to guard against.
    admit(std::move(rec), nullptr);
    // ...and the payload budget may itself have shrunk.
    trim_payloads();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool Recorder::open_log(const std::string& path, std::string* error) {
    close_log();
    auto file = std::make_unique<LogFile>();
    file->out.open(path, std::ios::binary | std::ios::app);
    if (!file->out) {
        if (error != nullptr) {
            *error = "cannot open recorder log for append: " + path;
        }
        return false;
    }
    log_ = std::move(file);
    log_path_ = path;
    return true;
}

void Recorder::close_log() {
    if (log_) {
        log_->out.flush();
        log_->out.close();
        log_.reset();
    }
}

bool Recorder::logging() const noexcept { return static_cast<bool>(log_) && !log_->stopped; }

void Recorder::write_frame(const HistoryRecord& rec, const std::string& body) {
    std::string bytes;
    try {
        bytes = serialize(to_value(rec, body));
    } catch (const std::exception&) {
        // A record that will not serialize is a defect in this recorder, not in
        // the traffic; it is counted and the log carries on rather than throwing
        // out of a tap callback and into somebody else's dispatch.
        ++counters_.log_refused;
        return;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(bytes.size());
    const char header[4] = {static_cast<char>(n & 0xFFu), static_cast<char>((n >> 8) & 0xFFu),
                            static_cast<char>((n >> 16) & 0xFFu),
                            static_cast<char>((n >> 24) & 0xFFu)};
    log_->out.write(header, 4);
    log_->out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    log_->bytes += bytes.size() + 4;
    ++counters_.log_records;
    counters_.log_bytes = log_->bytes;
}

void Recorder::append_to_log(const HistoryRecord& rec, const std::string& body) {
    if (!log_) {
        return;
    }
    if (log_->stopped) {
        ++counters_.log_refused;
        return;
    }
    // BOUNDED, AND IT SAYS SO. A file that grows without limit inside a process
    // that runs for weeks is the hazard the journal's own comment names, and a
    // budget that stopped writing without a word would be the observability lie
    // the same doctrine forbids. So the estimate is taken from the record itself
    // and the stop becomes a fact (`note_log_stopped`), once.
    std::string bytes;
    try {
        bytes = serialize(to_value(rec, body));
    } catch (const std::exception&) {
        ++counters_.log_refused;
        return;
    }
    if (log_->bytes + bytes.size() + 4 > policy_.log_byte_budget) {
        log_->stopped = true;
        log_stop_pending_ = true;
        ++counters_.log_refused;
        return;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(bytes.size());
    const char header[4] = {static_cast<char>(n & 0xFFu), static_cast<char>((n >> 8) & 0xFFu),
                            static_cast<char>((n >> 16) & 0xFFu),
                            static_cast<char>((n >> 24) & 0xFFu)};
    log_->out.write(header, 4);
    log_->out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    log_->bytes += bytes.size() + 4;
    ++counters_.log_records;
    counters_.log_bytes = log_->bytes;
}

void Recorder::note_log_stopped() {
    HistoryRecord stop;
    stop.record_seq = next_record_seq_++;
    stop.kind = RecordKind::RecorderPolicy;
    stop.retention = RetentionClass::Protected;
    stop.shape = "zen.recorder.PolicyChanged";
    stop.shape_version = 1;
    stop.note = "persistence stopped: log budget of " +
                std::to_string(policy_.log_byte_budget) + " bytes reached after " +
                std::to_string(counters_.log_records) + " records";
    stop.payload = PayloadDisposition::None;
    // THE LOG'S OWN LAST WORD, written past the budget by exactly one record. A
    // log that simply ended would leave a reader unable to tell a stopped
    // recorder from a killed process, which is the distinction this whole phase
    // exists to keep.
    write_frame(stop, std::string{});
    log_->out.flush();
    trim(protected_);
    protected_.records.push_back(std::move(stop));
    ++counters_.recorded;
}

bool Recorder::read_log(const std::string& path, std::vector<HistoryRecord>* out,
                        std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "cannot open recorder log for reading: " + path;
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
        // THROUGH THE ONE GATE, exactly as Arena re-admits every replay record:
        // a log is bytes on a disk anybody can write, so it is admitted rather
        // than believed.
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

// ---------------------------------------------------------------------------
// The structured reader
// ---------------------------------------------------------------------------

std::vector<HistoryRecord> Recorder::snapshot() const {
    std::vector<HistoryRecord> all;
    all.reserve(retained());
    all.insert(all.end(), shared_.records.begin(), shared_.records.end());
    all.insert(all.end(), protected_.records.begin(), protected_.records.end());
    for (const auto& entry : dedicated_) {
        all.insert(all.end(), entry.second.records.begin(), entry.second.records.end());
    }
    // ONE ORDER, and it is the recorder's own: `record_seq` is monotonic across
    // every window, so merging them cannot reorder anything. A bus `seq` could
    // not do this job — a lifecycle transition has none and a policy change is
    // not a bus fact at all.
    std::sort(all.begin(), all.end(), [](const HistoryRecord& a, const HistoryRecord& b) {
        return a.record_seq < b.record_seq;
    });
    return all;
}

std::vector<HistoryRecord> Recorder::snapshot_of(std::string_view shape) const {
    std::vector<HistoryRecord> out;
    for (const HistoryRecord& r : snapshot()) {
        if (r.shape == shape) {
            out.push_back(r);
        }
    }
    return out;
}

std::size_t Recorder::retained() const noexcept {
    std::size_t n = shared_.records.size() + protected_.records.size();
    for (const auto& entry : dedicated_) {
        n += entry.second.records.size();
    }
    return n;
}

RecorderBounds Recorder::bounds() const noexcept {
    RecorderBounds b;
    b.retained = retained();
    b.forgotten = forgotten_;
    b.forgotten_horizon_seq = forgotten_horizon_seq_;
    b.newest_observed_seq = newest_observed_seq_;
    b.payload_bytes = payload_bytes_;
    b.payloads_retained = payloads_.size();
    b.payloads_forgotten = payloads_forgotten_;
    const auto oldest_in = [&b](const std::deque<HistoryRecord>& d) {
        for (const HistoryRecord& r : d) {
            if (r.seq != 0 && (b.oldest_retained_seq == 0 || r.seq < b.oldest_retained_seq)) {
                b.oldest_retained_seq = r.seq;
            }
        }
    };
    oldest_in(shared_.records);
    oldest_in(protected_.records);
    for (const auto& entry : dedicated_) {
        oldest_in(entry.second.records);
    }
    return b;
}

Lookup Recorder::find(std::uint64_t bus_seq) const noexcept {
    if (bus_seq == 0) {
        return Lookup{Horizon::Unobserved, nullptr};
    }
    const auto scan = [bus_seq](const std::deque<HistoryRecord>& d) -> const HistoryRecord* {
        for (const HistoryRecord& r : d) {
            if (r.seq == bus_seq) {
                return &r;
            }
        }
        return nullptr;
    };
    const HistoryRecord* hit = scan(shared_.records);
    if (hit == nullptr) {
        hit = scan(protected_.records);
    }
    if (hit == nullptr) {
        for (const auto& entry : dedicated_) {
            hit = scan(entry.second.records);
            if (hit != nullptr) {
                break;
            }
        }
    }
    if (hit != nullptr) {
        return Lookup{Horizon::Retained, hit};
    }
    // THE THREE HONEST ABSENCES, and they are genuinely different facts.
    if (bus_seq > newest_observed_seq_) {
        // Beyond anything the tap has shown us: still queued, never dispatched,
        // or never issued. The recorder does not know which and does not guess —
        // asking the queue is a different question with no answer today.
        return Lookup{Horizon::Unobserved, nullptr};
    }
    if (bus_seq <= forgotten_horizon_seq_) {
        return Lookup{Horizon::Forgotten, nullptr};
    }
    // Inside the observed range, above everything released, and not here: the
    // recorder was watching and deliberately did not keep it.
    return Lookup{Horizon::NotRecorded, nullptr};
}

const HistoryRecord* Recorder::record(std::uint64_t record_seq) const noexcept {
    const auto scan = [record_seq](const std::deque<HistoryRecord>& d) -> const HistoryRecord* {
        for (const HistoryRecord& r : d) {
            if (r.record_seq == record_seq) {
                return &r;
            }
        }
        return nullptr;
    };
    const HistoryRecord* hit = scan(shared_.records);
    if (hit == nullptr) {
        hit = scan(protected_.records);
    }
    if (hit == nullptr) {
        for (const auto& entry : dedicated_) {
            hit = scan(entry.second.records);
            if (hit != nullptr) {
                break;
            }
        }
    }
    return hit;
}

PayloadLookup Recorder::payload(std::uint64_t record_seq) const {
    for (const PayloadSlot& slot : payloads_) {
        if (slot.record_seq == record_seq) {
            return PayloadLookup{PayloadState::Retained, slot.bytes, slot.shape, slot.version};
        }
    }
    PayloadLookup out;
    const HistoryRecord* rec = record(record_seq);
    if (rec == nullptr) {
        // The metadata is gone too. Whether its payload was here once is no
        // longer a question this recorder can answer about a record it does not
        // have, so it answers about the payload only.
        out.state = record_seq <= payload_horizon_ ? PayloadState::Evicted : PayloadState::Absent;
        return out;
    }
    switch (rec->payload) {
    case PayloadDisposition::None:
        out.state = PayloadState::Absent;
        break;
    case PayloadDisposition::Retained:
        // Admitted once, not here now: the byte budget released it. The metadata
        // beside this answer is untouched, which is the property that matters.
        out.state = PayloadState::Evicted;
        break;
    case PayloadDisposition::TooLarge:
    case PayloadDisposition::NotRetained:
        out.state = PayloadState::Declined;
        break;
    }
    out.shape = rec->shape;
    out.shape_version = rec->shape_version;
    return out;
}

std::vector<ShapeTally> Recorder::tallies() const {
    std::vector<ShapeTally> out;
    out.reserve(tallies_.size());
    for (const auto& entry : tallies_) {
        out.push_back(entry.second);
    }
    return out;
}

} // namespace loom
