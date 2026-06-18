#include <zen/switchboard/switchboard.hpp>

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace zen::sb {

// ---- Refusal --------------------------------------------------------------

const char* name_of(RefusalReason r) noexcept {
    switch (r) {
    case RefusalReason::None:
        return "None";
    case RefusalReason::NoSuchTarget:
        return "NoSuchTarget";
    case RefusalReason::TargetUnavailable:
        return "TargetUnavailable";
    case RefusalReason::NotAccepted:
        return "NotAccepted";
    case RefusalReason::GateRefused:
        return "GateRefused";
    case RefusalReason::CapabilityDenied:
        return "CapabilityDenied";
    }
    return "?";
}

std::string Refusal::message() const {
    switch (reason) {
    case RefusalReason::None:
        return "no refusal";
    case RefusalReason::NoSuchTarget:
        return "no such target shard";
    case RefusalReason::TargetUnavailable:
        return "target shard is dead (awaiting revival)";
    case RefusalReason::NotAccepted:
        return "target does not accept this schema";
    case RefusalReason::GateRefused:
        return "gate refused: " + error.message();
    case RefusalReason::CapabilityDenied:
        return "sender's grant does not permit this shape to this target";
    }
    return "?";
}

// ---- The fixed lifecycle-policy grammar -----------------------------------

std::shared_ptr<const Schema> lifecycle_policy_schema() {
    static const std::shared_ptr<const Schema> schema =
        SchemaBuilder("LifecyclePolicy", 1)
            .field("max_reloads", Kind::Int)
            .field("revive_from_last_good", Kind::Bool)
            .build();
    return schema;
}

// ---- Switchboard ----------------------------------------------------------

Switchboard::Switchboard() {
    journal_.push_back(DeliveryOutcome{}); // slot 0 is unused; seqs start at 1
}

Switchboard::~Switchboard() = default;

Switchboard::ShardRecord* Switchboard::find(ShardId id) {
    auto it = shards_.find(id.value);
    return it == shards_.end() ? nullptr : &it->second;
}

const Switchboard::ShardRecord* Switchboard::find(ShardId id) const {
    auto it = shards_.find(id.value);
    return it == shards_.end() ? nullptr : &it->second;
}

const std::shared_ptr<const Schema>* Switchboard::accept_match(const ShardRecord& rec,
                                                              std::string_view name,
                                                              std::uint32_t version) {
    for (const auto& s : rec.accept) {
        if (s->name() == name && s->version() == version) {
            return &s;
        }
    }
    return nullptr;
}

ShardId Switchboard::register_shard(std::unique_ptr<Shard> incoming) {
    return register_shard(std::move(incoming), Grant{}); // empty grant: minimal authority
}

ShardId Switchboard::register_shard(std::unique_ptr<Shard> incoming, Grant grant) {
    if (!incoming) {
        throw std::invalid_argument("register_shard: shard must be non-null");
    }

    // Record the accept-set, registering each schema so all Shards agree on what
    // a given (name, version) means (a disagreement throws zen::SchemaConflict).
    std::vector<std::shared_ptr<const Schema>> accept;
    auto declared = incoming->accepted_schemas();
    accept.reserve(declared.size());
    for (auto& s : declared) {
        if (!s) {
            throw std::invalid_argument("register_shard: a declared accept schema is null");
        }
        accept.push_back(registry_.register_schema(s).schema);
    }

    // Seed last-known-good from an initial snapshot, gated against its own schema.
    Value snap = incoming->snapshot();
    std::shared_ptr<const Schema> state_schema = snap.schema_ptr();
    registry_.register_schema(state_schema);
    Admission seeded = zen::admit(std::move(snap), *state_schema);
    if (!seeded.ok()) {
        throw std::invalid_argument("register_shard: initial snapshot does not conform to its "
                                    "own schema: " +
                                    seeded.first_error().message());
    }

    ShardId id{next_shard_id_++};
    ShardRecord rec{id,
                    std::move(incoming),
                    std::move(accept),
                    state_schema,
                    std::move(seeded).value(),
                    std::move(grant),
                    0,
                    true};
    shards_.emplace(id.value, std::move(rec));
    return id;
}

std::unique_ptr<Shard> Switchboard::unregister_shard(ShardId id) {
    auto it = shards_.find(id.value);
    if (it == shards_.end()) {
        return nullptr;
    }
    std::unique_ptr<Shard> released = std::move(it->second.shard);
    shards_.erase(it);
    return released;
}

Ticket Switchboard::enqueue_directed(ShardId target, Message msg, bool gated) {
    const std::uint64_t seq = next_seq_++;
    journal_.push_back(DeliveryOutcome{}); // Pending at index seq
    queue_.push_back(Envelope{std::move(msg), target, seq, gated});
    return Ticket{seq};
}

std::size_t Switchboard::fanout(Message msg, bool gated) {
    const std::string name(msg.payload.schema().name());
    const std::uint32_t version = msg.payload.schema().version();

    std::size_t recipients = 0;
    for (auto& entry : shards_) { // std::map: ascending id == registration order
        ShardRecord& rec = entry.second;
        if (!rec.alive) {
            continue;
        }
        if (accept_match(rec, name, version) == nullptr) {
            continue;
        }
        const std::uint64_t seq = next_seq_++;
        journal_.push_back(DeliveryOutcome{});
        queue_.push_back(Envelope{
            Message(msg.payload, msg.sender, msg.reply_to, msg.correlation), rec.id, seq, gated});
        ++recipients;
    }
    return recipients;
}

// Host root authority: held only by the host program, these enqueue ungated.
Ticket Switchboard::send(ShardId target, Message msg) {
    return enqueue_directed(target, std::move(msg), /*gated=*/false);
}

std::size_t Switchboard::publish(Message msg) { return fanout(std::move(msg), /*gated=*/false); }

// The path a Shard's ShardBus uses: stamp the authoritative sender (a Shard
// cannot send as anyone else) and enqueue gated, to be authorized at delivery.
Ticket Switchboard::gated_send(ShardId sender, ShardId target, Message msg) {
    msg.sender = sender;
    return enqueue_directed(target, std::move(msg), /*gated=*/true);
}

std::size_t Switchboard::gated_publish(ShardId sender, Message msg) {
    msg.sender = sender;
    return fanout(std::move(msg), /*gated=*/true);
}

void Switchboard::record(std::uint64_t seq, Disposition disposition, const Refusal& refusal) {
    if (seq < journal_.size()) {
        journal_[static_cast<std::size_t>(seq)] = DeliveryOutcome{disposition, refusal};
    }
}

void Switchboard::emit(const BusEvent& event) {
    for (auto& observer : observers_) {
        if (observer.second) {
            observer.second(event);
        }
    }
}

void Switchboard::deliver_one(Envelope env) {
    BusEvent ev;
    ev.seq = env.seq;
    ev.target = env.target;
    ev.sender = env.msg.sender;
    ev.schema_name = env.msg.payload.schema().name();
    ev.schema_version = env.msg.payload.schema().version();

    ShardRecord* rec = find(env.target);
    if (rec == nullptr) {
        const Refusal r{RefusalReason::NoSuchTarget, {}};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }
    if (!rec->alive) {
        const Refusal r{RefusalReason::TargetUnavailable, {}};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }

    // Capability authorization — only for Shard-originated (gated) messages, and
    // *before* the gate, so a denied message never reaches conformance (the gate
    // is correctly not invoked for it). Host-injected (root) messages skip this.
    // This is authorization ("are you allowed to send this to them"), categorically
    // distinct from the gate's conformance question, and lives outside it.
    if (env.gated) {
        const ShardRecord* sender = find(env.msg.sender);
        const bool permitted =
            sender != nullptr && sender->grant.permits(ev.schema_name, ev.schema_version, env.target);
        if (!permitted) {
            const Refusal r{RefusalReason::CapabilityDenied, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
    }

    const std::shared_ptr<const Schema>* door =
        accept_match(*rec, ev.schema_name, ev.schema_version);
    if (door == nullptr) {
        const Refusal r{RefusalReason::NotAccepted, {}};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }

    // The one gate, live path. admit() consumes the candidate and re-emits it
    // trusted on success; on failure the candidate is dropped and never seen.
    Admission a = zen::admit(std::move(env.msg.payload), **door);
    if (!a.ok()) {
        const Refusal r{RefusalReason::GateRefused, a.first_error()};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }

    Message trusted(std::move(a).value(), env.msg.sender, env.msg.reply_to, env.msg.correlation);
    // The handler receives a ShardBus bound to its own id — never the concrete
    // Switchboard — so anything it sends is stamped with its identity and gated
    // against its grant.
    ShardBus shard_bus(*this, env.target);
    rec->shard->handle(trusted, shard_bus); // may enqueue further deliveries
    record(env.seq, Disposition::Delivered, Refusal{});
    ev.kind = EventKind::Delivered;
    ev.payload = &trusted.payload;
    emit(ev);
}

void Switchboard::pump() {
    if (in_dispatch_) {
        return; // non-reentrant: a handler's sends were enqueued, not nested
    }
    in_dispatch_ = true;
    stop_requested_ = false;
    while (!queue_.empty() && !stop_requested_) {
        Envelope env = std::move(queue_.front());
        queue_.pop_front();
        deliver_one(std::move(env));
    }
    in_dispatch_ = false;
}

DeliveryOutcome Switchboard::outcome(Ticket t) const {
    if (t.seq == 0 || t.seq >= journal_.size()) {
        return DeliveryOutcome{};
    }
    return journal_[static_cast<std::size_t>(t.seq)];
}

ObserverId Switchboard::add_observer(Observer obs) {
    const ObserverId id = next_observer_id_++;
    observers_.emplace_back(id, std::move(obs));
    return id;
}

void Switchboard::remove_observer(ObserverId id) {
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
        if (it->first == id) {
            observers_.erase(it);
            return;
        }
    }
}

std::string Switchboard::snapshot_bytes(ShardId id) const {
    const ShardRecord* rec = find(id);
    if (rec == nullptr) {
        throw std::invalid_argument("snapshot_bytes: no such shard");
    }
    return zen::serialize(rec->shard->snapshot());
}

void Switchboard::kill(ShardId id) {
    ShardRecord* rec = find(id);
    if (rec == nullptr) {
        return;
    }
    rec->alive = false;
    BusEvent ev;
    ev.kind = EventKind::Died;
    ev.target = id;
    ev.schema_name = rec->state_schema->name();
    ev.schema_version = rec->state_schema->version();
    emit(ev);
}

ReviveOutcome Switchboard::reload(ShardId id, std::string_view candidate_bytes) {
    ReviveOutcome out;
    ShardRecord* rec = find(id);
    if (rec == nullptr) {
        out.refusal = Refusal{RefusalReason::NoSuchTarget, {}};
        return out;
    }

    // The self's lock must itself be a well-formed policy.
    Admission pol = zen::admit(rec->shard->policy(), *lifecycle_policy_schema());
    if (!pol.ok()) {
        out.policy_malformed = true;
        out.refusal = Refusal{RefusalReason::GateRefused, pol.first_error()};
        return out;
    }
    const std::int64_t max_reloads = pol.value().get("max_reloads")->as_int();
    const bool revive_from_last_good = pol.value().get("revive_from_last_good")->as_bool();

    if (static_cast<std::int64_t>(rec->reloads_used) >= max_reloads) {
        out.reloads_exhausted = true;
        return out;
    }

    auto announce = [&](bool from_lkg, const Refusal& refusal) {
        BusEvent ev;
        ev.kind = EventKind::Revived;
        ev.target = id;
        ev.schema_name = rec->state_schema->name();
        ev.schema_version = rec->state_schema->version();
        ev.from_last_known_good = from_lkg;
        ev.refusal = refusal;
        emit(ev);
    };

    // The bytes path: parse -> admit(Unverified, state schema). Same gate as live.
    Unverified candidate = zen::parse(candidate_bytes);
    Admission admitted = zen::admit(candidate, rec->state_schema);
    if (admitted.ok()) {
        Value state = std::move(admitted).value();
        rec->shard->revive(state);
        rec->last_known_good = state;
        ++rec->reloads_used;
        rec->alive = true;
        out.revived = true;
        announce(/*from_lkg=*/false, Refusal{});
        return out;
    }

    // The candidate was refused. The policy decides whether the self may return
    // as its last-known-good.
    out.refusal = Refusal{RefusalReason::GateRefused, admitted.first_error()};
    if (revive_from_last_good) {
        rec->shard->revive(rec->last_known_good);
        ++rec->reloads_used;
        rec->alive = true;
        out.revived = true;
        out.from_last_known_good = true;
        announce(/*from_lkg=*/true, out.refusal);
        return out;
    }

    BusEvent ev;
    ev.kind = EventKind::Refused;
    ev.target = id;
    ev.schema_name = rec->state_schema->name();
    ev.schema_version = rec->state_schema->version();
    ev.refusal = out.refusal;
    emit(ev);
    return out;
}

ReviveOutcome Switchboard::swap_state(ShardId id, std::string_view candidate_bytes) {
    ReviveOutcome out;
    ShardRecord* rec = find(id);
    if (rec == nullptr) {
        out.refusal = Refusal{RefusalReason::NoSuchTarget, {}};
        return out;
    }

    auto announce = [&](EventKind kind, const Refusal& refusal) {
        BusEvent ev;
        ev.kind = kind;
        ev.target = id;
        ev.schema_name = rec->state_schema->name();
        ev.schema_version = rec->state_schema->version();
        ev.from_last_known_good = false;
        ev.refusal = refusal;
        emit(ev);
    };

    // Same gate as the live and crash-revival paths: parse -> admit(Unverified,
    // state schema). No policy is consulted: an intentional swap spends no budget.
    Unverified candidate = zen::parse(candidate_bytes);
    Admission admitted = zen::admit(candidate, rec->state_schema);
    if (!admitted.ok()) {
        // A clean refusal — no last-known-good fallback for an intentional swap.
        out.refusal = Refusal{RefusalReason::GateRefused, admitted.first_error()};
        announce(EventKind::Refused, out.refusal);
        return out;
    }

    Value state = std::move(admitted).value();
    rec->shard->revive(state);
    rec->last_known_good = state;
    rec->alive = true;
    out.revived = true;
    announce(EventKind::Revived, Refusal{});
    return out;
}

std::vector<ShardId> Switchboard::list_shards() const {
    std::vector<ShardId> ids;
    ids.reserve(shards_.size());
    for (const auto& entry : shards_) {
        ids.push_back(entry.second.id);
    }
    return ids;
}

std::vector<std::shared_ptr<const Schema>> Switchboard::accepted_schemas(ShardId id) const {
    const ShardRecord* rec = find(id);
    if (rec == nullptr) {
        return {};
    }
    return rec->accept;
}

std::shared_ptr<const Schema> Switchboard::resolve_schema(std::string_view name,
                                                          std::uint32_t version) const {
    return registry_.lookup(name, version);
}

Shard* Switchboard::shard(ShardId id) {
    ShardRecord* rec = find(id);
    return rec == nullptr ? nullptr : rec->shard.get();
}

const Shard* Switchboard::shard(ShardId id) const {
    const ShardRecord* rec = find(id);
    return rec == nullptr ? nullptr : rec->shard.get();
}

bool Switchboard::alive(ShardId id) const {
    const ShardRecord* rec = find(id);
    return rec != nullptr && rec->alive;
}

} // namespace zen::sb
