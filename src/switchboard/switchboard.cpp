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
    case RefusalReason::SenderLifeEnded:
        return "SenderLifeEnded";
    case RefusalReason::AnswerTargetChanged:
        return "AnswerTargetChanged";
    case RefusalReason::SealedSpeech:
        return "SealedSpeech";
    }
    return "?";
}

const char* name_of(AdmitRefusal r) noexcept {
    switch (r) {
    case AdmitRefusal::None:
        return "None";
    case AdmitRefusal::ForeignAuthority:
        return "ForeignAuthority";
    case AdmitRefusal::NotACandidate:
        return "NotACandidate";
    case AdmitRefusal::OwnerChanged:
        return "OwnerChanged";
    case AdmitRefusal::IncumbentUnfit:
        return "IncumbentUnfit";
    case AdmitRefusal::RoleNotHeld:
        return "RoleNotHeld";
    }
    return "?";
}

const char* name_of(TxnState st) noexcept {
    switch (st) {
    case TxnState::Preparing:
        return "Preparing";
    case TxnState::Ready:
        return "Ready";
    case TxnState::Committed:
        return "Committed";
    case TxnState::Aborted:
        return "Aborted";
    }
    return "?";
}

const char* name_of(TxnReason r) noexcept {
    switch (r) {
    case TxnReason::None:
        return "None";
    case TxnReason::ExplicitAbort:
        return "ExplicitAbort";
    case TxnReason::PreparationExhausted:
        return "PreparationExhausted";
    case TxnReason::OperatorChanged:
        return "OperatorChanged";
    case TxnReason::CoordinatorChanged:
        return "CoordinatorChanged";
    case TxnReason::IncumbentChanged:
        return "IncumbentChanged";
    case TxnReason::CandidateChanged:
        return "CandidateChanged";
    case TxnReason::RoleChanged:
        return "RoleChanged";
    case TxnReason::CapacityExhausted:
        return "CapacityExhausted";
    case TxnReason::CommitPreconditionFailed:
        return "CommitPreconditionFailed";
    case TxnReason::AdmissionRefused:
        return "AdmissionRefused";
    case TxnReason::NoSuchTransaction:
        return "NoSuchTransaction";
    case TxnReason::WrongState:
        return "WrongState";
    case TxnReason::NotTheOwner:
        return "NotTheOwner";
    case TxnReason::PreconditionFailed:
        return "PreconditionFailed";
    case TxnReason::IncumbentBusy:
        return "IncumbentBusy";
    case TxnReason::CandidateBusy:
        return "CandidateBusy";
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
    case RefusalReason::SenderLifeEnded:
        // Names the AUTHOR, not the target, the payload or the grant: the sender
        // that spoke is no longer the sender that exists.
        return "the sender life that authored this message has ended";
    case RefusalReason::AnswerTargetChanged:
        // Names the CONVERSATION, not the address: the target is there and alive,
        // and is simply not the participant this answer was earned for.
        return "the requester this answer belongs to is no longer the participant "
               "at that address";
    case RefusalReason::SealedSpeech:
        return "a prepared candidate may converse with its coordinator, not with "
               "the world";
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

namespace {
/// Is `speaker` the exact participant that owns this seal — same id, same life,
/// same code? A successor at the same address is not (R2B-3b).
bool owns_seal(const CandidateOwner& owner, WeaveId speaker,
               std::uint64_t speaker_life, std::uint64_t speaker_incarnation) {
    return owner.valid() && owner.who == speaker && owner.life == speaker_life &&
           owner.incarnation == speaker_incarnation;
}
} // namespace

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
                    /*life=*/1,        // its first life; bumped only on a revival
                    /*sealed_by=*/CandidateOwner{}, // in the world from the moment it exists
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
    // THE TRANSITION AN OBSERVER CANNOT SEE. `unregister_weave` announces nothing,
    // which is exactly why the transaction registry lives in the bus rather than
    // watching from outside.
    invalidate_transactions_for(id);
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
    // AND EVERY ENQUEUE PATH ALSO DECIDES WHOSE LIFE IS SPEAKING (R2B-2b). The
    // stamp is read from the bus's own record of the sender, never from anything
    // the caller supplied, for exactly the reason provenance is: a weave hands the
    // bus a Message, and the bus decides the facts about it.
    const std::uint64_t life = gated ? life_of(msg.sender) : 0;
    queue_.push_back(Envelope{std::move(msg), target, seq, gated, std::string{}, life});
    return Ticket{seq};
}

Ticket Switchboard::enqueue_role(std::string role, Message msg, bool gated) {
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    msg.provenance = Provenance{};
    const std::uint64_t life = gated ? life_of(msg.sender) : 0;
    queue_.push_back(Envelope{std::move(msg), WeaveId{}, seq, gated, std::move(role), life});
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

Ticket Switchboard::enqueue_answer(WeaveId to, WeaveId as_sender, Message msg,
                                   std::uint64_t correlation, std::uint64_t requester_life,
                                   std::uint64_t requester_incarnation) {
    // Loom chooses the recipient and the correlation; the answerer chooses only
    // what it says. And Loom stamps WHICH requester the answer is for, so the
    // conversation survives in the envelope rather than only in the stack frame or
    // the registry record that produced it.
    msg.sender = as_sender;
    msg.reply_to = WeaveId{};
    msg.correlation = correlation;
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    msg.provenance = Provenance::attested(Provenance::Kind::Answer, 0);
    Envelope env{std::move(msg), to, seq, /*gated=*/true, std::string{}, life_of(as_sender)};
    env.answer_target = AnswerTarget{true, requester_life, requester_incarnation};
    queue_.push_back(std::move(env));
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
    // The requester's identity comes from the authority — captured when the
    // request was delivered — and not from a fresh lookup here. Within one handler
    // the two cannot differ; taking the same road as the deferred path is what
    // keeps them from ever differing later.
    return enqueue_answer(authority_.requester, as_sender, std::move(msg),
                          authority_.correlation, authority_.requester_life,
                          authority_.requester_incarnation);
}

// ---- deferred answers (R2B-2) ----------------------------------------------
//
// THE LAW: an answer may outlive the handler, but never the conversation or the
// incarnation that earned it.

std::uint64_t Switchboard::incarnation_of(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? 0 : rec->incarnation;
}

std::uint64_t Switchboard::life_of(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? 0 : rec->life;
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

void Switchboard::begin_new_life(WeaveRecord& rec) {
    // ONE PLACE, ONE RULE: a life generation advances exactly when a weave comes
    // back from the dead, and never otherwise. Registration starts at 1; a handler
    // returning, an ordinary message, and a live code reload all leave it alone.
    //
    // The `!alive` test is the whole condition, and it must be read BEFORE the
    // caller marks the weave alive — which is why this is a function rather than a
    // line copied into three revival paths.
    if (!rec.alive) {
        ++rec.life;
    }
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
    const std::uint64_t requester_incarnation = authority_.requester_incarnation;
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
                           authority_.requester_life, // captured at DELIVERY, not now
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
    const std::uint64_t requester_life = rec->requester_life;
    const std::uint64_t requester_incarnation = rec->requester_incarnation;
    *rec = DeferredRecord{};

    // THE SAME DOOR THE IMMEDIATE ANSWER LEAVES BY, carrying the requester facts
    // the RECORD kept — the ones from when the ask was delivered, never today's.
    return enqueue_answer(to, as_sender, std::move(msg), correlation, requester_life,
                          requester_incarnation);
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
    const std::uint64_t sender_life = gated ? life_of(msg.sender) : 0;

    std::size_t recipients = 0;
    for (auto& entry : weaves_) { // std::map: ascending id == registration order
        WeaveRecord& rec = entry.second;
        if (!rec.alive) {
            continue;
        }
        if (rec.sealed_by.valid()) {
            continue; // a candidate is not in the world; the world's news is not its
        }
        if (accept_match(rec, name, version) == nullptr) {
            continue;
        }
        const std::uint64_t seq = next_seq_++;
        journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
        // Rebuilt field by field, which also means a published Message carries no
        // provenance whatever the caller's copy held. EVERY recipient's envelope is
        // stamped, so a publication cannot fan out past a dead author on the
        // strength of one check: each delivery answers the question for itself.
        queue_.push_back(
            Envelope{Message(msg.payload, msg.sender, msg.reply_to, msg.correlation), rec.id,
                     seq, gated, std::string{}, sender_life});
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
    // A candidate cannot publish, and not merely because nobody would hear it: a
    // publication is speech into the world by definition, so there is no coherent
    // "to the coordinator only" version of it. Refused visibly, because a candidate
    // trying to publish is exactly what the operator wants to know about.
    if (sealed(as_sender)) {
        (void)refuse_now(WeaveId{}, as_sender, msg, RefusalReason::SealedSpeech);
        return 0;
    }
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
        // A WEAVE-ORIGINATED MESSAGE BELONGS TO THE LIFE THAT AUTHORED IT (R2B-2b).
        //
        // Queueing is the gap this closes. R2B-2a ends every conversation a dying
        // participant was already in — but a message it had merely QUEUED names no
        // conversation yet, so there was nothing for that cleanup to find. Delivered
        // later, it would have become speech from whatever now answers to the same
        // id: the same weave revived, mid-sentence from a life that ended.
        //
        // Checked FIRST, before the grant and before role resolution, so a stale
        // message reaches nothing at all — not a handler, not an answer authority,
        // not a deferred record, and not even the knowledge of whether a role is
        // currently held.
        ev.sender_life = env.sender_life;
        ev.sender_life_now = sender == nullptr ? 0 : sender->life;
        if (env.msg.sender.valid() &&
            (sender == nullptr || !sender->alive || sender->life != env.sender_life)) {
            // Three ways for a life to be over, and one refusal for all three: it
            // was permanently removed, it is dead, or it has been revived — which
            // makes the current occupant a different life behind the same id. An
            // INVALID sender id is deliberately not one of them: that is not a life
            // that ended, it is no sender at all, and the grant check below already
            // refuses it as CapabilityDenied.
            const Refusal r{RefusalReason::SenderLifeEnded, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
        // ---- the candidate boundary (R2B-3) --------------------------------
        //
        // A prepared candidate may converse INSIDE the preparation before it may
        // speak INSIDE the world, and both halves of that are decided here, before
        // the grant and before role resolution.
        if (sender != nullptr && sender->sealed_by.valid()) {
            // OUTBOUND. A sealed weave may address exactly one weave — the
            // coordinator preparing it — and may not address a ROLE at all, so it
            // cannot even learn whether a production slot is held.
            const WeaveRecord* owner = find(sender->sealed_by.who);
            const bool owner_is_current =
                owner != nullptr &&
                owns_seal(sender->sealed_by, sender->sealed_by.who, owner->life,
                          owner->incarnation);
            if (!env.role.empty() || !(env.target == sender->sealed_by.who) ||
                !owner_is_current) {
                const Refusal r{RefusalReason::SealedSpeech, {}};
                record(env.seq, Disposition::Refused, r);
                ev.kind = EventKind::Refused;
                ev.refusal = r;
                emit(ev);
                return;
            }
        }
        const WeaveRecord* addressee = env.role.empty() ? find(env.target) : nullptr;
        const bool from_owner =
            addressee != nullptr && sender != nullptr &&
            owns_seal(addressee->sealed_by, env.msg.sender, sender->life, sender->incarnation);
        if (addressee != nullptr && addressee->sealed_by.valid() && !from_owner) {
            // INBOUND, and deliberately indistinguishable from an unregistered id:
            // the world must not be able to discover that a candidate exists, still
            // less start a conversation with one. Only its coordinator gets through.
            const Refusal r{RefusalReason::NoSuchTarget, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
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

    // AN AUTHENTICATED ANSWER BELONGS TO THE LIFE AND INCARNATION THAT ASKED
    // (R2B-2c). R2B-2b bound a message to the life that AUTHORED it, which
    // protects the answerer's side; this is the other half — the participant the
    // answer was earned FOR.
    //
    // Ordinary messages deliberately do NOT get this treatment: a direct or
    // role-addressed send is aimed at a logical destination and should reach
    // whoever legitimately occupies it. An answer is different in kind, because
    // its meaning already names one conversation between two exact participants.
    // So the expectation rides only on envelopes that left by an answer door.
    if (env.answer_target.present) {
        ev.expected_requester_life = env.answer_target.life;
        ev.expected_requester_incarnation = env.answer_target.incarnation;
        ev.requester_life_now = rec->life;
        ev.requester_incarnation_now = rec->incarnation;
        // Two ways to be the wrong occupant, and both matter: a NEW LIFE behind
        // this id (it died and came back) and NEW CODE behind it (it was reloaded
        // in place). The second is why an incarnation is required as well as a
        // life — a live reload never stops the weave living, so a life check alone
        // would hand A's completed conversation to successor code B.
        if (rec->life != env.answer_target.life ||
            rec->incarnation != env.answer_target.incarnation) {
            const Refusal r{RefusalReason::AnswerTargetChanged, {}};
            record(env.seq, Disposition::Refused, r);
            ev.kind = EventKind::Refused;
            ev.refusal = r;
            emit(ev);
            return;
        }
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
    // ...AND IT REMEMBERS WHO ASKED, not merely where to send (R2B-2c). Captured
    // HERE, at the delivery that earns the authority, so that an answer produced
    // later — this handler's, or a deferred one spent minutes from now — is bound
    // to the requester that actually asked rather than to whatever occupies that
    // id when the answer is finally written.
    const WeaveRecord* asker = find(env.msg.sender);
    authority_ = ReplyAuthority{env.msg.sender,
                                env.msg.correlation,
                                /*spent=*/false,
                                trusted.payload.schema_ptr(),
                                asker == nullptr ? 0 : asker->life,
                                asker == nullptr ? 0 : asker->incarnation};
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

bool Switchboard::seal_weave(WeaveId candidate, WeaveId coordinator) {
    WeaveRecord* rec = find(candidate);
    const WeaveRecord* owner = find(coordinator);
    // A DEAD coordinator cannot own a preparation — it cannot converse, so the
    // candidate would be sealed to a correspondent that can never answer. And an
    // already-sealed candidate is not resealable: silently changing owners would
    // transfer a prepared candidate to somebody else's transaction, and transfer
    // semantics are deliberately not part of this errand.
    if (rec == nullptr || owner == nullptr || !owner->alive || !rec->role.empty() ||
        rec->sealed_by.valid()) {
        return false;
    }
    // The owner is captured as it is NOW — life and incarnation included — so a
    // later occupant of the same address is a different owner, not this one.
    rec->sealed_by = CandidateOwner{coordinator, owner->life, owner->incarnation};
    return true;
}

CandidateOwner Switchboard::candidate_owner(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? CandidateOwner{} : rec->sealed_by;
}

WeaveId Switchboard::role_holder(std::string_view role) const {
    const auto it = roles_.find(std::string(role));
    return it == roles_.end() ? WeaveId{} : it->second;
}

bool Switchboard::sealed(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec != nullptr && rec->sealed_by.valid();
}


bool Switchboard::commit_candidate(WeaveId candidate, WeaveId incumbent,
                                   const std::string& role) {
    // EVERY PRECONDITION FIRST, SO A REFUSAL CHANGES NOTHING. A half-applied commit
    // is the one outcome this whole phase exists to make impossible, and the
    // cheapest way to guarantee it is to have nothing to undo.
    WeaveRecord* cand = find(candidate);
    WeaveRecord* inc = find(incumbent);
    if (cand == nullptr || inc == nullptr || !cand->sealed_by.valid() || !cand->alive ||
        !inc->alive || role.empty()) {
        return false;
    }
    const auto held = roles_.find(role);
    if (held == roles_.end() || !(held->second == incumbent)) {
        return false; // somebody else holds the slot; this is not our replacement
    }

    // ...AND THEN THE WHOLE CHANGE, WITH NO DELIVERY BETWEEN ANY TWO LINES OF IT.
    // There is no lock here and none is needed: dispatch is single-threaded and
    // `pump()` is non-reentrant, so an ordinary observer's next delivery either
    // precedes all of this or follows all of it. What would NOT be atomic is
    // expressing the same change as several ordinary messages — which is exactly
    // what today's SwapWeave does, and exactly the observable window it documents.
    cand->sealed_by = CandidateOwner{};
    inc->role.clear();
    cand->role = role;
    held->second = candidate;
    return true;
}

AdmitResult Switchboard::admit_candidate(WeaveId candidate, WeaveId incumbent,
                                         const std::string& role,
                                         const LifecycleAuthority& authority,
                                         Message activation, std::int64_t sequence) {
    // EVERY PRECONDITION FIRST — including the authority, so an unattested caller
    // cannot move production topology.
    if (!issued_here(authority)) {
        return {false, AdmitRefusal::ForeignAuthority};
    }
    WeaveRecord* cand = find(candidate);
    WeaveRecord* inc = find(incumbent);
    if (cand == nullptr || !cand->alive || !cand->sealed_by.valid()) {
        return {false, AdmitRefusal::NotACandidate};
    }
    if (inc == nullptr || !inc->alive || inc->sealed_by.valid()) {
        return {false, AdmitRefusal::IncumbentUnfit};
    }
    const CandidateOwner owner = cand->sealed_by;

    // THE OWNER MUST STILL BE THE OWNER (R2B-3b-1a).
    //
    // The candidate's private conversation already checks this on every message;
    // admission did not, which left the strongest act in the system — moving
    // production topology — resting on a stale fact. A trusted host caller holding
    // a perfectly good lifecycle authority could admit a candidate whose
    // coordinator had died and revived, been reloaded into new code, or been
    // removed entirely. The preparation belonged to a LIFE, and that life is over.
    const WeaveRecord* owner_rec = find(owner.who);
    if (owner_rec == nullptr || !owner_rec->alive || owner_rec->life != owner.life ||
        owner_rec->incarnation != owner.incarnation) {
        return {false, AdmitRefusal::OwnerChanged};
    }

    if (role.empty()) {
        return {false, AdmitRefusal::RoleNotHeld};
    }
    const auto held = roles_.find(role);
    if (held == roles_.end() || !(held->second == incumbent)) {
        return {false, AdmitRefusal::RoleNotHeld};
    }

    // ---- ACTIVATION FIRST, and this is where the queue resisted the model -----
    //
    // Role resolution is a DELIVERY-time decision, so a role-addressed message
    // enqueued before this moment resolves to whoever holds the role when it is
    // finally dispatched — which, after this call, is the candidate. Appending the
    // activation at the tail would therefore let ordinary production reach a weave
    // that has not yet been told it is alive.
    //
    // The activation is placed immediately ahead of the FIRST queued envelope that
    // could reach this candidate: one addressed to the role being committed, or to
    // the candidate itself. That is the narrowest placement that makes activation
    // the candidate's first live delivery. Every other message keeps its order,
    // and nothing is dropped — the alternative (discarding the older traffic) would
    // buy ordering with silence.
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    activation.sender = owner.who;
    activation.provenance = Provenance::attested(Provenance::Kind::Activation, sequence);
    // The sender-life stamp comes from the VERIFIED owner — the life the seal
    // named and this call has just confirmed — never from a fresh lookup of the
    // coordinator id, which is precisely how a successor's life could be stamped
    // onto a predecessor's activation.
    Envelope act{std::move(activation), candidate, seq, /*gated=*/true, std::string{},
                 owner.life};
    auto at = queue_.begin();
    for (; at != queue_.end(); ++at) {
        if (at->target == candidate || (!at->role.empty() && at->role == role)) {
            break;
        }
    }
    queue_.insert(at, std::move(act));

    // ---- and then the whole topology change, with no delivery in between ------
    cand->sealed_by = CandidateOwner{};
    inc->role.clear();
    cand->role = role;
    held->second = candidate;
    // The incumbent is sealed FOR RETIREMENT, to the same coordinator: it stops
    // receiving production entirely — not merely role traffic — and remains
    // reachable for the private retirement conversation. Moving the role alone
    // would leave it publicly direct-addressable, which is a second live service.
    inc->sealed_by = owner;
    return {true, AdmitRefusal::None};
}

// ---- Prepared replacement (R2B-3b-2) ---------------------------------------

ParticipantRef Switchboard::participant(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        return ParticipantRef{};
    }
    return ParticipantRef{id, rec->life, rec->incarnation};
}

bool Switchboard::still(const ParticipantRef& was) const {
    const WeaveRecord* rec = find(was.who);
    return rec != nullptr && rec->alive && rec->life == was.life &&
           rec->incarnation == was.incarnation;
}

Switchboard::PreparedReplacement* Switchboard::find_txn(TxnId id) {
    if (!id.valid()) {
        return nullptr;
    }
    for (PreparedReplacement& t : txns_) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

const Switchboard::PreparedReplacement* Switchboard::find_txn(TxnId id) const {
    if (!id.valid()) {
        return nullptr;
    }
    for (const PreparedReplacement& t : txns_) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

void Switchboard::finish_txn(PreparedReplacement& txn, TxnState state, TxnReason reason) {
    // ORDERING IS THE WHOLE CORRECTION (R2B-3b-2a).
    //
    // Cleanup discards the candidate, discarding a candidate is a lifecycle
    // change, and a lifecycle change re-enters `invalidate_transactions_for`. If
    // this transaction were still in the active registry at that moment the hook
    // would rediscover it and end it a SECOND time — two terminal truths for one
    // promise, and two slots consumed in a bounded store that then evicts somebody
    // else's result early.
    //
    // So the record leaves the active registry FIRST, and the re-entrant hook has
    // nothing to find. That is structural non-reentrancy rather than a "currently
    // finishing" flag, which would have to be honoured at every future call site
    // instead of being true at one.
    //
    // `txn` is deliberately a caller-owned COPY (every call site passes one), so
    // erasing the registry entry below leaves these facts valid to use afterwards.
    const TxnId id = txn.id;
    const ParticipantRef op = txn.op;
    const WeaveId candidate = txn.candidate.who;

    // 1. no longer discoverable as active — and the slot is back at once.
    for (auto it = txns_.begin(); it != txns_.end(); ++it) {
        if (it->id == id) {
            txns_.erase(it);
            break;
        }
    }

    // 2. EXACTLY ONE terminal outcome. Evidence, not authority: it says what
    //    happened and confers nothing. Kept for the exact operator that began the
    //    transaction, in a separately bounded store, dropped oldest-first.
    if (outcomes_.size() >= kMaxTerminalOutcomes) {
        outcomes_.erase(outcomes_.begin());
        outcome_of_.erase(outcome_of_.begin());
    }
    outcomes_.push_back(TxnOutcome{id, state, reason});
    outcome_of_.push_back(op);

    // 3. and only now the cleanup that may re-enter. A candidate that never
    //    entered the world is discarded, which also ends any speech it had queued
    //    (R2B-2b). Only ever a SEALED weave: an admitted candidate is a live
    //    service and is nobody's to discard. If this fails, it leaves sealed
    //    wreckage — which is nonpublic by construction, cannot be admitted through
    //    a transaction that no longer exists, and does not produce a second result.
    if (state == TxnState::Aborted) {
        const WeaveRecord* cand = find(candidate);
        if (cand != nullptr && cand->sealed_by.valid()) {
            (void)unregister_weave(candidate);
        }
    }
}

void Switchboard::invalidate_transactions_for(WeaveId changed) {
    // SELECTIVE, ALWAYS. One weave's death is not everybody's problem, so this
    // aborts only transactions that BIND `changed` and only when the fact they
    // captured no longer holds.
    //
    // Re-entrancy matters here: finishing a transaction can unregister its
    // candidate, which calls back into this function. The loop re-scans from the
    // start after each finish rather than holding an iterator across it.
    bool again = true;
    while (again) {
        again = false;
        for (PreparedReplacement& t : txns_) {
            TxnReason why = TxnReason::None;
            if (t.op.who == changed && !still(t.op)) {
                why = TxnReason::OperatorChanged;
            } else if (t.coordinator.who == changed && !still(t.coordinator)) {
                why = TxnReason::CoordinatorChanged;
            } else if (t.incumbent.who == changed && !still(t.incumbent)) {
                why = TxnReason::IncumbentChanged;
            } else if (t.candidate.who == changed && !still(t.candidate)) {
                why = TxnReason::CandidateChanged;
            } else if (t.incumbent.who == changed) {
                // Still the same life and code — but the role may have moved out
                // from under it, or it may have been sealed by someone else.
                const WeaveRecord* inc = find(t.incumbent.who);
                const auto held = roles_.find(t.role);
                if (inc == nullptr || inc->sealed_by.valid() || held == roles_.end() ||
                    !(held->second == t.incumbent.who)) {
                    why = TxnReason::RoleChanged;
                }
            }
            if (why != TxnReason::None) {
                PreparedReplacement copy = t; // finish_txn erases from txns_
                finish_txn(copy, TxnState::Aborted, why);
                again = true;
                break;
            }
        }
    }
}

TxnResult Switchboard::begin_prepared_replacement(WeaveId op, WeaveId coordinator,
                                                  WeaveId incumbent, WeaveId candidate,
                                                  const std::string& role,
                                                  std::uint32_t budget) {
    // CAPACITY FIRST, so an overflow refuses before anything is inspected, let
    // alone touched. The incumbent's whole guarantee is that a failed attempt is
    // indistinguishable from no attempt.
    if (txns_.size() >= kMaxPreparedReplacements) {
        return {false, TxnId{}, TxnReason::CapacityExhausted};
    }
    if (budget == 0 || budget > kMaxPreparationBudget || role.empty()) {
        return {false, TxnId{}, TxnReason::PreconditionFailed};
    }
    const ParticipantRef o = participant(op);
    const ParticipantRef c = participant(coordinator);
    const ParticipantRef i = participant(incumbent);
    const ParticipantRef k = participant(candidate);
    if (!still(o) || !still(c) || !still(i) || !still(k)) {
        return {false, TxnId{}, TxnReason::PreconditionFailed};
    }
    const WeaveRecord* inc = find(incumbent);
    const WeaveRecord* cand = find(candidate);
    // The incumbent must be a public service holding the role; the candidate must
    // be sealed by exactly this coordinator and hold no role at all.
    if (inc->sealed_by.valid() || !cand->sealed_by.valid() || !cand->role.empty()) {
        return {false, TxnId{}, TxnReason::PreconditionFailed};
    }
    if (!(cand->sealed_by.who == coordinator) || cand->sealed_by.life != c.life ||
        cand->sealed_by.incarnation != c.incarnation) {
        return {false, TxnId{}, TxnReason::CoordinatorChanged};
    }
    const auto held = roles_.find(role);
    if (held == roles_.end() || !(held->second == incumbent)) {
        return {false, TxnId{}, TxnReason::RoleChanged};
    }
    // ONE INCUMBENT, ONE REPLACEMENT — and ONE CANDIDATE, ONE REPLACEMENT.
    //
    // The second half was missing, and the gap was not cosmetic: the same sealed
    // candidate could be promised to two different incumbents at once. Every other
    // precondition holds for both (it is sealed by the same coordinator, holds no
    // role, is alive), so nothing else would have caught it — and the consequences
    // are all bad. One readiness event would answer two transactions; aborting one
    // would unregister the candidate out from under the other; committing one
    // would make it public while the other still believed it sealed.
    //
    // Refused BEFORE a slot is consumed or anything is touched, and the two causes
    // are named separately because they send an operator to different places.
    for (const PreparedReplacement& t : txns_) {
        if (t.incumbent.who == incumbent) {
            return {false, TxnId{}, TxnReason::IncumbentBusy};
        }
        if (t.candidate.who == candidate) {
            return {false, TxnId{}, TxnReason::CandidateBusy};
        }
    }

    const TxnId id{next_txn_id_++};
    txns_.push_back(PreparedReplacement{id, o, c, i, k, role, TxnState::Preparing, budget,
                                        TxnReason::None});
    return {true, id, TxnReason::None};
}

TxnResult Switchboard::tick_preparation(TxnId id) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction};
    }
    if (t->state != TxnState::Preparing) {
        return {false, id, TxnReason::WrongState}; // the budget is a PREPARING cost
    }
    if (t->budget > 0) {
        --t->budget;
    }
    if (t->budget == 0) {
        PreparedReplacement copy = *t;
        finish_txn(copy, TxnState::Aborted, TxnReason::PreparationExhausted);
        return {false, id, TxnReason::PreparationExhausted};
    }
    return {true, id, TxnReason::None};
}

TxnResult Switchboard::mark_candidate_ready(TxnId id, WeaveId coordinator, WeaveId candidate) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction};
    }
    if (t->state != TxnState::Preparing) {
        return {false, id, TxnReason::WrongState};
    }
    if (!(t->coordinator.who == coordinator) || !(t->candidate.who == candidate)) {
        return {false, id, TxnReason::NotTheOwner};
    }
    if (!still(t->coordinator) || !still(t->candidate) || !still(t->op) ||
        !still(t->incumbent)) {
        return {false, id, TxnReason::PreconditionFailed};
    }
    const WeaveRecord* cand = find(t->candidate.who);
    if (cand == nullptr || !cand->sealed_by.valid() ||
        !(cand->sealed_by.who == t->coordinator.who) ||
        cand->sealed_by.life != t->coordinator.life ||
        cand->sealed_by.incarnation != t->coordinator.incarnation) {
        return {false, id, TxnReason::CandidateChanged};
    }
    t->state = TxnState::Ready;
    return {true, id, TxnReason::None};
}

TxnResult Switchboard::commit_prepared_replacement(TxnId id,
                                                   const LifecycleAuthority& authority,
                                                   Message activation,
                                                   std::int64_t sequence) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction};
    }
    if (t->state != TxnState::Ready) {
        // Preparing is the interesting case and it is simply refused: a commit
        // before readiness is the whole thing this phase exists to prevent.
        return {false, id, TxnReason::WrongState};
    }
    // Revalidate every exact identity. `admit_candidate` checks the ones it needs
    // for its own safety; the transaction checks the ones IT promised.
    if (!still(t->op) || !still(t->coordinator) || !still(t->incumbent) ||
        !still(t->candidate)) {
        PreparedReplacement copy = *t;
        finish_txn(copy, TxnState::Aborted, TxnReason::CommitPreconditionFailed);
        return {false, id, TxnReason::CommitPreconditionFailed};
    }

    // ONE ADMISSION MUTATION, AND IT IS NOT HERE. The transaction layer never
    // moves a role, never unseals anything and never queues an activation: it
    // delegates the whole topology change to the primitive that was proven atomic.
    const PreparedReplacement snapshot = *t;
    const AdmitResult admitted = admit_candidate(snapshot.candidate.who,
                                                 snapshot.incumbent.who, snapshot.role,
                                                 authority, std::move(activation), sequence);
    PreparedReplacement* again = find_txn(id);
    if (again == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction}; // aborted underneath us
    }
    if (!admitted.ok) {
        PreparedReplacement copy = *again;
        finish_txn(copy, TxnState::Aborted, TxnReason::AdmissionRefused);
        return {false, id, TxnReason::AdmissionRefused};
    }
    PreparedReplacement copy = *again;
    finish_txn(copy, TxnState::Committed, TxnReason::None);
    return {true, id, TxnReason::None};
}

TxnResult Switchboard::abort_prepared_replacement(TxnId id, WeaveId op) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction};
    }
    if (!(t->op.who == op)) {
        return {false, id, TxnReason::NotTheOwner};
    }
    PreparedReplacement copy = *t;
    finish_txn(copy, TxnState::Aborted, TxnReason::ExplicitAbort);
    return {true, id, TxnReason::None};
}

TxnState Switchboard::transaction_state(TxnId id) const {
    const PreparedReplacement* t = find_txn(id);
    if (t != nullptr) {
        return t->state;
    }
    for (const TxnOutcome& o : outcomes_) {
        if (o.id == id) {
            return o.state;
        }
    }
    return TxnState::Aborted; // unknown ids are not live, and never were
}

bool Switchboard::transaction_active(TxnId id) const { return find_txn(id) != nullptr; }

std::size_t Switchboard::active_transactions() const noexcept { return txns_.size(); }

bool Switchboard::take_outcome(WeaveId op, TxnOutcome& out) {
    const ParticipantRef now = participant(op);
    for (std::size_t i = 0; i < outcomes_.size(); ++i) {
        // The EXACT life and incarnation that began it. A successor at the same
        // address inherits no result, exactly as it inherits no conversation.
        if (outcome_of_[i] == now) {
            out = outcomes_[i];
            outcomes_.erase(outcomes_.begin() + static_cast<std::ptrdiff_t>(i));
            outcome_of_.erase(outcome_of_.begin() + static_cast<std::ptrdiff_t>(i));
            return true; // consumed once
        }
    }
    return false;
}

void Switchboard::kill(WeaveId id) {
    WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        return;
    }
    rec->alive = false;
    // WHAT THE ANNOUNCEMENT NEEDS IS READ BEFORE ANY HOOK RUNS, and that is not
    // tidiness: aborting a prepared replacement discards its candidate, and if THIS
    // weave is that candidate the hook erases the very record `rec` points at.
    // ASan found it; the Debug lane did not.
    BusEvent ev;
    ev.kind = EventKind::Died;
    ev.target = id;
    ev.schema_name = rec->state_schema->name();
    ev.schema_version = rec->state_schema->version();

    // Committing the death includes ending its unfinished conversations (R2B-2a)
    // and its unfinished transactions (R2B-3b-2), both BEFORE the announcement so
    // that anything observing `Died` sees a world in which they are already over.
    abandon_deferred_for(id);
    invalidate_transactions_for(id); // a life that ended owns no preparation
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
        begin_new_life(*rec); // a revival is a NEW LIFE behind the same id (R2B-2b)
        rec->alive = true;
        out.revived = true;
        announce(/*from_lkg=*/false, Refusal{});
        invalidate_transactions_for(id); // ...and a new life owns no preparation
        return out;
    }

    // The candidate was refused. The policy decides whether the self may return
    // as its last-known-good.
    out.refusal = Refusal{RefusalReason::GateRefused, admitted.first_error()};
    if (revive_from_last_good) {
        rec->weave->revive(rec->last_known_good);
        ++rec->reloads_used;
        begin_new_life(*rec); // the fallback branch is no less a new life
        rec->alive = true;
        out.revived = true;
        out.from_last_known_good = true;
        announce(/*from_lkg=*/true, out.refusal);
        invalidate_transactions_for(id);
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
    // A SWAP CAN ALSO BE A REVIVAL: this path marks the weave alive whatever it was
    // before, so if it was dead, this is a new life as well as new code. If it was
    // alive, the life continues — a live code reload is not a death, and speech
    // already in the queue is still that same living weave's (R2B-2b).
    begin_new_life(*rec);
    rec->alive = true;
    // NEW CODE BEHIND A STABLE ID IS A NEW INCARNATION (R2B-2). The WeaveId is
    // deliberately unchanged — that is what reload means — so this counter is the
    // only thing that distinguishes the successor from the incarnation that may
    // have earned a deferred answer. Bumping it here, then forgetting that
    // weave's unfinished conversations, is what keeps handler-surviving authority
    // from quietly becoming reload-surviving authority.
    ++rec->incarnation;
    forget_deferred_for(id);
    // New code, or a revival, is a new participant as far as a transaction is
    // concerned — and this hook was MISSING in the first cut, which the
    // commit-precondition case caught: a coordinator could be reloaded under a
    // Ready transaction and the transaction would not notice until commit. The
    // phase's own law is that it must become terminal PROMPTLY.
    out.revived = true;
    announce(EventKind::Revived, Refusal{}); // uses `rec`; the hook below may erase it
    invalidate_transactions_for(id);
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
