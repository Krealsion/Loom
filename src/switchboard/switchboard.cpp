#include <zen/switchboard/switchboard.hpp>

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace loom {

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
    case RefusalReason::ForeignAuthority:
        return "ForeignAuthority";
    case RefusalReason::Exhausted:
        return "Exhausted";
    }
    return "?";
}

std::string Refusal::message() const {
    switch (reason) {
    case RefusalReason::None:
        return "no refusal";
    case RefusalReason::NoSuchTarget:
        return "no such target weave";
    case RefusalReason::TargetUnavailable:
        return "target weave is dead (awaiting revival)";
    case RefusalReason::NotAccepted:
        return "target does not accept this schema";
    case RefusalReason::GateRefused:
        return "gate refused: " + error.message();
    case RefusalReason::CapabilityDenied:
        return "sender's grant does not permit this shape to this target";
    case RefusalReason::ForeignAuthority:
        // Deliberately says AUTHORITY, not grant: the sender may hold exactly the
        // right grant and still be presenting an authority this Loom never issued.
        return "lifecycle/answer authority was not issued here, or is expired, "
               "spent, or bound to a different conversation";
    case RefusalReason::Exhausted:
        // Says CAPACITY, and says which bound, because the fix is a different one:
        // nothing here is wrong with the sender, the shape, or the authority.
        return "a published bound was reached (deferred answers in flight)";
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

Switchboard::Switchboard()
    // THIS LOOM'S IDENTITY, minted once here and never again. `new` rather than
    // make_shared because LoomIdentity's constructor is private to this class —
    // which is the point: nobody else can make one, so nobody else can produce a
    // value that compares equal to it.
    : identity_(std::shared_ptr<const LoomIdentity>(new LoomIdentity{})) {
    // A fixed-size ring, allocated once: the journal's footprint is bounded by
    // kJournalCapacity for the life of the bus, never by how many messages it ever
    // carries (audit F-6). Slots start seq=0 ("never written"); real seqs start at 1.
    journal_.assign(kJournalCapacity, JournalSlot{});
}

Switchboard::~Switchboard() = default;

Switchboard::WeaveRecord* Switchboard::find(WeaveId id) {
    auto it = weaves_.find(id.value);
    return it == weaves_.end() ? nullptr : &it->second;
}

const Switchboard::WeaveRecord* Switchboard::find(WeaveId id) const {
    auto it = weaves_.find(id.value);
    return it == weaves_.end() ? nullptr : &it->second;
}

const std::shared_ptr<const Schema>* Switchboard::accept_match(const WeaveRecord& rec,
                                                              std::string_view name,
                                                              std::uint32_t version) {
    for (const auto& s : rec.accept) {
        if (s->name() == name && s->version() == version) {
            return &s;
        }
    }
    return nullptr;
}

WeaveId Switchboard::register_weave(std::unique_ptr<Weave> incoming) {
    return register_weave(std::move(incoming), Grant{}); // empty grant: minimal authority
}

WeaveId Switchboard::register_weave(std::unique_ptr<Weave> incoming, Grant grant) {
    return register_weave(std::move(incoming), std::move(grant), std::string{});
}

WeaveId Switchboard::register_weave(std::unique_ptr<Weave> incoming, Grant grant, std::string role) {
    if (!incoming) {
        throw std::invalid_argument("register_weave: weave must be non-null");
    }
    if (!role.empty() && roles_.count(role) != 0) {
        throw std::invalid_argument("register_weave: role '" + role +
                                    "' is already held (roles are singletons in this phase)");
    }

    // Record the accept-set, registering each schema so all Weaves agree on what
    // a given (name, version) means (a disagreement throws loom::SchemaConflict).
    std::vector<std::shared_ptr<const Schema>> accept;
    auto declared = incoming->accepted_schemas();
    accept.reserve(declared.size());
    for (auto& s : declared) {
        if (!s) {
            throw std::invalid_argument("register_weave: a declared accept schema is null");
        }
        accept.push_back(registry_.register_schema(s).schema);
    }

    // Seed last-known-good from an initial snapshot, gated against its own schema.
    Value snap = incoming->snapshot();
    std::shared_ptr<const Schema> state_schema = snap.schema_ptr();
    registry_.register_schema(state_schema);
    Admission seeded = loom::admit(std::move(snap), *state_schema);
    if (!seeded.ok()) {
        throw std::invalid_argument("register_weave: initial snapshot does not conform to its "
                                    "own schema: " +
                                    seeded.first_error().message());
    }

    WeaveId id{next_weave_id_++};
    WeaveRecord rec{id,
                    std::move(incoming),
                    std::move(accept),
                    state_schema,
                    std::move(seeded).value(),
                    std::move(grant),
                    0,
                    true,
                    /*incarnation=*/1, // this id's first code; bumped by swap_state alone
                    role};
    weaves_.emplace(id.value, std::move(rec));
    if (!role.empty()) {
        roles_.emplace(std::move(role), id);
    }
    return id;
}

WeaveId Switchboard::register_weave(std::unique_ptr<Weave> incoming, Grant grant,
                                    AcceptMode accept_mode) {
    WeaveId id = register_weave(std::move(incoming), std::move(grant), std::string{});
    if (accept_mode == AcceptMode::AnyRegistered) {
        auto it = weaves_.find(id.value);
        if (it != weaves_.end()) {
            it->second.accepts_any = true; // accept any registered shape, gated at delivery
        }
    }
    return id;
}

std::unique_ptr<Weave> Switchboard::unregister_weave(WeaveId id) {
    auto it = weaves_.find(id.value);
    if (it == weaves_.end()) {
        return nullptr;
    }
    if (!it->second.role.empty()) {
        roles_.erase(it->second.role); // a role has no holder once its Weave is removed
    }
    std::unique_ptr<Weave> released = std::move(it->second.weave);
    weaves_.erase(it);
    // Its unfinished conversations end with it, in both directions: it can no
    // longer answer, and nothing can be answered TO it. Unconditional (R2B-2a):
    // this used to rely on the staleness sweep happening to see incarnation 0
    // because the record was already erased above — true, but true only by call
    // order. Permanent removal is the end of a life, so it asks the death question.
    abandon_deferred_for(id);
    return released;
}

// EVERY ENQUEUE PATH DECIDES THE PROVENANCE, and the ordinary ones decide
// "none" — unconditionally, overwriting whatever the caller's Message carried.
// That single assignment is what makes provenance unforgeable by ordinary weave
// code: a weave may construct a Message however it likes, and may even copy one
// it was delivered, but the moment it hands that Message to the bus the fact is
// erased. Only answer_as() and announce_as() pass a non-empty one.
Ticket Switchboard::enqueue_directed(WeaveId target, Message msg, bool gated,
                                     Provenance provenance) {
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    msg.provenance = provenance;
    queue_.push_back(Envelope{std::move(msg), target, seq, gated});
    return Ticket{seq};
}

Ticket Switchboard::enqueue_role(std::string role, Message msg, bool gated) {
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    msg.provenance = Provenance{};
    queue_.push_back(Envelope{std::move(msg), WeaveId{}, seq, gated, std::move(role)});
    return Ticket{seq};
}

Ticket Switchboard::refuse_now(WeaveId target, WeaveId sender, const Message& msg,
                               RefusalReason reason) {
    // A refusal that never became a delivery still gets a seq, a journal slot and
    // a tap event, so "this weave tried to answer without authority" is visible at
    // exactly the altitude "this weave tried to send without a grant" already is.
    const std::uint64_t seq = next_seq_++;
    const Refusal r{reason, {}};
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{Disposition::Refused, r}};
    BusEvent ev;
    ev.kind = EventKind::Refused;
    ev.seq = seq;
    ev.target = target;
    ev.sender = sender;
    ev.schema_name = msg.payload.schema().name();
    ev.schema_version = msg.payload.schema().version();
    ev.refusal = r;
    emit(ev);
    return Ticket{seq};
}

Ticket Switchboard::answer_as(WeaveId as_sender, Message msg) {
    // Three ways to have no authority, and each is a refusal of AUTHORITY —
    // categorically distinct from the grant check that still runs afterwards on
    // a legitimate answer:
    //   - nothing is being dispatched, or the caller is not the weave being
    //     dispatched (a Bus that outlived its delivery, or one belonging to
    //     another delivery entirely);
    //   - the request came from a root, so there is no requester to answer;
    //   - this delivery's one answer is already spent.
    if (!current_target_.valid() || as_sender != current_target_ ||
        !authority_.requester.valid() || authority_.spent) {
        // Visible on the tap AND honestly reported to the caller: an INVALID
        // ticket, because nothing was queued. A refusal ticket would tell a
        // responder its answer had been sent.
        (void)refuse_now(authority_.requester, as_sender, msg, RefusalReason::CapabilityDenied);
        return Ticket{};
    }
    authority_.spent = true;
    // Loom chooses the recipient and the correlation; the answerer chooses only
    // what it says. That is what binds the proof to ONE request from ONE weave.
    const WeaveId to = authority_.requester;
    msg.sender = as_sender;
    msg.reply_to = WeaveId{};
    msg.correlation = authority_.correlation;
    return enqueue_directed(to, std::move(msg), /*gated=*/true,
                            Provenance::attested(Provenance::Kind::Answer, 0));
}

// ---- deferred answers (R2B-2) ----------------------------------------------
//
// THE LAW: an answer may outlive the handler, but never the conversation or the
// incarnation that earned it.

std::uint64_t Switchboard::incarnation_of(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? 0 : rec->incarnation;
}

Switchboard::DeferredRecord* Switchboard::find_deferred(std::uint64_t token) {
    if (token == 0) {
        return nullptr;
    }
    for (DeferredRecord& r : deferred_) {
        if (r.token == token) {
            return &r;
        }
    }
    return nullptr;
}

void Switchboard::abandon_deferred_for(WeaveId id) {
    // A HANDLER MAY END WITHOUT ENDING THE CONVERSATION. A LIFE MAY NOT.
    //
    // This is the death half of R2B-2's law, and it needs its own function because
    // the staleness sweep below CANNOT express it: `kill` leaves the id and the
    // incarnation exactly as they were, so every record still looks perfectly
    // current. Without this, a crashed weave revived from its own snapshot — the
    // isolation supervisor's ordinary recovery path — would come back holding its
    // predecessor's answer rights.
    //
    // Unconditional, and in BOTH directions: it does not matter whether the dead
    // participant was the one who asked or the one who was going to answer. Nor
    // does it matter what happens next — revival, last-known-good fallback, or
    // permanent quarantine — because the slot is reclaimed HERE, at the transition,
    // rather than at some later event that may never arrive.
    //
    // Deliberately selective: only conversations this weave is a party to. Every
    // other conversation in the registry belongs to lives that did not end.
    for (DeferredRecord& r : deferred_) {
        if (r.token != 0 && (r.respondent == id || r.requester == id)) {
            r = DeferredRecord{}; // the slot is free again
        }
    }
}

void Switchboard::forget_deferred_for(WeaveId id) {
    // Called when NEW CODE is committed behind an existing id (`swap_state`), and
    // only there. That event ends every unfinished conversation the predecessor was
    // a party to: the incarnation that earned the right is gone, and a successor
    // does not inherit it. Death and permanent removal are a different question and
    // have their own function — see `abandon_deferred_for`, and note that THIS one
    // could not answer them: a killed weave keeps both its id and its incarnation,
    // so nothing about its records looks stale.
    //
    // This is also how a LEAKED capability is reclaimed: it costs one slot until
    // its owner's code is replaced or its life ends, and no longer.
    const std::uint64_t now = incarnation_of(id);
    for (DeferredRecord& r : deferred_) {
        if (r.token == 0) {
            continue;
        }
        const bool stale_respondent = r.respondent == id && r.respondent_incarnation != now;
        const bool stale_requester = r.requester == id && r.requester_incarnation != now;
        if (stale_respondent || stale_requester) {
            r = DeferredRecord{}; // the slot is free again
        }
    }
}

DeferredAnswer Switchboard::defer_answer_as(WeaveId as_sender) {
    // DEFERRING IS A CONVERSION, NOT AN ADDITION. It spends the immediate
    // opportunity, so one delivered request still grants exactly one answer: a
    // second defer_answer() finds nothing left to convert, and answer() finds
    // nothing left to send. A delivery that never earned answer authority — an
    // ordinary path, or a request from a root with nobody to answer — has nothing
    // to defer, and says so by returning an invalid capability.
    if (!current_target_.valid() || as_sender != current_target_ ||
        !authority_.requester.valid() || authority_.spent) {
        return DeferredAnswer{};
    }
    const std::uint64_t requester_incarnation = incarnation_of(authority_.requester);
    const std::uint64_t respondent_incarnation = incarnation_of(as_sender);
    if (requester_incarnation == 0 || respondent_incarnation == 0) {
        return DeferredAnswer{}; // one of the participants is already gone
    }

    DeferredRecord* slot = nullptr;
    for (DeferredRecord& r : deferred_) {
        if (r.token == 0) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        if (deferred_.size() >= kMaxDeferredAnswers) {
            // BOUNDED, AND THE OVERFLOW IS VISIBLE rather than a silent nothing:
            // the caller keeps its immediate opportunity (nothing was consumed),
            // and a tap sees a refusal that names the ask it could not defer and
            // says CAPACITY rather than implying a forgery.
            Message empty{Value(authority_.shape != nullptr ? authority_.shape
                                                            : lifecycle_policy_schema())};
            (void)refuse_now(authority_.requester, as_sender, empty, RefusalReason::Exhausted);
            return DeferredAnswer{};
        }
        deferred_.emplace_back();
        slot = &deferred_.back();
    }

    authority_.spent = true; // the immediate right is now the deferred one
    const std::uint64_t token = next_deferred_token_++;
    *slot = DeferredRecord{token,
                           authority_.requester,
                           requester_incarnation,
                           as_sender,
                           respondent_incarnation,
                           authority_.correlation};
    return DeferredAnswer{identity_, token};
}

Ticket Switchboard::spend_deferred_as(WeaveId as_sender, const DeferredAnswer& answer,
                                      Message msg) {
    // Board-relativity first: a capability minted by another Loom has no standing
    // here even if its token happens to name a live record of ours. (Two symmetric
    // worlds really can mint the same token number — that is exactly the trap.)
    if (!issued_here_deferred(answer)) {
        (void)refuse_now(WeaveId{}, as_sender, msg, RefusalReason::ForeignAuthority);
        return Ticket{};
    }
    DeferredRecord* rec = find_deferred(answer.opaque_token());
    // EVERY TERM MATTERS, and each one is a different attack:
    //   - the record exists and has not been spent or released;
    //   - the speaker IS the bound respondent (not a successor, not the current
    //     role holder, not another weave holding copied public values);
    //   - the respondent is STILL THE SAME INCARNATION (a reload behind the same
    //     id is a different one, and does not inherit the conversation);
    //   - the requester still exists AT THE INCARNATION THAT ASKED, so an answer
    //     cannot be delivered to a successor that happens to hold the same id.
    if (rec == nullptr || as_sender != rec->respondent ||
        incarnation_of(as_sender) != rec->respondent_incarnation) {
        (void)refuse_now(rec == nullptr ? WeaveId{} : rec->requester, as_sender, msg,
                         RefusalReason::ForeignAuthority);
        return Ticket{};
    }
    if (incarnation_of(rec->requester) != rec->requester_incarnation) {
        const WeaveId to = rec->requester;
        *rec = DeferredRecord{}; // the conversation is over either way
        (void)refuse_now(to, as_sender, msg, RefusalReason::NoSuchTarget);
        return Ticket{};
    }

    // CONSUMED BEFORE QUEUEING, so neither a replay nor a reentrant handler can
    // turn one right into two answers.
    const WeaveId to = rec->requester;
    const std::uint64_t correlation = rec->correlation;
    *rec = DeferredRecord{};

    msg.sender = as_sender;
    msg.reply_to = WeaveId{};
    msg.correlation = correlation; // the ORIGINAL request's, never the answerer's choice
    return enqueue_directed(to, std::move(msg), /*gated=*/true,
                            Provenance::attested(Provenance::Kind::Answer, 0));
}

void Switchboard::release_deferred_as(WeaveId as_sender, const DeferredAnswer& answer) {
    // Abandonment is SILENT to the requester in V1 — there is no cancellation
    // vocabulary, and inventing one here would be a protocol nobody asked for.
    // What it is not is silent to the bus: the slot is reclaimed at once.
    if (!issued_here_deferred(answer)) {
        return;
    }
    DeferredRecord* rec = find_deferred(answer.opaque_token());
    if (rec != nullptr && as_sender == rec->respondent &&
        incarnation_of(as_sender) == rec->respondent_incarnation) {
        *rec = DeferredRecord{};
    }
}

Ticket Switchboard::announce_as(WeaveId as_sender, const LifecycleAuthority& authority,
                                WeaveId target, Message msg, std::int64_t sequence) {
    // AUTHORITY IS RELATIVE TO THE LOOM THAT ISSUED IT.
    //
    // R2B-1a made minting require a Switchboard; it did not make the result mean
    // anything about WHICH Switchboard. The authority was an empty marker, so
    // this function took one and ignored it — and an ordinary weave could stand
    // up a decoy board of its own, mint a genuine authority from it, and spend it
    // here. Constructing that decoy is legal and stays legal: a Switchboard is an
    // ordinary object. What it cannot be is THIS Loom.
    //
    // So the check is the issuer, and it lives here rather than in any consumer:
    // holding an authority gives you no way to ask the question, and no standing
    // to answer it. `issued_here` also fails for an authority whose board has
    // been destroyed — the lifetime rule, not a special case.
    if (!issued_here(authority)) {
        (void)refuse_now(target, as_sender, msg, RefusalReason::ForeignAuthority);
        return Ticket{};
    }
    // The attestation is bound to THIS target and THIS sequence, both taken from
    // the call rather than from the payload.
    if (!target.valid()) {
        (void)refuse_now(target, as_sender, msg, RefusalReason::NoSuchTarget);
        return Ticket{};
    }
    msg.sender = as_sender;
    return enqueue_directed(target, std::move(msg), /*gated=*/true,
                            Provenance::attested(Provenance::Kind::Activation, sequence));
}

std::size_t Switchboard::fanout(Message msg, bool gated) {
    const std::string name(msg.payload.schema().name());
    const std::uint32_t version = msg.payload.schema().version();

    std::size_t recipients = 0;
    for (auto& entry : weaves_) { // std::map: ascending id == registration order
        WeaveRecord& rec = entry.second;
        if (!rec.alive) {
            continue;
        }
        if (accept_match(rec, name, version) == nullptr) {
            continue;
        }
        const std::uint64_t seq = next_seq_++;
        journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
        // Rebuilt field by field, which also means a published Message carries no
        // provenance whatever the caller's copy held.
        queue_.push_back(Envelope{
            Message(msg.payload, msg.sender, msg.reply_to, msg.correlation), rec.id, seq, gated});
        ++recipients;
    }
    return recipients;
}

// Host root authority: held only by the host program, these enqueue ungated.
Ticket Switchboard::send(WeaveId target, Message msg) {
    return enqueue_directed(target, std::move(msg), /*gated=*/false);
}

std::size_t Switchboard::publish(Message msg) { return fanout(std::move(msg), /*gated=*/false); }

// The gated path a Weave's WeaveBus uses, and the host uses to re-enter a
// child's output: stamp the authoritative sender (a Weave cannot send as anyone
// else) and enqueue gated, to be authorized against that sender's grant at
// delivery.
Ticket Switchboard::send_as(WeaveId as_sender, WeaveId target, Message msg) {
    msg.sender = as_sender;
    return enqueue_directed(target, std::move(msg), /*gated=*/true);
}

std::size_t Switchboard::publish_as(WeaveId as_sender, Message msg) {
    msg.sender = as_sender;
    return fanout(std::move(msg), /*gated=*/true);
}

// Role-addressed sends. send_to_role is the host's ungated root authority; the
// gated form (the WeaveBus path) stamps the authoritative sender and is authorized
// against that sender's grant by role at delivery.
Ticket Switchboard::send_to_role(std::string_view role, Message msg) {
    return enqueue_role(std::string(role), std::move(msg), /*gated=*/false);
}

Ticket Switchboard::send_as_to_role(WeaveId as_sender, std::string_view role, Message msg) {
    msg.sender = as_sender;
    return enqueue_role(std::string(role), std::move(msg), /*gated=*/true);
}

void Switchboard::record(std::uint64_t seq, Disposition disposition, const Refusal& refusal) {
    JournalSlot& slot = journal_[seq % kJournalCapacity];
    if (slot.seq == seq) {
        // Still the slot's owner. If a wrap past kJournalCapacity already evicted this
        // seq (only possible when a single pump outruns the window), the guard leaves
        // the newer owner untouched and this outcome is simply forgotten — never
        // misattributed. Read-immediately consumers never reach that depth.
        slot.outcome = DeliveryOutcome{disposition, refusal};
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

    // Capability authorization — only for Weave-originated (gated) messages, and
    // *before* role resolution and the gate, so a denied message never reaches
    // either. Host-injected (root) messages skip this. This is authorization
    // ("are you allowed to send this"), categorically distinct from the gate's
    // conformance question. A role-targeted send is authorized by *role* (the stable
    // slot the rule names — unspoofable, reload-stable); a direct send by WeaveId.
    // Authorizing before resolution means an unauthorized sender cannot even learn
    // whether the role is currently held.
    if (env.gated) {
        const WeaveRecord* sender = find(env.msg.sender);
        const bool permitted =
            sender != nullptr &&
            (env.role.empty()
                 ? sender->grant.permits(ev.schema_name, ev.schema_version, env.target)
                 : sender->grant.permits_role(ev.schema_name, ev.schema_version, env.role));
        if (!permitted) {
            const Refusal r{RefusalReason::CapabilityDenied, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
    }

    // Resolve a role target to its current holder (singleton in this phase). An
    // unheld role degrades exactly like an unknown WeaveId — NoSuchTarget, never the
    // gate — so a crashed/unmounted broker is "unavailable", not a hole.
    if (!env.role.empty()) {
        auto it = roles_.find(env.role);
        if (it == roles_.end()) {
            const Refusal r{RefusalReason::NoSuchTarget, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
        env.target = it->second;
        ev.target = env.target;
    }

    WeaveRecord* rec = find(env.target);
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

    const std::shared_ptr<const Schema>* door =
        accept_match(*rec, ev.schema_name, ev.schema_version);
    // Wildcard-accept (a deliberate capability — the console): a Weave registered
    // AcceptMode::AnyRegistered accepts any shape it does not explicitly list, gated
    // against the shape's OWN registry-resolved schema. An unregistered shape resolves
    // to null and is still refused — an unknown shape reaches no one, not even the
    // console. This widens the door set, it never skips the gate.
    std::shared_ptr<const Schema> wildcard_door;
    if (door == nullptr && rec->accepts_any) {
        wildcard_door = resolve_schema(ev.schema_name, ev.schema_version);
    }
    if (door == nullptr && !wildcard_door) {
        const Refusal r{RefusalReason::NotAccepted, {}};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }

    // The one gate, live path. admit() consumes the candidate and re-emits it
    // trusted on success; on failure the candidate is dropped and never seen.
    const Schema& door_schema = door != nullptr ? **door : *wildcard_door;
    Admission a = loom::admit(std::move(env.msg.payload), door_schema);
    if (!a.ok()) {
        const Refusal r{RefusalReason::GateRefused, a.first_error()};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
        return;
    }

    Message trusted(std::move(a).value(), env.msg.sender, env.msg.reply_to, env.msg.correlation);
    trusted.provenance = env.msg.provenance; // Loom's own word, set at enqueue and only there
    // THE REPLY AUTHORITY IS THIS STACK FRAME. It is created after routing has
    // chosen the recipient — so it names the incarnation that ACTUALLY received
    // the request, not the one the sender guessed or the one the role names now —
    // and it dies when the handler returns. A role changing hands after this
    // point hands the new holder nothing: it never received this request.
    current_target_ = env.target;
    authority_ = ReplyAuthority{env.msg.sender, env.msg.correlation, /*spent=*/false,
                                trusted.payload.schema_ptr()};
    // The handler receives a WeaveBus bound to its own id — never the concrete
    // Switchboard — so anything it sends is stamped with its identity and gated
    // against its grant.
    WeaveBus weave_bus(*this, env.target);
    rec->weave->handle(trusted, weave_bus); // may enqueue further deliveries
    // The authority does not outlive the handler. Cleared even on the throwing
    // path would be better still, but handle() escaping is already a programming
    // bug that unwinds past the pump; what matters here is that no ordinary
    // return leaves an answerable delivery behind.
    current_target_ = WeaveId{};
    authority_ = ReplyAuthority{};
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
    if (t.seq == 0 || t.seq >= next_seq_) {
        return DeliveryOutcome{}; // the invalid ticket, or a seq never issued
    }
    const JournalSlot& slot = journal_[t.seq % kJournalCapacity];
    if (slot.seq != t.seq) {
        return DeliveryOutcome{}; // evicted: older than the retained window (Pending, as for unknown)
    }
    return slot.outcome;
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

std::string Switchboard::snapshot_bytes(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        throw std::invalid_argument("snapshot_bytes: no such weave");
    }
    return loom::serialize(rec->weave->snapshot());
}

void Switchboard::kill(WeaveId id) {
    WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        return;
    }
    rec->alive = false;
    // Committing the death includes ending its unfinished conversations (R2B-2a),
    // and that happens BEFORE the announcement so that anything observing `Died`
    // sees a world in which those conversations are already over.
    abandon_deferred_for(id);
    BusEvent ev;
    ev.kind = EventKind::Died;
    ev.target = id;
    ev.schema_name = rec->state_schema->name();
    ev.schema_version = rec->state_schema->version();
    emit(ev);
}

ReviveOutcome Switchboard::reload(WeaveId id, std::string_view candidate_bytes) {
    ReviveOutcome out;
    WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        out.refusal = Refusal{RefusalReason::NoSuchTarget, {}};
        return out;
    }

    // The self's lock must itself be a well-formed policy.
    Admission pol = loom::admit(rec->weave->policy(), *lifecycle_policy_schema());
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
    Unverified candidate = loom::parse(candidate_bytes);
    Admission admitted = loom::admit(candidate, rec->state_schema);
    if (admitted.ok()) {
        Value state = std::move(admitted).value();
        rec->weave->revive(state);
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
        rec->weave->revive(rec->last_known_good);
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

ReviveOutcome Switchboard::swap_state(WeaveId id, std::string_view candidate_bytes) {
    ReviveOutcome out;
    WeaveRecord* rec = find(id);
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
    Unverified candidate = loom::parse(candidate_bytes);
    Admission admitted = loom::admit(candidate, rec->state_schema);
    if (!admitted.ok()) {
        // A clean refusal — no last-known-good fallback for an intentional swap.
        out.refusal = Refusal{RefusalReason::GateRefused, admitted.first_error()};
        announce(EventKind::Refused, out.refusal);
        return out;
    }

    Value state = std::move(admitted).value();
    rec->weave->revive(state);
    rec->last_known_good = state;
    rec->alive = true;
    // NEW CODE BEHIND A STABLE ID IS A NEW INCARNATION (R2B-2). The WeaveId is
    // deliberately unchanged — that is what reload means — so this counter is the
    // only thing that distinguishes the successor from the incarnation that may
    // have earned a deferred answer. Bumping it here, then forgetting that
    // weave's unfinished conversations, is what keeps handler-surviving authority
    // from quietly becoming reload-surviving authority.
    ++rec->incarnation;
    forget_deferred_for(id);
    out.revived = true;
    announce(EventKind::Revived, Refusal{});
    return out;
}

std::vector<WeaveId> Switchboard::list_weaves() const {
    std::vector<WeaveId> ids;
    ids.reserve(weaves_.size());
    for (const auto& entry : weaves_) {
        ids.push_back(entry.second.id);
    }
    return ids;
}

std::vector<std::shared_ptr<const Schema>> Switchboard::accepted_schemas(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        return {};
    }
    return rec->accept;
}

std::shared_ptr<const Schema> Switchboard::resolve_schema(std::string_view name,
                                                          std::uint32_t version) const {
    return registry_.lookup(name, version);
}

Weave* Switchboard::weave(WeaveId id) {
    WeaveRecord* rec = find(id);
    return rec == nullptr ? nullptr : rec->weave.get();
}

const Weave* Switchboard::weave(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? nullptr : rec->weave.get();
}

bool Switchboard::alive(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec != nullptr && rec->alive;
}

} // namespace loom
