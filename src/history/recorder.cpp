// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/history/recorder.hpp>

#include <zen/serialize.hpp>

#include <utility>

namespace loom {

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
// The recorder
// ---------------------------------------------------------------------------

Recorder::Recorder(Switchboard& bus, RecorderPolicy policy)
    : bus_(bus), policy_(std::move(policy)) {
    recent_.capacity = policy_.recent_capacity;
    protected_.capacity = policy_.protected_capacity;
    tap_ = bus_.add_observer([this](const BusEvent& e) { observe(e); });
}

Recorder::~Recorder() {
    // Stop the callback before the members it captures die — the console's own
    // idiom, and the reason MSG-11 honours a removal made during a notification.
    bus_.remove_observer(tap_);
}

Recorder::ShapeState& Recorder::state_for(const std::string& shape) {
    auto it = shapes_.find(shape);
    if (it != shapes_.end()) {
        return it->second;
    }
    // FIRST SIGHT OF THIS SHAPE. The rule scan happens HERE, once, and never again
    // on the hot path: the resolved answer is cached in the shape's own state and
    // `apply_policy` is the only thing that invalidates it.
    ShapeState st;
    st.tally.shape = shape;
    const RetentionRule* rule = policy_.rule_for(shape);
    st.last_call.capacity = rule != nullptr ? rule->last_n : policy_.default_last_n;
    st.in_recent = rule != nullptr ? rule->in_recent : policy_.default_in_recent;
    st.retain_payload = rule != nullptr ? rule->retain_payload : policy_.default_retain_payload;
    return shapes_.emplace(shape, std::move(st)).first->second;
}

void Recorder::observe(const BusEvent& e) {
    ++counters_.observed;
    if (e.seq > newest_observed_seq_) {
        newest_observed_seq_ = e.seq;
    }
    // THE STRUCTURAL CLASS FIRST, before any counter that could be read as traffic
    // and before any policy decision. Recorder machinery is not a fact about the
    // system; it never enters the recordable universe, so there is nothing for a
    // rule to have an opinion about.
    if (blacklist_.excludes(e)) {
        ++counters_.declined_internal;
        return;
    }

    ShapeState& st = state_for(e.schema_name);
    ++st.tally.observed;

    // STRUCTURAL PROTECTION OUTRANKS A SHAPE RULE — for KEEPING. A rule saying "not
    // worth recent context" is about a shape's ordinary traffic, and a refusal, a
    // failed handler and a lifecycle transition are not that.
    const bool structural =
        (policy_.protect_refusals && e.kind == EventKind::Refused) ||
        (policy_.protect_handler_failures && e.kind == EventKind::HandlerFailed) ||
        (policy_.protect_lifecycle &&
         (e.kind == EventKind::Died || e.kind == EventKind::Revived));

    // THE CHEAP REJECTION, TAKEN BEFORE A RECORD IS BUILT. A shape with no
    // last-call slot, no share of recent context and no structural claim is not
    // going to be kept by anybody, and building nine strings to discover that is
    // the cost the heartbeat traffic would pay four hundred times a second.
    // Counted, never silent.
    if (!structural && st.last_call.capacity == 0 && !st.in_recent) {
        ++counters_.declined_by_policy;
        ++st.tally.declined;
        return;
    }
    ++st.tally.recorded;

    HistoryRecord rec;
    fill_from_event(rec, e);

    // FOUR DISPOSITIONS, AND THEY ARE NOT THE SAME FACT. An event that carried no
    // payload at all (every refusal, every lifecycle transition) is `None`; one
    // whose payload policy declined is `NotRetained`; the difference is exactly
    // what a reader needs to tell "there was nothing to keep" from "I chose not to
    // keep it".
    const bool has_body = e.payload != nullptr;
    if (has_body && !st.retain_payload) {
        rec.payload = PayloadDisposition::NotRetained;
    }
    admit(std::move(rec), has_body && st.retain_payload ? &e : nullptr, st, structural);
}

void Recorder::admit(HistoryRecord rec, const BusEvent* e, ShapeState& st, bool structural) {
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

    // Stored FIRST, with no claims: the windows below are what keep it alive, and
    // a record no window wants is erased again before this function returns.
    records_.emplace(record_seq, Slot{std::move(rec), 0});

    if (st.last_call.capacity > 0) {
        claim(st.last_call, record_seq, Held::LastCall);
    }
    // PROTECTION DECIDES WHETHER A FACT IS KEPT; THE SHAPE DECIDES WHETHER IT
    // COMPETES FOR RECENT CONTEXT. A muted shape's one refused beat is kept below,
    // and a storm of refused beats still cannot drown the build a maker came for.
    if (st.in_recent) {
        claim(recent_, record_seq, Held::Recent);
    }
    if (structural) {
        claim(protected_, record_seq, Held::Protected);
    }

    auto it = records_.find(record_seq);
    if (it == records_.end() || it->second.claims == 0) {
        // Nothing wanted it after all — only reachable when a structural claim was
        // the sole reason to build it and the protected window's own capacity is
        // zero. The tallies are corrected rather than left saying "recorded".
        if (it != records_.end()) {
            records_.erase(it);
        }
        --st.tally.recorded;
        ++st.tally.declined;
        ++counters_.declined_by_policy;
        return;
    }
    ++counters_.recorded;
    st.tally.last_call_held = st.last_call.seqs.size();

    if (keep_body) {
        store_payload(record_seq, std::move(body), shape, version);
    }
}

void Recorder::admit_local(HistoryRecord rec) {
    rec.record_seq = next_record_seq_++;
    const std::uint64_t record_seq = rec.record_seq;
    records_.emplace(record_seq, Slot{std::move(rec), 0});
    // THE PROTECTED WINDOW ONLY, and that is what keeps `apply_policy` prospective.
    // A note that also took a place in the recent FIFO would trim that FIFO to its
    // NEW capacity as a side effect of being written — so the act of shrinking a
    // window would destroy exactly what RTH-1 established it must not.
    claim(protected_, record_seq, Held::Protected);
    auto it = records_.find(record_seq);
    if (it == records_.end() || it->second.claims == 0) {
        if (it != records_.end()) {
            records_.erase(it);
        }
    }
    // DELIBERATELY NOT COUNTED IN `recorded`. That counter answers "how many of
    // the events I was shown became records", and this was not an event — nothing
    // was published and nothing observed it. Counting it here would break the one
    // arithmetic a reader can check by hand:
    //     observed == recorded + declined_by_policy + declined_internal
}

void Recorder::claim(Ring& w, std::uint64_t record_seq, Held which) {
    if (w.capacity == 0) {
        return;
    }
    // Room for one more. A capacity a policy change LOWERED is honoured here and
    // only here, which is what makes the change prospective: applying a policy
    // destroys nothing, and the excess drains as new traffic arrives.
    while (!w.seqs.empty() && w.seqs.size() >= w.capacity) {
        const std::uint64_t oldest = w.seqs.front();
        w.seqs.pop_front();
        ++w.evicted;
        auto it = records_.find(oldest);
        if (it != records_.end()) {
            it->second.rec.held &= static_cast<HeldMask>(~static_cast<std::uint8_t>(which));
            if (--it->second.claims == 0) {
                forget(oldest);
            }
        }
    }
    w.seqs.push_back(record_seq);
    auto it = records_.find(record_seq);
    if (it != records_.end()) {
        ++it->second.claims;
        it->second.rec.held |= static_cast<std::uint8_t>(which);
    }
}

void Recorder::forget(std::uint64_t record_seq) {
    auto it = records_.find(record_seq);
    if (it == records_.end()) {
        return;
    }
    ++forgotten_;
    if (it->second.rec.seq > forgotten_horizon_seq_) {
        forgotten_horizon_seq_ = it->second.rec.seq;
    }
    records_.erase(it);
    // A payload whose metadata is gone is unreachable — nothing can name it — so it
    // goes with the record rather than sitting in the budget forever. The reverse
    // is NOT true and must not be: a payload evicted by the byte budget leaves its
    // metadata exactly where it was.
    for (auto p = payloads_.begin(); p != payloads_.end(); ++p) {
        if (p->record_seq == record_seq) {
            payload_bytes_ -= p->bytes.size();
            ++payloads_forgotten_;
            if (p->record_seq > payload_horizon_) {
                payload_horizon_ = p->record_seq;
            }
            payloads_.erase(p);
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
    // the wrong thing. The newest payload is always kept even when it alone exceeds
    // the budget: refusing it would make the budget a second, silent per-payload
    // ceiling, and there already is one.
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

std::string describe_rule(const RetentionRule& r) {
    return "last_n=" + std::to_string(r.last_n) +
           (r.in_recent ? ", recent" : ", no recent") +
           (r.retain_payload ? ", payloads" : ", no payloads");
}

std::string describe_change(const RecorderPolicy& was, const RecorderPolicy& now,
                            std::size_t over_bound) {
    std::string note;
    const auto add = [&note](const std::string& s) {
        if (!note.empty()) {
            note += "; ";
        }
        note += s;
    };
    if (was.recent_capacity != now.recent_capacity) {
        add("recent " + std::to_string(was.recent_capacity) + " -> " +
            std::to_string(now.recent_capacity));
    }
    if (was.protected_capacity != now.protected_capacity) {
        add("protected " + std::to_string(was.protected_capacity) + " -> " +
            std::to_string(now.protected_capacity));
    }
    if (was.default_last_n != now.default_last_n) {
        add("default last_n " + std::to_string(was.default_last_n) + " -> " +
            std::to_string(now.default_last_n));
    }
    if (was.payload_byte_budget != now.payload_byte_budget) {
        add("payload budget " + std::to_string(was.payload_byte_budget) + " -> " +
            std::to_string(now.payload_byte_budget) + " bytes");
    }
    if (was.max_payload_bytes != now.max_payload_bytes) {
        add("payload ceiling " + std::to_string(was.max_payload_bytes) + " -> " +
            std::to_string(now.max_payload_bytes) + " bytes");
    }
    // Rules, by shape, both directions. THREE KNOBS ARE REPORTED, not one: a rule
    // that changed only its payload appetite still changed what Zen remembers, and
    // reporting a class alone would print a change that was not one.
    for (const RetentionRule& r : now.rules) {
        const RetentionRule* before = was.rule_for(r.shape);
        if (before == nullptr) {
            add(r.shape + ": default -> " + describe_rule(r));
        } else if (before->last_n != r.last_n || before->in_recent != r.in_recent ||
                   before->retain_payload != r.retain_payload) {
            add(r.shape + ": " + describe_rule(*before) + " -> " + describe_rule(r));
        }
    }
    for (const RetentionRule& r : was.rules) {
        if (now.rule_for(r.shape) == nullptr) {
            add(r.shape + ": " + describe_rule(r) + " -> default");
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
    const auto over_by = [](const Ring& w, std::size_t cap) {
        return w.seqs.size() > cap ? w.seqs.size() - cap : std::size_t{0};
    };
    over += over_by(recent_, next.recent_capacity);
    over += over_by(protected_, next.protected_capacity);
    for (const auto& entry : shapes_) {
        const RetentionRule* rule = next.rule_for(entry.first);
        over += over_by(entry.second.last_call,
                        rule != nullptr ? rule->last_n : next.default_last_n);
    }

    HistoryRecord rec;
    rec.kind = RecordKind::RecorderPolicy;
    rec.shape = "zen.recorder.PolicyChanged";
    rec.shape_version = 1;
    rec.note = describe_change(policy_, next, over);
    rec.payload = PayloadDisposition::None;

    policy_ = std::move(next);
    reseat_policy();

    // ALWAYS PROTECTED, and not a policy question. A recorder that could be told to
    // forget being told to forget would be able to lose the one fact that explains
    // every other absence. It is subject to the same bound as everything else in
    // that window — and it is NOT sent, which is the whole point: nothing was
    // published, so nothing observed it, so there is no recursion to guard against.
    admit_local(std::move(rec));
    // ...and the payload budget may itself have shrunk.
    trim_payloads();
}

void Recorder::reseat_policy() {
    recent_.capacity = policy_.recent_capacity;
    protected_.capacity = policy_.protected_capacity;
    // EVERY CACHED RULE IS INVALIDATED, which is the price of caching it on the hot
    // path and is paid here, once per policy change, rather than per delivery.
    for (auto& entry : shapes_) {
        const RetentionRule* rule = policy_.rule_for(entry.first);
        entry.second.last_call.capacity =
            rule != nullptr ? rule->last_n : policy_.default_last_n;
        entry.second.in_recent = rule != nullptr ? rule->in_recent : policy_.default_in_recent;
        entry.second.retain_payload =
            rule != nullptr ? rule->retain_payload : policy_.default_retain_payload;
    }
}

// ---------------------------------------------------------------------------
// The structured reader
// ---------------------------------------------------------------------------

std::vector<HistoryRecord> Recorder::snapshot() const {
    std::vector<HistoryRecord> all;
    all.reserve(records_.size());
    // ONE ORDER, and it is the recorder's own: `record_seq` is monotonic and the
    // store is keyed on it, so this is already chronological and a record claimed
    // by three windows still appears exactly once. A bus `seq` could not do this
    // job — a lifecycle transition has none and a policy change is not a bus fact
    // at all.
    for (const auto& entry : records_) {
        all.push_back(entry.second.rec);
    }
    return all;
}

std::vector<HistoryRecord> Recorder::snapshot_of(std::string_view shape) const {
    std::vector<HistoryRecord> out;
    for (const auto& entry : records_) {
        if (entry.second.rec.shape == shape) {
            out.push_back(entry.second.rec);
        }
    }
    return out;
}

std::vector<HistoryRecord> Recorder::recent() const {
    std::vector<HistoryRecord> out;
    out.reserve(recent_.seqs.size());
    for (const std::uint64_t s : recent_.seqs) {
        auto it = records_.find(s);
        if (it != records_.end()) {
            out.push_back(it->second.rec);
        }
    }
    return out;
}

std::size_t Recorder::retained() const noexcept { return records_.size(); }

RecorderBounds Recorder::bounds() const noexcept {
    RecorderBounds b;
    b.retained = records_.size();
    b.forgotten = forgotten_;
    b.forgotten_horizon_seq = forgotten_horizon_seq_;
    b.newest_observed_seq = newest_observed_seq_;
    b.payload_bytes = payload_bytes_;
    b.payloads_retained = payloads_.size();
    b.payloads_forgotten = payloads_forgotten_;
    b.recent_held = recent_.seqs.size();
    b.protected_held = protected_.seqs.size();
    b.shapes_observed = shapes_.size();
    for (const auto& entry : shapes_) {
        b.last_call_held += entry.second.last_call.seqs.size();
    }
    for (const auto& entry : records_) {
        const std::uint64_t s = entry.second.rec.seq;
        if (s != 0 && (b.oldest_retained_seq == 0 || s < b.oldest_retained_seq)) {
            b.oldest_retained_seq = s;
        }
    }
    return b;
}

std::vector<ShapeTally> Recorder::tallies() const {
    std::vector<ShapeTally> out;
    out.reserve(shapes_.size());
    for (const auto& entry : shapes_) {
        out.push_back(entry.second.tally);
    }
    return out;
}

bool Recorder::observed(std::string_view shape) const noexcept {
    const auto it = shapes_.find(shape);
    return it != shapes_.end() && it->second.tally.observed > 0;
}

Lookup Recorder::last_of(std::string_view shape) const noexcept {
    const auto it = shapes_.find(shape);
    if (it == shapes_.end()) {
        // Never seen at all — which is a different fact from "seen and not kept".
        return Lookup{Horizon::Unobserved, nullptr};
    }
    if (!it->second.last_call.seqs.empty()) {
        const auto slot = records_.find(it->second.last_call.seqs.back());
        if (slot != records_.end()) {
            return Lookup{Horizon::Retained, &slot->second.rec};
        }
    }
    // Observed, and no last-call slot holds one: the rule set `last_n` to zero, or
    // it has not been observed since the rule gave it a slot.
    return Lookup{Horizon::NotRecorded, nullptr};
}

std::vector<HistoryRecord> Recorder::last_calls_of(std::string_view shape) const {
    std::vector<HistoryRecord> out;
    const auto it = shapes_.find(shape);
    if (it == shapes_.end()) {
        return out;
    }
    for (const std::uint64_t s : it->second.last_call.seqs) {
        const auto slot = records_.find(s);
        if (slot != records_.end()) {
            out.push_back(slot->second.rec);
        }
    }
    return out;
}

Lookup Recorder::find(std::uint64_t bus_seq) const noexcept {
    if (bus_seq == 0) {
        return Lookup{Horizon::Unobserved, nullptr};
    }
    for (const auto& entry : records_) {
        if (entry.second.rec.seq == bus_seq) {
            return Lookup{Horizon::Retained, &entry.second.rec};
        }
    }
    // THE THREE HONEST ABSENCES, and they are genuinely different facts.
    if (bus_seq > newest_observed_seq_) {
        // Beyond anything the tap has shown us: still queued, never dispatched, or
        // never issued. The recorder does not know which and does not guess —
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
    const auto it = records_.find(record_seq);
    return it != records_.end() ? &it->second.rec : nullptr;
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
        // The metadata is gone too. Whether its payload was here once is no longer
        // a question this recorder can answer about a record it does not have, so
        // it answers about the payload only.
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

} // namespace loom
