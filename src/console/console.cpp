#include <zen/console/console.hpp>

#include <zen/kind.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace zen::console {

namespace {

std::string type_string(const zen::TypeRef& t) {
    switch (t.kind) {
    case zen::Kind::List:
        return std::string("List<") + type_string(*t.element) + ">";
    case zen::Kind::Message:
        return std::string("Message(") + t.message->name() + " v" +
               std::to_string(t.message->version()) + ")";
    default:
        return zen::name_of(t.kind);
    }
}

// Build a Cell for a declared field from a typed input value, type-checked at compose
// time. Returns nullopt + an error on a type mismatch or an unsupported (Message/List)
// field — the gate is still the unconditional backstop at send.
std::optional<zen::Cell> make_cell(const zen::Field& f, const FieldValue& v, std::string& err) {
    switch (f.type.kind) {
    case zen::Kind::Int:
        if (const auto* p = std::get_if<std::int64_t>(&v)) {
            return zen::Cell::integer(*p);
        }
        break;
    case zen::Kind::Float:
        if (const auto* p = std::get_if<double>(&v)) {
            return zen::Cell::real(*p);
        }
        break;
    case zen::Kind::Text:
        if (const auto* p = std::get_if<std::string>(&v)) {
            return zen::Cell::text(*p);
        }
        break;
    case zen::Kind::Bool:
        if (const auto* p = std::get_if<bool>(&v)) {
            return zen::Cell::boolean(*p);
        }
        break;
    case zen::Kind::Bytes:
        if (const auto* p = std::get_if<zen::Bytes>(&v)) {
            return zen::Cell::bytes(*p);
        }
        break;
    default:
        err = "field '" + f.name + "' is " + zen::name_of(f.type.kind) +
              " — Stage 1 compose sets only scalar fields (the gate backstops a required one)";
        return std::nullopt;
    }
    err = "field '" + f.name + "': value does not match the declared type " +
          zen::name_of(f.type.kind);
    return std::nullopt;
}

const char* event_kind_name(zen::sb::EventKind k) {
    switch (k) {
    case zen::sb::EventKind::Delivered:
        return "Delivered";
    case zen::sb::EventKind::Refused:
        return "Refused";
    case zen::sb::EventKind::Died:
        return "Died";
    case zen::sb::EventKind::Revived:
        return "Revived";
    }
    return "?";
}

// The console's trivial state (it is in-process and not crash-revived; the reply buffer is
// in-memory). A 1-field count keeps register_shard's snapshot-seeding happy.
std::shared_ptr<const zen::Schema> console_state_schema() {
    static const auto s =
        zen::SchemaBuilder("zen.ConsoleState", 1).field("received", zen::Kind::Int).build();
    return s;
}

// ---- Stage 2: the assumption-ladder helpers ----

// A normalized argument: a literal (FieldValue) or a resolved reference (a scalar Cell),
// carrying the arg's kind for type-directed matching.
struct NArg {
    std::optional<std::string> name;          // set iff named (`field=…`)
    zen::Kind kind;                           // the arg's own type
    std::variant<FieldValue, zen::Cell> payload; // literal, or resolved-reference cell
};

zen::Kind literal_kind(const FieldValue& v) {
    return std::visit(
        [](auto&& x) -> zen::Kind {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return zen::Kind::Int;
            } else if constexpr (std::is_same_v<T, double>) {
                return zen::Kind::Float;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return zen::Kind::Text;
            } else if constexpr (std::is_same_v<T, bool>) {
                return zen::Kind::Bool;
            } else {
                return zen::Kind::Bytes;
            }
        },
        v);
}

// Place arg `a` into field `f`: a literal coerces among numerics (Int→Float ok), exact
// otherwise; a reference matches its resolved kind EXACTLY (no coercion — predictable
// wiring). Returns the cell to assign, or nullopt if the arg does not fit the field.
std::optional<zen::Cell> place(const zen::Field& f, const NArg& a) {
    if (const zen::Cell* refcell = std::get_if<zen::Cell>(&a.payload)) {
        if (refcell->kind() == f.type.kind) {
            return *refcell;
        }
        return std::nullopt;
    }
    const FieldValue& v = std::get<FieldValue>(a.payload);
    switch (f.type.kind) {
    case zen::Kind::Int:
        if (const auto* p = std::get_if<std::int64_t>(&v)) {
            return zen::Cell::integer(*p);
        }
        break;
    case zen::Kind::Float:
        if (const auto* pd = std::get_if<double>(&v)) {
            return zen::Cell::real(*pd);
        }
        if (const auto* pi = std::get_if<std::int64_t>(&v)) {
            return zen::Cell::real(static_cast<double>(*pi)); // numeric widening Int→Float
        }
        break;
    case zen::Kind::Text:
        if (const auto* p = std::get_if<std::string>(&v)) {
            return zen::Cell::text(*p);
        }
        break;
    case zen::Kind::Bool:
        if (const auto* p = std::get_if<bool>(&v)) {
            return zen::Cell::boolean(*p);
        }
        break;
    case zen::Kind::Bytes:
        if (const auto* p = std::get_if<zen::Bytes>(&v)) {
            return zen::Cell::bytes(*p);
        }
        break;
    default:
        break; // Message/List not composable in Stage 2
    }
    return std::nullopt;
}

// Render an unplaced arg for the NeedsInput prompt (domain data; the frontend formats).
std::string render_narg(const NArg& a) {
    if (const zen::Cell* c = std::get_if<zen::Cell>(&a.payload)) {
        switch (c->kind()) {
        case zen::Kind::Int:
            return std::to_string(c->as_int());
        case zen::Kind::Float:
            return std::to_string(c->as_float());
        case zen::Kind::Text:
            return c->as_text();
        case zen::Kind::Bool:
            return c->as_bool() ? "true" : "false";
        default:
            return std::string("(") + zen::name_of(c->kind()) + ")";
        }
    }
    return std::visit(
        [](auto&& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                return std::to_string(x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return x;
            } else if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else {
                return "(bytes)";
            }
        },
        std::get<FieldValue>(a.payload));
}

} // namespace

// ---- the console's own Shard: accepts anything (via AcceptMode), buffers it -----------
class ConsoleShard final : public zen::sb::Shard {
public:
    std::vector<std::shared_ptr<const zen::Schema>> accepted_schemas() const override {
        return {}; // no listed shapes; AcceptMode::AnyRegistered widens this at delivery
    }
    void handle(const zen::sb::Message& in, zen::sb::Bus&) override {
        received_.push_back(in.payload); // buffer EVERY received Value, generically
    }
    zen::Value snapshot() const override {
        zen::Value v(console_state_schema());
        v.set("received", zen::Cell::integer(static_cast<std::int64_t>(received_.size())));
        return v;
    }
    zen::Value policy() const override {
        zen::Value v(zen::sb::lifecycle_policy_schema());
        v.set("max_reloads", zen::Cell::integer(0));
        v.set("revive_from_last_good", zen::Cell::boolean(true));
        return v;
    }
    void revive(const zen::Value&) override {}
    const std::vector<zen::Value>& received() const noexcept { return received_; }

private:
    std::vector<zen::Value> received_;
};

ConsoleEngine::ConsoleEngine(zen::sb::Switchboard& bus) : bus_(bus) {
    auto shard = std::make_unique<ConsoleShard>();
    shard_ = shard.get();
    // The most-granted participant — broad send (drive any Shard with any shape) — but a
    // GRANT, not host root authority. Reply-receipt is AcceptMode::AnyRegistered; the tap +
    // discovery are direct host-side bus queries (pragmatic for a single operator; a
    // multi-user future scopes them to the operator's authority).
    console_id_ = bus_.register_shard(std::move(shard), zen::sb::Grant{}.allow_any(),
                                      zen::sb::AcceptMode::AnyRegistered);
    tap_obs_ = bus_.add_observer([this](const zen::sb::BusEvent& e) { record_tap(e); });
}

ConsoleEngine::~ConsoleEngine() {
    bus_.remove_observer(tap_obs_);           // stop the callback before our members die
    (void)bus_.unregister_shard(console_id_); // hand the ConsoleShard back; it is destroyed
}

void ConsoleEngine::record_tap(const zen::sb::BusEvent& e) {
    TapEvent t;
    t.kind = event_kind_name(e.kind);
    t.target = e.target;
    t.sender = e.sender;
    t.schema = e.schema_name;
    t.refusal = e.kind == zen::sb::EventKind::Refused ? zen::sb::name_of(e.refusal.reason) : "";
    tap_.push_back(std::move(t));

    // Per-region dirty for message-driven redraw. Every event touches the tap pane. A reply is a
    // Delivered event addressed to the console (ConsoleShard::handle has already grown received_
    // by the time we see it). A Died/Revived changes who is on the bus -> the discovery pane.
    dirty_.tap = true;
    if (e.kind == zen::sb::EventKind::Delivered && e.target == console_id_) {
        dirty_.buffer = true;
    }
    if (e.kind == zen::sb::EventKind::Died || e.kind == zen::sb::EventKind::Revived) {
        dirty_.shards = true;
    }
}

ConsoleEngine::Dirty ConsoleEngine::take_dirty() noexcept {
    Dirty d = dirty_;
    dirty_ = Dirty{};
    return d;
}

std::vector<ShardInfo> ConsoleEngine::shards() const {
    std::vector<ShardInfo> out;
    for (zen::sb::ShardId id : bus_.list_shards()) {
        if (id == console_id_) {
            continue; // the console is the operator's hands, not a send target
        }
        ShardInfo info;
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
    std::shared_ptr<const zen::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return std::nullopt;
    }
    ShapeDesc d;
    d.name = schema->name();
    d.version = schema->version();
    for (const zen::Field& f : schema->fields()) {
        d.fields.push_back({f.name, type_string(f.type), f.required});
    }
    return d;
}

zen::sb::Ticket ConsoleEngine::assemble_and_send(
    zen::sb::ShardId target, const std::shared_ptr<const zen::Schema>& schema,
    const std::map<std::string, zen::Cell>& cells) {
    // construct_blind walks the schema's declared fields and fills each from the cells we
    // set; an unset field is left ABSENT, to be caught at the gate (the send-time backstop).
    zen::Value v =
        zen::construct_blind(schema, [&](const zen::Field& f) -> std::optional<zen::Cell> {
            auto it = cells.find(f.name);
            if (it == cells.end()) {
                return std::nullopt;
            }
            return it->second;
        });
    // Gated send AS the console: send_as stamps the console as sender and authorizes against
    // the console's grant; reply_to is the console so replies route back; the gate admits
    // against the target's accept-set at delivery.
    return bus_.send_as(
        console_id_, target,
        zen::sb::Message(std::move(v), zen::sb::ShardId{}, console_id_, ++correlation_));
}

zen::sb::Ticket ConsoleEngine::submit(zen::sb::ShardId target, std::string_view name,
                                      std::uint32_t version,
                                      const std::map<std::string, FieldValue>& fields,
                                      std::string* error) {
    const auto fail = [&](const std::string& m) {
        if (error != nullptr) {
            *error = m;
        }
        return zen::sb::Ticket{};
    };
    std::shared_ptr<const zen::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return fail("no such registered shape: " + std::string(name) + " v" +
                    std::to_string(version));
    }
    std::map<std::string, zen::Cell> cells;
    for (const auto& [fname, fval] : fields) {
        const zen::Field* f = schema->find(fname);
        if (f == nullptr) {
            return fail("shape " + schema->name() + " has no field '" + fname + "'");
        }
        std::string err;
        std::optional<zen::Cell> cell = make_cell(*f, fval, err);
        if (!cell) {
            return fail(err);
        }
        cells.insert_or_assign(fname, std::move(*cell));
    }
    return assemble_and_send(target, schema, cells);
}

bool ConsoleEngine::begin(zen::sb::ShardId target, std::string_view name, std::uint32_t version,
                          std::string* error) {
    std::shared_ptr<const zen::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        if (error != nullptr) {
            *error = "no such registered shape";
        }
        return false;
    }
    compose_ = Compose{target, std::move(schema), {}};
    return true;
}

bool ConsoleEngine::set_field(std::string_view field, const FieldValue& value, std::string* error) {
    if (!compose_) {
        if (error != nullptr) {
            *error = "no compose in progress (call begin first)";
        }
        return false;
    }
    const zen::Field* f = compose_->schema->find(field);
    if (f == nullptr) {
        if (error != nullptr) {
            *error = "shape has no field '" + std::string(field) + "'";
        }
        return false;
    }
    std::string err;
    std::optional<zen::Cell> cell = make_cell(*f, value, err);
    if (!cell) {
        if (error != nullptr) {
            *error = err;
        }
        return false;
    }
    compose_->cells.insert_or_assign(std::string(field), std::move(*cell));
    return true;
}

zen::sb::Ticket ConsoleEngine::send(std::string* error) {
    if (!compose_) {
        if (error != nullptr) {
            *error = "no compose in progress";
        }
        return zen::sb::Ticket{};
    }
    Compose c = std::move(*compose_);
    compose_.reset();
    return assemble_and_send(c.target, c.schema, c.cells);
}

SendOutcome ConsoleEngine::outcome(zen::sb::Ticket t) const {
    const zen::sb::DeliveryOutcome o = bus_.outcome(t);
    SendOutcome s;
    s.delivered = o.disposition == zen::sb::Disposition::Delivered;
    s.refused = o.disposition == zen::sb::Disposition::Refused;
    if (s.refused) {
        s.reason = o.refusal.message();
    }
    return s;
}

std::size_t ConsoleEngine::buffer_size() const { return shard_->received().size(); }

std::optional<BufferEntry> ConsoleEngine::buffer_at(std::size_t one_based_index) const {
    const std::vector<zen::Value>& buf = shard_->received();
    if (one_based_index == 0 || one_based_index > buf.size()) {
        return std::nullopt;
    }
    const zen::Value& v = buf[one_based_index - 1];
    BufferEntry e{"m" + std::to_string(one_based_index), v.schema().name(), v.schema().version(),
                  v};
    return e;
}

void ConsoleEngine::pump() { bus_.pump(); }

std::optional<zen::Cell> ConsoleEngine::resolve_ref(const Ref& ref, std::string* error) const {
    const auto fail = [&](const std::string& m) -> std::optional<zen::Cell> {
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
    std::optional<BufferEntry> entry = buffer_at(n);
    if (!entry) {
        return fail("no such buffer entry: " + ref.label);
    }
    const zen::Cell* c = entry->value.get(ref.field);
    if (c == nullptr) {
        return fail("buffer entry " + ref.label + " (" + entry->name + ") has no field '" +
                    ref.field + "'");
    }
    const zen::Kind k = c->kind();
    if (k == zen::Kind::Message || k == zen::Kind::List) {
        return fail("reference to non-scalar field '" + ref.field + "' is not supported in Stage 2");
    }
    return *c; // copy the scalar Cell out of the immutable buffered Value
}

Composed ConsoleEngine::compose(zen::sb::ShardId target, std::string_view name,
                                std::uint32_t version, const std::vector<Arg>& args) {
    Composed result;
    const auto error = [&](const std::string& m) {
        result.status = Composed::Status::Error;
        result.error = m;
        return result;
    };
    std::shared_ptr<const zen::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return error("no such registered shape: " + std::string(name) + " v" +
                     std::to_string(version));
    }

    // Normalize: resolve references to scalar Cells up front (a bad reference is a hard error).
    std::vector<NArg> nargs;
    nargs.reserve(args.size());
    for (const Arg& a : args) {
        if (const Ref* ref = std::get_if<Ref>(&a.value)) {
            std::string rerr;
            std::optional<zen::Cell> cell = resolve_ref(*ref, &rerr);
            if (!cell) {
                return error(rerr);
            }
            const zen::Kind k = cell->kind();
            nargs.push_back(NArg{a.name, k, std::move(*cell)});
        } else {
            const FieldValue& lit = std::get<FieldValue>(a.value);
            nargs.push_back(NArg{a.name, literal_kind(lit), lit});
        }
    }

    std::map<std::string, zen::Cell> assigned;

    // Rung 1: named wins.
    std::vector<const NArg*> bare;
    for (const NArg& a : nargs) {
        if (a.name) {
            const zen::Field* f = schema->find(*a.name);
            if (f == nullptr) {
                return error("shape " + schema->name() + " has no field '" + *a.name + "'");
            }
            if (assigned.count(*a.name) != 0) {
                return error("field '" + *a.name + "' assigned more than once");
            }
            std::optional<zen::Cell> cell = place(*f, a);
            if (!cell) {
                return error("field '" + *a.name + "' (" + zen::name_of(f->type.kind) +
                             ") cannot take this value");
            }
            assigned.insert_or_assign(*a.name, std::move(*cell));
        } else {
            bare.push_back(&a);
        }
    }

    // Open fields, in declaration order.
    std::vector<const zen::Field*> open;
    for (const zen::Field& f : schema->fields()) {
        if (assigned.count(f.name) == 0) {
            open.push_back(&f);
        }
    }

    // Rungs 2/3: place the bare args — positional, else type-directed, else prompt.
    if (!bare.empty()) {
        bool placed = false;

        // Rung 2: positional (bare[i] → open[i]); accept only if EVERY one fits, else fall.
        if (bare.size() <= open.size()) {
            std::vector<zen::Cell> cells;
            bool all = true;
            for (std::size_t i = 0; i < bare.size(); ++i) {
                std::optional<zen::Cell> c = place(*open[i], *bare[i]);
                if (!c) {
                    all = false;
                    break;
                }
                cells.push_back(std::move(*c));
            }
            if (all) {
                for (std::size_t i = 0; i < bare.size(); ++i) {
                    assigned.insert_or_assign(open[i]->name, std::move(cells[i]));
                }
                open.erase(open.begin(), open.begin() + static_cast<std::ptrdiff_t>(bare.size()));
                placed = true;
            }
        }

        // Rung 3: type-directed (each bare arg → its UNIQUE fitting open field, all distinct).
        if (!placed) {
            std::vector<std::size_t> chosen(bare.size(), 0);
            bool ok = true;
            for (std::size_t i = 0; i < bare.size() && ok; ++i) {
                std::size_t match = 0;
                int count = 0;
                for (std::size_t j = 0; j < open.size(); ++j) {
                    if (place(*open[j], *bare[i]).has_value()) {
                        match = j;
                        ++count;
                    }
                }
                if (count != 1) {
                    ok = false; // no match, or several → ambiguous
                    break;
                }
                chosen[i] = match;
            }
            std::set<std::size_t> matched;
            if (ok) {
                for (std::size_t c : chosen) {
                    if (!matched.insert(c).second) {
                        ok = false; // two args claim the same field → ambiguous
                        break;
                    }
                }
            }
            if (ok) {
                for (std::size_t i = 0; i < bare.size(); ++i) {
                    const zen::Field* f = open[chosen[i]];
                    assigned.insert_or_assign(f->name, std::move(*place(*f, *bare[i])));
                }
                std::vector<const zen::Field*> remaining;
                for (std::size_t j = 0; j < open.size(); ++j) {
                    if (matched.count(j) == 0) {
                        remaining.push_back(open[j]);
                    }
                }
                open.swap(remaining);
                placed = true;
            }
        }

        if (!placed) {
            // Rung 4: genuine ambiguity → prompt (never guess, never mis-send).
            result.status = Composed::Status::NeedsInput;
            for (const zen::Field* f : open) {
                result.open_fields.push_back({f->name, type_string(f->type), f->required});
            }
            for (const NArg* a : bare) {
                result.unplaced.push_back(render_narg(*a));
            }
            return result;
        }
    }

    // A still-open REQUIRED field → prompt rather than knowingly send incomplete (the gate
    // would refuse it anyway, but prompting is the friendlier floor).
    bool any_required_open = false;
    for (const zen::Field* f : open) {
        if (f->required) {
            any_required_open = true;
        }
    }
    if (any_required_open) {
        result.status = Composed::Status::NeedsInput;
        for (const zen::Field* f : open) {
            result.open_fields.push_back({f->name, type_string(f->type), f->required});
        }
        return result; // unplaced empty: every arg was placed; only required fields remain
    }

    // Ready: assemble + gate-send (the gate is the unconditional backstop).
    result.status = Composed::Status::Ready;
    result.ticket = assemble_and_send(target, schema, assigned);
    return result;
}

} // namespace zen::console
