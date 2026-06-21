#include <zen/console/console.hpp>

#include <zen/kind.hpp>

#include <string>
#include <utility>

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

} // namespace zen::console
