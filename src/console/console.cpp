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

namespace loom {

namespace {

std::string type_string(const loom::TypeRef& t) {
    switch (t.kind) {
    case loom::Kind::List:
        return std::string("List<") + type_string(*t.element) + ">";
    case loom::Kind::Message:
        return std::string("Message(") + t.message->name() + " v" +
               std::to_string(t.message->version()) + ")";
    default:
        return loom::name_of(t.kind);
    }
}

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

// ---- Stage 2: the assumption-ladder helpers ----

// A normalized argument: a literal (FieldValue) or a resolved reference (a scalar Cell),
// carrying the arg's kind for type-directed matching.
struct NArg {
    std::optional<std::string> name;          // set iff named (`field=…`)
    loom::Kind kind;                           // the arg's own type
    std::variant<FieldValue, loom::Cell> payload; // literal, or resolved-reference cell
};

loom::Kind literal_kind(const FieldValue& v) {
    return std::visit(
        [](auto&& x) -> loom::Kind {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return loom::Kind::Int;
            } else if constexpr (std::is_same_v<T, double>) {
                return loom::Kind::Float;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return loom::Kind::Text;
            } else if constexpr (std::is_same_v<T, bool>) {
                return loom::Kind::Bool;
            } else {
                return loom::Kind::Bytes;
            }
        },
        v);
}

// Place arg `a` into field `f`: a literal coerces among numerics (Int→Float ok), exact
// otherwise; a reference matches its resolved kind EXACTLY (no coercion — predictable
// wiring). Returns the cell to assign, or nullopt if the arg does not fit the field.
std::optional<loom::Cell> place(const loom::Field& f, const NArg& a) {
    if (const loom::Cell* refcell = std::get_if<loom::Cell>(&a.payload)) {
        if (refcell->kind() == f.type.kind) {
            return *refcell;
        }
        return std::nullopt;
    }
    const FieldValue& v = std::get<FieldValue>(a.payload);
    switch (f.type.kind) {
    case loom::Kind::Int:
        if (const auto* p = std::get_if<std::int64_t>(&v)) {
            return loom::Cell::integer(*p);
        }
        break;
    case loom::Kind::Float:
        if (const auto* pd = std::get_if<double>(&v)) {
            return loom::Cell::real(*pd);
        }
        if (const auto* pi = std::get_if<std::int64_t>(&v)) {
            return loom::Cell::real(static_cast<double>(*pi)); // numeric widening Int→Float
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
        break; // Message/List not composable in Stage 2
    }
    return std::nullopt;
}

// Render an unplaced arg for the NeedsInput prompt (domain data; the frontend formats).
std::string render_narg(const NArg& a) {
    if (const loom::Cell* c = std::get_if<loom::Cell>(&a.payload)) {
        switch (c->kind()) {
        case loom::Kind::Int:
            return std::to_string(c->as_int());
        case loom::Kind::Float:
            return std::to_string(c->as_float());
        case loom::Kind::Text:
            return c->as_text();
        case loom::Kind::Bool:
            return c->as_bool() ? "true" : "false";
        default:
            return std::string("(") + loom::name_of(c->kind()) + ")";
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

// ---- the console's own Weave: accepts anything (via AcceptMode), buffers it -----------
class ConsoleWeave final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {}; // no listed shapes; AcceptMode::AnyRegistered widens this at delivery
    }
    void handle(const loom::Message& in, loom::Bus&) override {
        received_.push_back(in.payload); // buffer EVERY received Value, generically
    }
    loom::Value snapshot() const override {
        loom::Value v(console_state_schema());
        v.set("received", loom::Cell::integer(static_cast<std::int64_t>(received_.size())));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}
    const std::vector<loom::Value>& received() const noexcept { return received_; }

private:
    std::vector<loom::Value> received_;
};

ConsoleEngine::ConsoleEngine(loom::Switchboard& bus) : bus_(bus) {
    auto weave = std::make_unique<ConsoleWeave>();
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
    tap_.push_back(std::move(t));

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

ShapeDesc describe_schema(const loom::Schema& schema) {
    ShapeDesc d;
    d.name = schema.name();
    d.version = schema.version();
    for (const loom::Field& f : schema.fields()) {
        d.fields.push_back({f.name, type_string(f.type), f.required});
    }
    return d;
}

std::optional<ShapeDesc> ConsoleEngine::describe(std::string_view name,
                                                 std::uint32_t version) const {
    std::shared_ptr<const loom::Schema> schema = bus_.resolve_schema(name, version);
    if (!schema) {
        return std::nullopt;
    }
    return describe_schema(*schema);
}

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
    // Gated send AS the console: send_as stamps the console as sender and authorizes against
    // the console's grant; reply_to is the console so replies route back; the gate admits
    // against the target's accept-set at delivery.
    return bus_.send_as(
        console_id_, target,
        loom::Message(std::move(v), loom::WeaveId{}, console_id_, ++correlation_));
}

loom::Ticket ConsoleEngine::submit(loom::WeaveId target, std::string_view name,
                                      std::uint32_t version,
                                      const std::map<std::string, FieldValue>& fields,
                                      std::string* error) {
    const auto fail = [&](const std::string& m) {
        if (error != nullptr) {
            *error = m;
        }
        return loom::Ticket{};
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
    return assemble_and_send(target, schema, cells);
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

std::size_t ConsoleEngine::buffer_size() const { return weave_->received().size(); }

std::optional<BufferEntry> ConsoleEngine::buffer_at(std::size_t one_based_index) const {
    const std::vector<loom::Value>& buf = weave_->received();
    if (one_based_index == 0 || one_based_index > buf.size()) {
        return std::nullopt;
    }
    const loom::Value& v = buf[one_based_index - 1];
    BufferEntry e{"m" + std::to_string(one_based_index), v.schema().name(), v.schema().version(),
                  v};
    return e;
}

void ConsoleEngine::pump() { bus_.pump(); }

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
    return run_compose_ladder(*this, target, name, version, args);
}

Composed run_compose_ladder(LadderHost& host, loom::WeaveId target, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args) {
    Composed result;
    const auto error = [&](const std::string& m) {
        result.status = Composed::Status::Error;
        result.error = m;
        return result;
    };
    std::shared_ptr<const loom::Schema> schema = host.resolve_schema(name, version);
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
            std::optional<loom::Cell> cell = host.resolve_ref(*ref, &rerr);
            if (!cell) {
                return error(rerr);
            }
            const loom::Kind k = cell->kind();
            nargs.push_back(NArg{a.name, k, std::move(*cell)});
        } else {
            const FieldValue& lit = std::get<FieldValue>(a.value);
            nargs.push_back(NArg{a.name, literal_kind(lit), lit});
        }
    }

    std::map<std::string, loom::Cell> assigned;

    // Rung 1: named wins.
    std::vector<const NArg*> bare;
    for (const NArg& a : nargs) {
        if (a.name) {
            const loom::Field* f = schema->find(*a.name);
            if (f == nullptr) {
                return error("shape " + schema->name() + " has no field '" + *a.name + "'");
            }
            if (assigned.count(*a.name) != 0) {
                return error("field '" + *a.name + "' assigned more than once");
            }
            std::optional<loom::Cell> cell = place(*f, a);
            if (!cell) {
                return error("field '" + *a.name + "' (" + loom::name_of(f->type.kind) +
                             ") cannot take this value");
            }
            assigned.insert_or_assign(*a.name, std::move(*cell));
        } else {
            bare.push_back(&a);
        }
    }

    // Open fields, in declaration order.
    std::vector<const loom::Field*> open;
    for (const loom::Field& f : schema->fields()) {
        if (assigned.count(f.name) == 0) {
            open.push_back(&f);
        }
    }

    // Rungs 2/3: place the bare args — positional, else type-directed, else prompt.
    if (!bare.empty()) {
        bool placed = false;

        // Rung 2: positional (bare[i] → open[i]); accept only if EVERY one fits, else fall.
        if (bare.size() <= open.size()) {
            std::vector<loom::Cell> cells;
            bool all = true;
            for (std::size_t i = 0; i < bare.size(); ++i) {
                std::optional<loom::Cell> c = place(*open[i], *bare[i]);
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
                    const loom::Field* f = open[chosen[i]];
                    assigned.insert_or_assign(f->name, std::move(*place(*f, *bare[i])));
                }
                std::vector<const loom::Field*> remaining;
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
            for (const loom::Field* f : open) {
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
    for (const loom::Field* f : open) {
        if (f->required) {
            any_required_open = true;
        }
    }
    if (any_required_open) {
        result.status = Composed::Status::NeedsInput;
        for (const loom::Field* f : open) {
            result.open_fields.push_back({f->name, type_string(f->type), f->required});
        }
        return result; // unplaced empty: every arg was placed; only required fields remain
    }

    // Ready: assemble + gate-send (the gate is the unconditional backstop).
    result.status = Composed::Status::Ready;
    result.ticket = host.assemble_and_send(target, schema, assigned);
    return result;
}

} // namespace loom
