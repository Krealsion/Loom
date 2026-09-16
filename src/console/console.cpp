// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/console/console.hpp>

#include <zen/kind.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loom {

namespace {

// Build a Cell for a declared field from a typed input value, type-checked at compose
// time. Returns nullopt + an error on a type mismatch or an unsupported (Message/List)
// field — the gate is still the unconditional backstop at send.
std::optional<loom::Cell> make_cell(const loom::Field& f, const FieldValue& v, std::string& err) {
    switch (f.type.kind) {
    case loom::Kind::Int:
        if (const auto* p = std::get_if<std::int64_t>(&v)) {
            return loom::Cell::integer(*p);
        }
        break;
    case loom::Kind::Float:
        if (const auto* p = std::get_if<double>(&v)) {
            return loom::Cell::real(*p);
        }
        break;
    case loom::Kind::Text:
        if (const auto* p = std::get_if<std::string>(&v)) {
            return loom::Cell::text(*p);
        }
        break;
    case loom::Kind::Bool:
        if (const auto* p = std::get_if<bool>(&v)) {
            return loom::Cell::boolean(*p);
        }
        break;
    case loom::Kind::Bytes:
        if (const auto* p = std::get_if<loom::Bytes>(&v)) {
            return loom::Cell::bytes(*p);
        }
        break;
    default:
        err = "field '" + f.name + "' is " + loom::name_of(f.type.kind) +
              " — Stage 1 compose sets only scalar fields (the gate backstops a required one)";
        return std::nullopt;
    }
    err = "field '" + f.name + "': value does not match the declared type " +
          loom::name_of(f.type.kind);
    return std::nullopt;
}

const char* event_kind_name(loom::EventKind k) {
    switch (k) {
    case loom::EventKind::Delivered:
        return "Delivered";
    case loom::EventKind::Refused:
        return "Refused";
    case loom::EventKind::Died:
        return "Died";
    case loom::EventKind::Revived:
        return "Revived";
    case loom::EventKind::HandlerFailed:
        return "HandlerFailed";
    }
    return "?";
}

// The console's trivial state (it is in-process and not crash-revived; the reply buffer is
// in-memory). A 1-field count keeps register_weave's snapshot-seeding happy.
std::shared_ptr<const loom::Schema> console_state_schema() {
    static const auto s =
        loom::SchemaBuilder("zen.ConsoleState", 1).field("received", loom::Kind::Int).build();
    return s;
}

} // namespace

// ---- the console's own Weave: accepts anything (via AcceptMode), buffers it -----------
class ConsoleWeave final : public loom::Weave {
public:
    ConsoleWeave(std::vector<std::shared_ptr<const loom::Schema>> vocabulary,
                 std::function<void(const loom::Message&)> on_arrival)
        : vocabulary_(std::move(vocabulary)), on_arrival_(std::move(on_arrival)) {}

    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        // Usually EMPTY: AcceptMode::AnyRegistered widens the door set at delivery, so the
        // console receives whatever the system already knows about.
        //
        // But "already knows about" is exactly the limit, and it is not a console policy — a
        // shape enters the registry by being in some weave's accept-set, claim-set or state,
        // and `AnyRegistered` still refuses a shape the registry cannot resolve. So a
        // NOTIFICATION shape — one that only ever travels TO the operator, which no other
        // participant has any reason to accept — was undeliverable to the operator's own
        // window. Declaring it here is how a host says "this window expects to be told this",
        // and it costs the rest of the console nothing: the wildcard still answers for
        // everything else, and a listed door is gated exactly as the wildcard one is.
        return vocabulary_;
    }
    void handle(const loom::Message& in, loom::Bus&) override {
        // Buffer EVERY received Value, generically — into a BOUNDED window. This weave is
        // registered AcceptMode::AnyRegistered, so what lands here is traffic-controlled: the
        // retention has to be the console's decision, not the senders'. Nothing is owed on the
        // buffer (handle() discharges the delivery here and now), so evicting the oldest retained
        // reply drops no obligation — only the operator's oldest referenceable value, whose label
        // then refuses honestly instead of re-binding.
        //
        // WITH ITS ROUTING FACTS. `sender` is Loom's stamp and `correlation` is what the sender
        // named; keeping the payload alone is what left a host unable to tell an answer from an
        // unrelated message of the same shape.
        received_.push(ConsoleArrival{in.payload, in.sender, in.correlation,
                                      in.provenance.answers_ask()});
        // SETTLED HERE, INSIDE THE DELIVERY, not later off the window. The window is bounded
        // history and may evict; a settlement is not history and must not be evictable.
        if (on_arrival_) {
            on_arrival_(in);
        }
    }
    loom::Value snapshot() const override {
        loom::Value v(console_state_schema());
        // Total OBSERVED, not retained: the field is named for what was received, and after the
        // window saturates the retained count would sit at the capacity forever and say nothing.
        v.set("received", loom::Cell::integer(static_cast<std::int64_t>(
                              received_.evicted() + received_.size())));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}
    const BoundedHistory<ConsoleArrival, kConsoleBufferCapacity>& received() const noexcept {
        return received_;
    }

private:
    BoundedHistory<ConsoleArrival, kConsoleBufferCapacity> received_;
    std::vector<std::shared_ptr<const loom::Schema>> vocabulary_;
    std::function<void(const loom::Message&)> on_arrival_;
};

ConsoleEngine::ConsoleEngine(loom::Switchboard& bus,
                             std::vector<std::shared_ptr<const loom::Schema>> vocabulary)
    : bus_(bus) {
    auto weave = std::make_unique<ConsoleWeave>(
        std::move(vocabulary), [this](const loom::Message& in) { record_arrival(in); });
    weave_ = weave.get();
    // The most-granted participant — broad send (drive any Weave with any shape) — but a
    // GRANT, not host root authority. Reply-receipt is AcceptMode::AnyRegistered; the tap +
    // discovery are direct host-side bus queries (pragmatic for a single operator; a
    // multi-user future scopes them to the operator's authority).
    console_id_ = bus_.register_weave(std::move(weave), loom::Grant{}.allow_any(),
                                      loom::AcceptMode::AnyRegistered);
    tap_obs_ = bus_.add_observer([this](const loom::BusEvent& e) { record_tap(e); });
}

ConsoleEngine::~ConsoleEngine() {
    bus_.remove_observer(tap_obs_);           // stop the callback before our members die
    (void)bus_.unregister_weave(console_id_); // hand the ConsoleWeave back; it is destroyed
}

void ConsoleEngine::record_tap(const loom::BusEvent& e) {
    TapEvent t;
    t.kind = event_kind_name(e.kind);
    t.target = e.target;
    t.sender = e.sender;
    t.schema = e.schema_name;
    t.refusal = e.kind == loom::EventKind::Refused ? loom::name_of(e.refusal.reason) : "";
    tap_.push(std::move(t)); // bounded window: the oldest event is evicted and counted, never lost
                             // silently (Console::evicted().tap, surfaced in the tap pane's title)

    // Per-region dirty for message-driven redraw. Every event touches the tap pane. A reply is a
    // Delivered event addressed to the console (ConsoleWeave::handle has already grown received_
    // by the time we see it). A Died/Revived changes who is on the bus -> the discovery pane.
    dirty_.tap = true;
    if (e.kind == loom::EventKind::Delivered && e.target == console_id_) {
        dirty_.buffer = true;
    }
    if (e.kind == loom::EventKind::Died || e.kind == loom::EventKind::Revived) {
        dirty_.weaves = true;
    }
}

Dirty ConsoleEngine::take_dirty() noexcept {
    Dirty d = dirty_;
    dirty_ = Dirty{};
    return d;
}

std::vector<WeaveInfo> ConsoleEngine::weaves() const {
    std::vector<WeaveInfo> out;
    for (loom::WeaveId id : bus_.list_weaves()) {
        if (id == console_id_) {
            continue; // the console is the operator's hands, not a send target
        }
        WeaveInfo info;
        info.id = id;
        for (const auto& s : bus_.accepted_schemas(id)) {
            info.accepts.push_back({s->name(), s->version()});
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::optional<ShapeDesc> ConsoleEngine::describe(std::string_view name,
                                                 std::uint32_t version) const {
    std::shared_ptr<const loom::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return std::nullopt;
    }
    return describe_schema(*schema);
}

void ConsoleEngine::record_arrival(const loom::Message& in) {
    // THE WALL, APPLIED ONCE, FOR EVERY ARRIVAL. `AskBook::settle` requires the pair — the
    // correlation this console minted, AND Loom's own stamp of who spoke — so a perfectly
    // shaped message from a participant that was not asked settles nothing, and neither does
    // a genuine respondent talking about a different conversation.
    const std::optional<loom::PendingAsk> mine = book_.settle(in.correlation, in.sender);
    if (!mine) {
        return;
    }
    // The label this arrival is also readable under, so an operator reading `buffer` and a
    // command reading its own answer are naming the same thing. The push has already
    // happened, so the newest retained label is base + size.
    BufferEntry e{"m" + std::to_string(reply_history().evicted() + reply_history().size()),
                  in.payload.schema().name(),
                  in.payload.schema().version(),
                  in.payload,
                  in.sender,
                  in.correlation,
                  in.provenance.answers_ask()};
    settled_.erase(mine->id); // a settled id is never reopened, so this can only be a no-op
    settled_.emplace(mine->id, std::move(e));
}

std::optional<BufferEntry> ConsoleEngine::settled(std::uint64_t ask) const {
    auto it = settled_.find(ask);
    return it == settled_.end() ? std::nullopt : std::optional<BufferEntry>(it->second);
}

bool ConsoleEngine::awaiting(std::uint64_t ask) const noexcept { return book_.waiting_on(ask); }

std::vector<loom::PendingAsk> ConsoleEngine::open_asks() const { return book_.entries(); }

bool ConsoleEngine::forget_ask(std::uint64_t ask) {
    const bool had_answer = settled_.erase(ask) != 0;
    return book_.forget(ask).has_value() || had_answer;
}

std::size_t ConsoleEngine::ask_capacity() const noexcept { return book_.capacity(); }

std::size_t ConsoleEngine::asks_outstanding() const noexcept { return book_.outstanding(); }

loom::Ticket ConsoleEngine::assemble_and_send(
    loom::WeaveId target, const std::shared_ptr<const loom::Schema>& schema,
    const std::map<std::string, loom::Cell>& cells) {
    // construct_blind walks the schema's declared fields and fills each from the cells we
    // set; an unset field is left ABSENT, to be caught at the gate (the send-time backstop).
    loom::Value v =
        loom::construct_blind(schema, [&](const loom::Field& f) -> std::optional<loom::Cell> {
            auto it = cells.find(f.name);
            if (it == cells.end()) {
                return std::nullopt;
            }
            return it->second;
        });
    // OPEN THE CONVERSATION FIRST, so the number on the wire is the number the book is
    // watching for. A full book still SENDS — refusing the send would turn a bookkeeping
    // limit into a messaging limit — but it stamps an untracked number from the same
    // sequence and reports `ask == 0`, so a caller that needs attribution knows it has none
    // rather than being handed a handle that can never settle.
    const loom::AskOpened opened = book_.open(target, schema->name(), schema->version());
    last_ask_ = opened ? opened.id : 0;
    const std::uint64_t correlation = opened ? opened.correlation : book_.mint_correlation();
    // Gated send AS the console: send_as stamps the console as sender and authorizes against
    // the console's grant; reply_to is the console so replies route back; the gate admits
    // against the target's accept-set at delivery.
    const loom::Ticket t = bus_.send_as(
        console_id_, target,
        loom::Message(std::move(v), loom::WeaveId{}, console_id_, correlation));
    if (opened) {
        (void)book_.bind_attempt(opened.id, t.seq); // the queued attempt, for diagnostics
    }
    return t;
}

Submitted ConsoleEngine::submit(loom::WeaveId target, std::string_view name,
                                std::uint32_t version,
                                const std::map<std::string, FieldValue>& fields,
                                std::string* error) {
    const auto fail = [&](const std::string& m) {
        if (error != nullptr) {
            *error = m;
        }
        return Submitted{};
    };
    std::shared_ptr<const loom::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return fail("no such registered shape: " + std::string(name) + " v" +
                    std::to_string(version));
    }
    std::map<std::string, loom::Cell> cells;
    for (const auto& [fname, fval] : fields) {
        const loom::Field* f = schema->find(fname);
        if (f == nullptr) {
            return fail("shape " + schema->name() + " has no field '" + fname + "'");
        }
        std::string err;
        std::optional<loom::Cell> cell = make_cell(*f, fval, err);
        if (!cell) {
            return fail(err);
        }
        cells.insert_or_assign(fname, std::move(*cell));
    }
    const loom::Ticket t = assemble_and_send(target, schema, cells);
    return Submitted{t, last_ask_};
}

SendOutcome ConsoleEngine::outcome(loom::Ticket t) const {
    const loom::DeliveryOutcome o = bus_.outcome(t);
    SendOutcome s;
    s.delivered = o.disposition == loom::Disposition::Delivered;
    s.refused = o.disposition == loom::Disposition::Refused;
    if (s.refused) {
        s.reason = o.refusal.message();
    }
    return s;
}

const BoundedHistory<ConsoleArrival, kConsoleBufferCapacity>& ConsoleEngine::reply_history() const {
    return weave_->received();
}

std::size_t ConsoleEngine::buffer_size() const { return reply_history().size(); }

Evicted ConsoleEngine::evicted() const {
    Evicted e;
    e.tap = tap_.evicted();
    e.buffer = reply_history().evicted();
    return e;
}

std::optional<BufferEntry> ConsoleEngine::buffer_at(std::size_t label_number) const {
    // `label_number` is the N of `mN` — a STABLE IDENTITY over every reply this console has ever
    // received, not a position in the retained window. Before the window saturates the two
    // coincide, which is why this used to be spelled as an index; once eviction begins they part,
    // and the identity is the one worth keeping (an operator's `$m7.count` must never quietly
    // become a different reply). Outside the retained range this refuses, exactly as it always did
    // for a label that never arrived.
    const BoundedHistory<ConsoleArrival, kConsoleBufferCapacity>& buf = reply_history();
    const std::uint64_t base = buf.evicted(); // labels m(base+1) .. m(base+size) are retained
    if (label_number <= base || label_number > base + buf.size()) {
        return std::nullopt;
    }
    const ConsoleArrival& a = buf.at(static_cast<std::size_t>(label_number - base - 1));
    return BufferEntry{"m" + std::to_string(label_number),
                       a.payload.schema().name(),
                       a.payload.schema().version(),
                       a.payload,
                       a.sender,
                       a.correlation,
                       a.answers_ask};
}

void ConsoleEngine::pump() { bus_.drain_until_idle(); }

std::shared_ptr<const loom::Schema> ConsoleEngine::resolve_schema(std::string_view name,
                                                                 std::uint32_t version) const {
    return bus_.resolve_schema(name, version); // the in-process engine reads the bus's registry
}

std::optional<loom::Cell> ConsoleEngine::resolve_ref(const Ref& ref, std::string* error) const {
    return resolve_ref_from(*this, ref, error); // shared with the remote console (both have buffer_at)
}

std::optional<loom::Cell> resolve_ref_from(const Console& console, const Ref& ref,
                                           std::string* error) {
    const auto fail = [&](const std::string& m) -> std::optional<loom::Cell> {
        if (error != nullptr) {
            *error = m;
        }
        return std::nullopt;
    };
    // The buffer label is the engine's format ("mN"); parse it back to an index.
    if (ref.label.size() < 2 || ref.label[0] != 'm') {
        return fail("bad reference label '" + ref.label + "' (expected mN)");
    }
    std::size_t n = 0;
    try {
        std::size_t pos = 0;
        n = static_cast<std::size_t>(std::stoull(ref.label.substr(1), &pos));
        if (pos + 1 != ref.label.size()) {
            return fail("bad reference label '" + ref.label + "'");
        }
    } catch (...) {
        return fail("bad reference label '" + ref.label + "'");
    }
    std::optional<BufferEntry> entry = console.buffer_at(n);
    if (!entry) {
        // Two different absences, and the operator needs them apart: a label that never arrived is
        // a typo, a label that was EVICTED is the bounded window telling the truth about its own
        // horizon. A reference never silently resolves to whatever now occupies that slot.
        if (n != 0 && n <= console.evicted().buffer) {
            return fail("buffer entry " + ref.label + " was evicted (the console retains the most " +
                        "recent " + std::to_string(kConsoleBufferCapacity) + " replies; " +
                        std::to_string(console.evicted().buffer) + " older ones were discarded)");
        }
        return fail("no such buffer entry: " + ref.label);
    }
    const loom::Cell* c = entry->value.get(ref.field);
    if (c == nullptr) {
        return fail("buffer entry " + ref.label + " (" + entry->name + ") has no field '" +
                    ref.field + "'");
    }
    const loom::Kind k = c->kind();
    if (k == loom::Kind::Message || k == loom::Kind::List) {
        return fail("reference to non-scalar field '" + ref.field + "' is not supported in Stage 2");
    }
    return *c; // copy the scalar Cell out of the immutable buffered Value
}

Composed ConsoleEngine::compose(loom::WeaveId target, std::string_view name,
                                std::uint32_t version, const std::vector<Arg>& args) {
    // This engine IS the LadderHost; the ladder logic is shared with the remote console so the
    // ~150 lines of placement intricacy are not duplicated across transports.
    last_ask_ = 0;
    Composed c = run_compose_ladder(*this, target, name, version, args);
    // The ladder sends through assemble_and_send above, which is where the conversation was
    // opened; carry its handle out so an operator one-liner is attributable too.
    c.ask = (c.status == Composed::Status::Ready) ? last_ask_ : 0;
    return c;
}

Composed run_compose_ladder(LadderHost& host, loom::WeaveId target, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args) {
    // The ladder itself is `loom::compose_message`, which stops one step before anything
    // is authored; this is the console's original one-shot form over it, so the placement rules,
    // their order, and every refusal they produce are exactly the ones this suite has always
    // pinned. The only thing that moved is where the sending happens.
    const Composition composed = compose_message(host, name, version, args);
    Composed result;
    switch (composed.status) {
    case Composition::Status::Error:
        result.status = Composed::Status::Error;
        result.error = composed.error;
        return result;
    case Composition::Status::NeedsInput:
        result.status = Composed::Status::NeedsInput;
        result.open_fields = composed.open_fields;
        result.unplaced = composed.unplaced;
        return result;
    case Composition::Status::Ready:
        break;
    }
    // Ready: assemble + gate-send (the gate is the unconditional backstop).
    result.status = Composed::Status::Ready;
    result.ticket = host.assemble_and_send(target, composed.schema, composed.cells);
    return result;
}

} // namespace loom
