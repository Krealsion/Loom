// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/switchboard/switchboard.hpp>
#include <zen/weave/dispatch_refusal.hpp>
#include <stdexcept>

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace loom {

namespace {

/// How long the handler held the one mind: two `steady_clock::now()` reads per delivery and
/// nothing else. A duration, never a time: Loom stamps no message with a clock reading. A pair
/// of reads costs about 35 ns on x86-64 with GCC 11.4, about one part in 100,000 of an idle
/// application's runtime, so it is unconditional rather than an option.
std::uint64_t elapsed_ns_since(std::chrono::steady_clock::time_point t0) noexcept {
    const auto d = std::chrono::steady_clock::now() - t0;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    return ns < 0 ? 0 : static_cast<std::uint64_t>(ns);
}

/// The (name, version) pairs a set of send rules actually NAME (LIFE-08). A
/// wildcard rule names nothing, so it contributes nothing: `allow_any` is
/// permission without a declared vocabulary, and there is no shape it could
/// sensibly keep alive.
std::vector<detail::SchemaKey> named_send_shapes(const LiveAuthority& authority) {
    std::vector<detail::SchemaKey> keys;
    for (const SendRule& rule : authority.rules()) {
        if (!rule.any_shape) {
            keys.emplace_back(rule.shape_name, rule.shape_version);
        }
    }
    return keys;
}
/// The baseline half of a grant, by the same rule — delegated authority earns
/// the identical claim through the overload above (GATE-05).
std::vector<detail::SchemaKey> named_send_shapes(const Grant& grant) {
    return named_send_shapes(grant.live());
}

} // namespace

// ---- Grant administration --------------------------------------------------

const char* name_of(GrantOutcome outcome) noexcept {
    switch (outcome) {
    case GrantOutcome::Installed:
        return "Installed";
    case GrantOutcome::NoAuthority:
        return "NoAuthority";
    case GrantOutcome::ForeignBoard:
        return "ForeignBoard";
    case GrantOutcome::NoSuchSubject:
        return "NoSuchSubject";
    case GrantOutcome::ExceedsCeiling:
        return "ExceedsCeiling";
    case GrantOutcome::NoLiveDelivery:
        return "NoLiveDelivery";
    }
    return "?";
}

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
    case RefusalReason::AdmissionRevoked:
        return "AdmissionRevoked";
    case RefusalReason::RoleAuthorshipDenied:
        return "RoleAuthorshipDenied";
    case RefusalReason::SeamUnresolved:
        return "SeamUnresolved";
    case RefusalReason::ApplicationFailed:
        return "ApplicationFailed";
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
    case AdmitRefusal::CandidateContract:
        return "CandidateContract";
    }
    return "?";
}

const char* name_of(TxnState st) noexcept {
    switch (st) {
    case TxnState::Preparing:
        return "Preparing";
    case TxnState::Ready:
        return "Ready";
    case TxnState::AdmissionPending:
        return "AdmissionPending";
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
    case TxnReason::CandidateRefused:
        return "CandidateRefused";
    case TxnReason::InvalidReadiness:
        return "InvalidReadiness";
    case TxnReason::LateReadiness:
        return "LateReadiness";
    case TxnReason::PreparationAlreadyAsked:
        return "PreparationAlreadyAsked";
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
    case RefusalReason::AdmissionRevoked:
        // Names the ADMISSION, not the message. Nothing about this delivery was
        // wrong; the world it described stopped being true before it was reached,
        // and the topology change it was carrying did not happen.
        return "the scheduled admission no longer described the world; nothing "
               "changed and the incumbent is still the service";
    case RefusalReason::RoleAuthorshipDenied:
        // Names the AUTHORSHIP, not a grant or a forged capability: the sender
        // asked to speak for an office it does not currently hold. Nothing was
        // queued, and nothing was downgraded to personal speech.
        return "the sender does not hold the role it deliberately asked to "
               "speak for; nothing was queued";
    case RefusalReason::SeamUnresolved:
        // Names the SEAM and the VOCABULARY, not the payload, the target or the
        // grant: the shape's registrar was never loaded here, so there is no door
        // to admit this against and no target was ever consulted.
        return "the shape claimed across the library seam is not registered in "
               "this Loom; nothing was queued";
    case RefusalReason::ApplicationFailed:
        // Names the TARGET'S OWN STATE, not the message, the grant or the address:
        // the target is there and alive and could not apply a value the bus
        // published under its claim, so nothing is delivered to it until it is
        // reloaded or removed. The record and the operator's notice say which
        // participant and which publication.
        return "the target is held behind a published claim it could not apply; "
               "nothing is delivered to it until it is reloaded or removed";
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
/// same code? A successor at the same address is not (PR-03).
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
    // A fixed-size ring, allocated once: the journal's footprint is kJournalCapacity for the
    // life of the bus, however many messages it carries. Slots start seq=0 ("never written");
    // real seqs start at 1.
    journal_.assign(kJournalCapacity, JournalSlot{});
}

// A WEAVE MAY REACH BACK INTO THE BUS FROM ITS OWN DESTRUCTOR -- a weave that owns a
// BridgeServer unregisters that server's proxies and removes its observer when it dies.
// Were the registry destroyed member-wise, that call would erase from a map already being
// torn down. So the registry is emptied FIRST, into a local, and the weaves die from there:
// a re-entrant `unregister_weave` then finds nothing and returns nothing, and every other
// member (observers, roles, the journal) is still whole for the duration of the body.
Switchboard::~Switchboard() {
    std::map<std::uint64_t, WeaveRecord> dying = std::move(weaves_);
    weaves_.clear();
    dying.clear();
}

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
                                    "' is already held (roles are singletons)");
    }

    // Record the accept-set, so all Weaves agree on what a given (name, version)
    // means (a disagreement throws loom::SchemaConflict).
    std::vector<std::shared_ptr<const Schema>> accept;
    auto declared = incoming->accepted_schemas();
    accept.reserve(declared.size());
    for (auto& s : declared) {
        if (!s) {
            throw std::invalid_argument("register_weave: a declared accept schema is null");
        }
        accept.push_back(std::move(s));
    }

    // Record the declared claim-set the same way, and for the same
    // reason plus one more: claiming these here is what makes a Sense
    // DISCOVERABLE — its shape resolves, and a consumer can ask what this weave
    // can claim — before any runtime claim has ever happened.
    std::vector<std::shared_ptr<const Schema>> claims;
    auto declared_claims = incoming->claimed_schemas();
    claims.reserve(declared_claims.size());
    for (auto& s : declared_claims) {
        if (!s) {
            throw std::invalid_argument("register_weave: a declared claim schema is null");
        }
        claims.push_back(std::move(s));
    }

    // Record the declared emit-set the same way: a shape a weave says it will send is a promise
    // about somebody else's door, so its definition meets the same wall below, at registration
    // rather than at the first refused delivery. It also makes the emit-set discoverable
    // (`emitted_schemas(id)`), and it confers no authority: the grant is checked at every send.
    // docs/decisions/declared-vocabulary-is-agreed-at-admission.md
    std::vector<std::shared_ptr<const Schema>> emits;
    auto declared_emits = incoming->emitted_schemas();
    emits.reserve(declared_emits.size());
    for (auto& s : declared_emits) {
        if (!s) {
            throw std::invalid_argument("register_weave: a declared emit schema is null");
        }
        emits.push_back(std::move(s));
    }

    // Seed last-known-good from an initial snapshot, gated against its own schema.
    Value snap = incoming->snapshot();
    std::shared_ptr<const Schema> state_schema = snap.schema_ptr();

    // One transaction for the whole vocabulary (LIFE-08): every shape this weave needs
    // resolvable is claimed together, so a conflict over the last leaves no trace of the first.
    // The vocabulary is the closure, not the four lists' roots: every component a declared
    // shape nests is claimed beside it, so two weaves nesting `Part v1` under different outer
    // names, or one declaration nesting two, still meet the registry's one comparison. The
    // claim lives in the record built below and dies when that record is erased.
    std::vector<std::shared_ptr<const Schema>> vocabulary;
    auto declare = [&vocabulary](const std::shared_ptr<const Schema>& s) {
        collect_referenced(*s, vocabulary); // components first; identity-deduplicated
        vocabulary.push_back(s);
    };
    for (const auto& s : accept) {
        declare(s);
    }
    for (const auto& s : claims) {
        declare(s);
    }
    for (const auto& s : emits) {
        declare(s);
    }
    declare(state_schema);
    SchemaClaimScope schemas = registry_.claim(vocabulary);
    // ...and the shapes this weave may speak but does not define (LIFE-08). A producer needs its
    // named send shapes resolvable at the seam as a consumer's door does, or once the defining
    // weave unmounts its next send meets "unknown shape" rather than "nobody holds that role".
    // Claimed by key: a grant-only producer has no definition, so a shape nobody published stays
    // unpublished (MSG-08); a declared emitter offered its definition above, and a wildcard rule
    // names nothing. docs/laws/lifecycle-laws.md
    registry_.claim_known(schemas, named_send_shapes(grant));
    // Adopt the canonical owners the registry settled on, so every weave that accepts a shape
    // holds the same Schema object for it. `state_schema` keeps the weave's own object: it is
    // the door its own snapshot is admitted against, and the gate compares content, never
    // pointers.
    auto canonicalize = [this](std::shared_ptr<const Schema>& s) {
        if (auto canon = registry_.lookup(s->name(), s->version())) {
            s = std::move(canon);
        }
    };
    for (auto& s : accept) {
        canonicalize(s);
    }
    for (auto& s : claims) {
        canonicalize(s);
    }
    for (auto& s : emits) {
        canonicalize(s);
    }

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
                    std::move(claims),
                    std::move(emits),
                    state_schema,
                    std::move(schemas),
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
    // A self-describing weave cannot also be wildcard-accepting: the self-description door
    // answers from the declared accept-set, while `AnyRegistered` widens the door set at
    // delivery from a registry the weave cannot read, so the answer would understate what is
    // enforced. Checked before the flag is set, and by name, so it holds for a raw loom::Weave
    // that declares the door by hand as well as for a WeaveBase.
    if (accept_mode == AcceptMode::AnyRegistered && incoming) {
        for (const auto& s : incoming->accepted_schemas()) {
            if (s && s->name() == kDescribeAcceptedShapeName) {
                throw std::invalid_argument(
                    "register_weave: a weave accepting '" +
                    std::string(kDescribeAcceptedShapeName) +
                    "' describes its own accept-set, so it cannot also be registered "
                    "AcceptMode::AnyRegistered — the wildcard accepts shapes the "
                    "description could never name");
            }
        }
    }
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
    // A weave outlives its own callback (LIFE-06): checked first, because this refusal means
    // nothing happened, and a refusal rather than a deferral, because a removal hands back a
    // `unique_ptr` the caller may reset at once. `current_target_` is the object whose `handle`
    // is running (cleared by `DeliveryScope` on every exit), not `in_dispatch_`: a turn may
    // remove a different weave, as the transaction layer does when a candidate refuses.
    // docs/reference/lifecycle.md#permanent-removal-and-the-active-callback
    if (current_target_.valid() && current_target_ == id) {
        return nullptr;
    }
    auto it = weaves_.find(id.value);
    if (it == weaves_.end()) {
        return nullptr;
    }
    if (!it->second.role.empty()) {
        roles_.erase(it->second.role); // a role has no holder once its Weave is removed
        // ...and an office with no officeholder has no current claimant, so its
        // latest claims go with it. This is the ONLY way office claims
        // are dropped: an admission moves the role holder IN PLACE and never
        // passes through unheld, so a replacement leaves the predecessor's claim
        // standing — stamped stale, never deleted and never relabelled.
        forget_office_claims(it->second.role);
    }
    // The weave's own latest claims end with it. This is what bounds the
    // repository by CURRENT keys rather than by claims ever made.
    forget_personal_claims(id);
    std::unique_ptr<Weave> released = std::move(it->second.weave);
    weaves_.erase(it);
    // Its unfinished conversations end with it, in both directions: it can no longer answer,
    // and nothing can be answered to it. Asked unconditionally (ANS-04): permanent removal is
    // the end of a life.
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
// erased. Only the verified answer, lifecycle, office and refusal paths attest.
std::uint64_t Switchboard::allocate_sequence() {
    if (next_seq_ == 0) {
        throw std::overflow_error("Loom delivery sequence exhausted");
    }
    return next_seq_++;
}

void Switchboard::capture_refusal_recipient(Envelope& env) {
    if (!env.gated || env.preparation.valid() ||
        env.msg.provenance.kind() != Provenance::Kind::None) { return; }
    const WeaveRecord* sender = find(env.msg.sender);
    if (sender != nullptr && sender->alive &&
        accept_match(*sender, DispatchRefused::zen_name, DispatchRefused::zen_version)) {
        env.refusal_incarnation = sender->incarnation;
    }
}

void Switchboard::notify_dispatch_refusal(const Envelope& env, const BusEvent& ev,
                                          bool permitted) {
    if (env.refusal_incarnation == 0 || !env.gated) { return; }
    switch (ev.refusal.reason) {
    case RefusalReason::CapabilityDenied:
    case RefusalReason::NoSuchTarget:
    case RefusalReason::TargetUnavailable:
    case RefusalReason::NotAccepted:
    case RefusalReason::GateRefused:
    // A target held behind a published claim it could not apply is a pre-handler
    // refusal like the five above, and the one an operator most needs by exact
    // attempt.
    case RefusalReason::ApplicationFailed:
        break;
    default:
        return;
    }
    const WeaveRecord* sender = find(env.msg.sender);
    if (sender == nullptr || !sender->alive || sender->life != env.sender_life ||
        sender->incarnation != env.refusal_incarnation ||
        !accept_match(*sender, DispatchRefused::zen_name, DispatchRefused::zen_version)) {
        return;
    }
    DispatchRefused notice;
    notice.attempt = std::to_string(env.seq);
    notice.target = env.role.empty() ? std::to_string(env.target.value) : std::string{};
    notice.role = env.role;
    notice.shape = ev.schema_name; // the gate may already have consumed the payload
    notice.version = ev.schema_version;
    notice.reason = name_of(permitted ? ev.refusal.reason : RefusalReason::CapabilityDenied);
    const std::uint64_t seq = allocate_sequence();
    Message msg(to_value(notice), WeaveId{}, WeaveId{}, env.msg.correlation);
    msg.provenance = Provenance::attested(Provenance::Kind::DispatchRefusal, 0);
    Envelope reply{std::move(msg), env.msg.sender, seq, false, {}, env.sender_life};
    reply.refusal_incarnation = env.refusal_incarnation;
    reply.dispatch_parent = env.seq;
    reply.fence = env.fence; // a consequence of the refused envelope, counted where it is
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    queue_back(std::move(reply));
}

Ticket Switchboard::enqueue_directed(WeaveId target, Message msg, bool gated,
                                     Provenance provenance, TxnId preparation) {
    const std::uint64_t seq = allocate_sequence();
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    msg.provenance = std::move(provenance);
    // AND EVERY ENQUEUE PATH ALSO DECIDES WHOSE LIFE IS SPEAKING (MSG-03). The
    // stamp is read from the bus's own record of the sender, never from anything
    // the caller supplied, for exactly the reason provenance is: a weave hands the
    // bus a Message, and the bus decides the facts about it.
    const std::uint64_t life = gated ? life_of(msg.sender) : 0;
    Envelope env{std::move(msg), target, seq, gated, std::string{}, life};
    env.preparation = preparation; // invalid for every caller but one
    capture_refusal_recipient(env);
    // ...and which delivery this was authored from, read from the bus's own dispatch state for
    // the same reason: it is a fact about the message, so the message never gets to say it. 0
    // when nothing was being dispatched.
    env.dispatch_parent = current_dispatch_seq_;
    env.fence = current_dispatch_fence_; // ...and which fence that delivery belongs to
    queue_back(std::move(env));
    return Ticket{seq};
}

Ticket Switchboard::enqueue_role(std::string role, Message msg, bool gated,
                                 Provenance provenance) {
    const std::uint64_t seq = allocate_sequence();
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    // The same single assignment that makes provenance unforgeable on the
    // directed path: ordinary callers pass nothing and the default ERASES
    // whatever the Message carried; only the office-authorship door passes a
    // verified fact.
    msg.provenance = std::move(provenance);
    const std::uint64_t life = gated ? life_of(msg.sender) : 0;
    Envelope env{std::move(msg), WeaveId{}, seq, gated, std::move(role), life};
    capture_refusal_recipient(env);
    env.dispatch_parent = current_dispatch_seq_; // the delivery this was authored from
    env.fence = current_dispatch_fence_;
    queue_back(std::move(env));
    return Ticket{seq};
}

Ticket Switchboard::refuse_now(WeaveId target, WeaveId sender, const Message& msg,
                               RefusalReason reason) {
    // A refusal that never became a delivery still gets a seq, a journal slot and
    // a tap event, so "this weave tried to answer without authority" is visible at
    // exactly the altitude "this weave tried to send without a grant" already is.
    const std::uint64_t seq = allocate_sequence();
    const Refusal r{reason, {}};
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{Disposition::Refused, r}};
    BusEvent ev;
    ev.kind = EventKind::Refused;
    ev.seq = seq;
    ev.target = target;
    ev.sender = sender;
    ev.schema_name = msg.payload.schema().name();
    ev.schema_version = msg.payload.schema().version();
    ev.correlation = msg.correlation;
    ev.dispatch_parent = current_dispatch_seq_;
    ev.refusal = r;
    emit(ev);
    return Ticket{seq};
}

// ---- Senses ----------------------------------------------------------------

const char* name_of(SenseRefusal r) noexcept {
    switch (r) {
    case SenseRefusal::None:
        return "None";
    case SenseRefusal::NoClaim:
        return "NoClaim";
    case SenseRefusal::NotAuthorized:
        return "NotAuthorized";
    case SenseRefusal::Undeclared:
        return "Undeclared";
    case SenseRefusal::OfficeNotHeld:
        return "OfficeNotHeld";
    case SenseRefusal::GateRefused:
        return "GateRefused";
    }
    return "?";
}

const std::shared_ptr<const Schema>* Switchboard::declared_claim(const WeaveRecord& rec,
                                                                 std::string_view name,
                                                                 std::uint32_t version) {
    for (const auto& s : rec.claims) {
        if (s && s->name() == name && s->version() == version) {
            return &s;
        }
    }
    return nullptr;
}

bool Switchboard::declares_claim(const WeaveRecord& rec, std::string_view name,
                                 std::uint32_t version) const {
    return declared_claim(rec, name, version) != nullptr;
}

Switchboard::MadeClaim Switchboard::make_claim(const WeaveRecord& rec, Value value,
                                               std::uint64_t previous_revision) {
    const std::string name = value.schema().name();
    const std::uint32_t version = value.schema().version();
    const std::shared_ptr<const Schema>* declared = declared_claim(rec, name, version);
    if (declared == nullptr) {
        // A weave claims only what it declared. This is what makes the claim-set
        // a real contract rather than documentation, and what makes discovery
        // answerable before the first runtime claim.
        return MadeClaim{std::nullopt, SenseClaimResult{false, SenseRefusal::Undeclared, 0}};
    }
    // THE DOOR IS THE DECLARATION ITSELF, never a registry lookup with a
    // register-if-missing fallback. The record's own claim-set already holds the
    // canonical schema and the weave's live claim is what keeps it resolvable, so
    // reading it from the record denies the claim door any way to publish
    // vocabulary — and it cannot drift from what `declares_claim` just matched.
    // SENSE-04; docs/laws/sense-laws.md
    const std::shared_ptr<const Schema>& door = *declared;
    // The same one gate every value crosses. A malformed claim is refused, not
    // stored: a repository holding an unadmitted value would be the one place in
    // Loom where a value was trusted without passing the gate.
    Admission a = loom::admit(std::move(value), *door);
    if (!a.ok()) {
        return MadeClaim{std::nullopt, SenseClaimResult{false, SenseRefusal::GateRefused, 0}};
    }
    // REPLACE, NEVER ACCUMULATE. The revision advances; the number of retained
    // claims does not. That is the whole of the lifecycle rule on the write side.
    const std::uint64_t revision = previous_revision + 1;
    return MadeClaim{ClaimRecord{std::move(a).value(), rec.id, rec.life, rec.incarnation, revision},
                     SenseClaimResult{true, SenseRefusal::None, revision}};
}

SenseClaimResult Switchboard::claim_as(WeaveId claimant, Value value) {
    auto it = weaves_.find(claimant.value);
    if (it == weaves_.end() || !it->second.alive) {
        // A weave that is not live has no declared claim-set to check against.
        return SenseClaimResult{false, SenseRefusal::Undeclared, 0};
    }
    const PersonalKey key{claimant.value, value.schema().name(), value.schema().version()};
    auto slot = personal_claims_.find(key);
    const std::uint64_t previous =
        slot == personal_claims_.end() ? 0 : slot->second.revision;

    MadeClaim made = make_claim(it->second, std::move(value), previous);
    if (!made.result.accepted) {
        return made.result;
    }
    if (slot == personal_claims_.end()) {
        personal_claims_.emplace(key, std::move(*made.record));
    } else {
        slot->second = std::move(*made.record);
    }
    // Joint publication: the claimant's own claim is the newest fact under this
    // key, so any operation that bound the previous revision is stale — aborted
    // now, its offers released, rather than at its commit.
    abort_joint_on_claim(key);
    return made.result;
}

SenseClaimResult Switchboard::office_claim_as(WeaveId claimant, std::string_view as_role,
                                              Value value) {
    auto it = weaves_.find(claimant.value);
    if (it == weaves_.end() || !it->second.alive) {
        return SenseClaimResult{false, SenseRefusal::OfficeNotHeld, 0};
    }
    // THE MSG-07 RULE, at the claim moment. Holding is necessary and not
    // sufficient; not holding refuses and stores NOTHING — never a downgrade to
    // a personal claim, which would silently answer a different question.
    if (as_role.empty() || !holds_role_now(claimant, as_role)) {
        return SenseClaimResult{false, SenseRefusal::OfficeNotHeld, 0};
    }
    const OfficeKey key{std::string(as_role), value.schema().name(), value.schema().version()};
    auto slot = office_claims_.find(key);
    const std::uint64_t previous = slot == office_claims_.end() ? 0 : slot->second.revision;

    MadeClaim made = make_claim(it->second, std::move(value), previous);
    if (!made.result.accepted) {
        return made.result;
    }
    if (slot == office_claims_.end()) {
        office_claims_.emplace(key, std::move(*made.record));
    } else {
        slot->second = std::move(*made.record);
    }
    return made.result;
}

SenseAuthorship Switchboard::authorship_of(const ClaimRecord& rec, const std::string& office,
                                           const std::string& name,
                                           std::uint32_t version) const {
    SenseAuthorship by;
    by.author = rec.author;
    by.author_life = rec.author_life;
    by.author_incarnation = rec.author_incarnation;
    // ASKED NOW, NEVER STORED. A stored "is current" would be a fact that goes
    // stale inside the repository — the exact failure the whole design refuses.
    by.author_life_is_current = life_of(rec.author) == rec.author_life;
    // ASKED OF THE TOPOLOGY SEPARATELY, never derived from the line above. A
    // live replacement advances the incarnation while the life stands, so the
    // two answers genuinely differ; the life must also match, because a fresh
    // life at the same address restarts the incarnation counter and an
    // incarnation-only comparison would call that a match.
    by.author_incarnation_is_current =
        by.author_life_is_current && incarnation_of(rec.author) == rec.author_incarnation;
    by.office = office;
    by.office_holder_is_current =
        !office.empty() && role_holder(office).value == rec.author.value;
    by.revision = rec.revision;
    by.schema_name = name;
    by.schema_version = version;
    return by;
}

SenseReading Switchboard::observe(WeaveId author, std::string_view shape_name,
                                  std::uint32_t shape_version) const {
    const PersonalKey key{author.value, std::string(shape_name), shape_version};
    auto it = personal_claims_.find(key);
    if (it == personal_claims_.end()) {
        return SenseReading{SenseRefusal::NoClaim, {}, std::nullopt};
    }
    SenseReading out;
    out.refusal = SenseRefusal::None;
    out.by = authorship_of(it->second, std::string{}, std::string(shape_name), shape_version);
    out.value = it->second.value; // BY VALUE: the reader owns its copy, always
    return out;
}

SenseReading Switchboard::observe_office(std::string_view role, std::string_view shape_name,
                                         std::uint32_t shape_version) const {
    const OfficeKey key{std::string(role), std::string(shape_name), shape_version};
    auto it = office_claims_.find(key);
    if (it == office_claims_.end()) {
        return SenseReading{SenseRefusal::NoClaim, {}, std::nullopt};
    }
    SenseReading out;
    out.refusal = SenseRefusal::None;
    out.by = authorship_of(it->second, std::string(role), std::string(shape_name), shape_version);
    out.value = it->second.value;
    return out;
}

SenseReading Switchboard::observe_as(WeaveId reader, WeaveId author, std::string_view shape_name,
                                     std::uint32_t shape_version) const {
    auto it = weaves_.find(reader.value);
    // Effective observe authority, read at the moment of the read (GATE-05) — the
    // same live-value discipline the send path follows, and the reason observation
    // is delegable at all: nothing here was decided earlier and cached.
    if (it == weaves_.end() || !effective_permits_observe(it->second.grant.live(),
                                                         it->second.delegated, shape_name,
                                                         shape_version)) {
        // Refused BEFORE the lookup, so an unauthorized reader cannot learn
        // whether a claim exists — the same discipline role authorization
        // follows, where authorization happens before role resolution.
        return SenseReading{SenseRefusal::NotAuthorized, {}, std::nullopt};
    }
    return observe(author, shape_name, shape_version);
}

SenseReading Switchboard::observe_office_as(WeaveId reader, std::string_view role,
                                            std::string_view shape_name,
                                            std::uint32_t shape_version) const {
    auto it = weaves_.find(reader.value);
    if (it == weaves_.end() || !effective_permits_observe(it->second.grant.live(),
                                                         it->second.delegated, shape_name,
                                                         shape_version)) {
        return SenseReading{SenseRefusal::NotAuthorized, {}, std::nullopt};
    }
    return observe_office(role, shape_name, shape_version);
}

std::vector<std::shared_ptr<const Schema>> Switchboard::claimed_schemas(WeaveId id) const {
    auto it = weaves_.find(id.value);
    return it == weaves_.end() ? std::vector<std::shared_ptr<const Schema>>{} : it->second.claims;
}

std::vector<std::shared_ptr<const Schema>> Switchboard::emitted_schemas(WeaveId id) const {
    auto it = weaves_.find(id.value);
    return it == weaves_.end() ? std::vector<std::shared_ptr<const Schema>>{} : it->second.emits;
}

void Switchboard::forget_personal_claims(WeaveId id) {
    for (auto it = personal_claims_.begin(); it != personal_claims_.end();) {
        it = std::get<0>(it->first) == id.value ? personal_claims_.erase(it) : std::next(it);
    }
}

void Switchboard::forget_office_claims(const std::string& role) {
    for (auto it = office_claims_.begin(); it != office_claims_.end();) {
        it = std::get<0>(it->first) == role ? office_claims_.erase(it) : std::next(it);
    }
}

// ---- Joint publication of latest claims -----------------------------------
// The account is in zen/switchboard/sense.hpp and docs/reference/joint-publication.md; here
// is the mechanism, in the order a commitment happens: bind, offer, revalidate, exchange, and
// the hook that shows a claimant its published value before anything can observe it.

const char* name_of(JointRefusal r) noexcept {
    switch (r) {
    case JointRefusal::None: return "None";
    case JointRefusal::NoLiveDelivery: return "NoLiveDelivery";
    case JointRefusal::ForeignAuthority: return "ForeignAuthority";
    case JointRefusal::NotOperator: return "NotOperator";
    case JointRefusal::OutsideCeiling: return "OutsideCeiling";
    case JointRefusal::NoClaim: return "NoClaim";
    case JointRefusal::KeyBusy: return "KeyBusy";
    case JointRefusal::Exhausted: return "Exhausted";
    case JointRefusal::NoSuchOperation: return "NoSuchOperation";
    case JointRefusal::WrongState: return "WrongState";
    case JointRefusal::NotBound: return "NotBound";
    case JointRefusal::NotClaimant: return "NotClaimant";
    case JointRefusal::StaleRevision: return "StaleRevision";
    case JointRefusal::Undeclared: return "Undeclared";
    case JointRefusal::GateRefused: return "GateRefused";
    case JointRefusal::TooLarge: return "TooLarge";
    case JointRefusal::ParticipantChanged: return "ParticipantChanged";
    case JointRefusal::OfferMissing: return "OfferMissing";
    case JointRefusal::Cancelled: return "Cancelled";
    }
    return "?";
}

const char* name_of(JointState s) noexcept {
    switch (s) {
    case JointState::Missing: return "Missing";
    case JointState::Preparing: return "Preparing";
    case JointState::Committed: return "Committed";
    case JointState::Aborted: return "Aborted";
    }
    return "?";
}

const char* name_of(JointApplication a) noexcept {
    switch (a) {
    case JointApplication::None: return "None";
    case JointApplication::Pending: return "Pending";
    case JointApplication::Applied: return "Applied";
    case JointApplication::Failed: return "Failed";
    case JointApplication::Lost: return "Lost";
    case JointApplication::Declined: return "Declined";
    }
    return "?";
}

namespace {
/// THE WORST-FIRST ORDER every aggregate uses: Failed over Lost over Declined over
/// Pending over Applied over None. A held owner outranks a missing one, which
/// outranks one that answered no, which outranks one still to be shown.
int rank_of(JointApplication a) noexcept {
    switch (a) {
    case JointApplication::Failed: return 5;
    case JointApplication::Lost: return 4;
    case JointApplication::Declined: return 3;
    case JointApplication::Pending: return 2;
    case JointApplication::Applied: return 1;
    case JointApplication::None: return 0;
    }
    return 0;
}
} // namespace

Switchboard::JointOp* Switchboard::find_joint(std::uint64_t id) noexcept {
    if (id == 0) {
        return nullptr;
    }
    for (JointOp& op : joint_ops_) {
        if (op.id == id) {
            return &op;
        }
    }
    return nullptr;
}

const Switchboard::JointOp* Switchboard::find_joint(std::uint64_t id) const noexcept {
    return const_cast<Switchboard*>(this)->find_joint(id);
}

void Switchboard::finish_joint(JointOp& op, JointState state, JointRefusal reason) noexcept {
    // TERMINAL, AND THE OFFERS GO WITH IT. Whichever way it ended, nothing of
    // what was offered is retained: a committed offer already lives in its claim
    // record, an aborted one was never anybody's value.
    op.state = state;
    op.reason = reason;
    for (JointPart& p : op.parts) {
        p.offer.reset();
        p.offer_bytes = 0;
    }
}

JointAuthority Switchboard::mint_joint_authority(WeaveId operator_id,
                                                 std::vector<std::string> ceiling_roles) const {
    // The participant as it is now, or nothing: the same three facts a bound claimant is held
    // by (`participant`), so a successor incarnation at the same address cannot present it.
    const WeaveRecord* rec = find(operator_id);
    if (rec == nullptr || !rec->alive) {
        return JointAuthority{}; // no live participant to bind: not valid()
    }
    return JointAuthority{identity_, operator_id, rec->life, rec->incarnation,
                          std::move(ceiling_roles)};
}

JointRefusal Switchboard::joint_authority_check(WeaveId caller,
                                                const JointAuthority& authority) const {
    // THE LIVE DELIVERY IS HALF THE CHECK — the same discipline every deferred
    // spend keeps: the weave speaking now must be the weave the WeaveBus stamps,
    // and it must be speaking from inside its own delivery.
    if (!current_target_.valid() || !(caller == current_target_)) {
        return JointRefusal::NoLiveDelivery;
    }
    if (!authority.valid() || !issued_here(authority)) {
        return JointRefusal::ForeignAuthority;
    }
    if (!(authority.operator_id() == caller)) {
        return JointRefusal::NotOperator;
    }
    // THE EXACT PARTICIPANT THE AUTHORITY WAS MINTED FOR, asked the way every bound
    // participant is asked (`still`): alive, and at the life and incarnation the
    // host minted it for. A swap, a revival or a removal-and-replacement leaves a
    // weave at this address that the capability does not name; the records that
    // operator began were retired at that transition, and its capability is refused
    // here -- two obligations, both kept. The host mints again for the successor.
    if (!still(ParticipantRef{caller, authority.operator_life(),
                              authority.operator_incarnation()})) {
        return JointRefusal::NotOperator;
    }
    return JointRefusal::None;
}

JointBegin Switchboard::begin_joint_as(WeaveId caller, const JointAuthority& authority,
                                       std::vector<ClaimKey> keys) {
    if (const JointRefusal why = joint_authority_check(caller, authority);
        why != JointRefusal::None) {
        return JointBegin{false, 0, why};
    }
    if (keys.empty() || keys.size() > kMaxJointKeys) {
        return JointBegin{false, 0, JointRefusal::Exhausted};
    }
    std::vector<JointPart> parts;
    parts.reserve(keys.size());
    for (ClaimKey key : keys) {
        const std::string named_role = key.role; // kept for the words a failure is told in
        // A KEY NAMED BY ROLE IS RESOLVED NOW, to the exact holder, and bound as that
        // weave: what the parts record is always a participant, never an office.
        if (!key.role.empty()) {
            if (key.claimant.valid()) {
                return JointBegin{false, 0, JointRefusal::NoClaim}; // both: a contradiction
            }
            const auto held = roles_.find(key.role);
            if (held == roles_.end()) {
                return JointBegin{false, 0, JointRefusal::NoClaim};
            }
            key.claimant = held->second;
            key.role.clear();
        }
        for (const JointPart& already : parts) {
            if (already.key == key) {
                return JointBegin{false, 0, JointRefusal::KeyBusy}; // named twice
            }
        }
        const WeaveRecord* claimant = find(key.claimant);
        if (claimant == nullptr || !claimant->alive) {
            return JointBegin{false, 0, JointRefusal::NoClaim};
        }
        // THE CEILING IS A SET OF ROLES, RESOLVED NOW: the claimant must hold one
        // of them at this moment. What is bound is the exact participant, so a
        // role that moves afterwards does not move the operation with it.
        bool within = false;
        for (const std::string& role : authority.ceiling()) {
            const auto held = roles_.find(role);
            if (held != roles_.end() && held->second == key.claimant) {
                within = true;
                break;
            }
        }
        if (!within) {
            return JointBegin{false, 0, JointRefusal::OutsideCeiling};
        }
        const PersonalKey pk{key.claimant.value, key.schema_name, key.schema_version};
        const auto record = personal_claims_.find(pk);
        if (record == personal_claims_.end()) {
            return JointBegin{false, 0, JointRefusal::NoClaim};
        }
        // ONE LIVE OPERATION PER KEY. Supersession is the operator's explicit act
        // (`cancel_joint`), never something begin performs on its behalf.
        for (const JointOp& live : joint_ops_) {
            if (live.state != JointState::Preparing) {
                continue;
            }
            for (const JointPart& p : live.parts) {
                if (p.key == key) {
                    return JointBegin{false, 0, JointRefusal::KeyBusy};
                }
            }
        }
        JointPart part;
        part.key = key;
        part.claimant = participant(key.claimant);
        // THE OFFICE A FAILURE WILL BE TOLD IN: the one the key was named by, or --
        // for a key named by weave -- the one the claimant holds at this moment. A
        // fact about the bound participant, recorded now, so a later role movement
        // does not rewrite whom the operator was coordinating.
        part.bound_role = named_role.empty() ? claimant->role : named_role;
        part.revision = record->second.revision;
        parts.push_back(std::move(part));
    }
    for (JointOp& slot : joint_ops_) {
        if (slot.state != JointState::Missing) {
            // A slot is never reused while its operator has not released it: a Preparing
            // record is live, a Committed one is re-read when the word of its application
            // arrives, an Aborted one when the word that it ended does. Only a released slot
            // is free, and a non-releasing operator meets the bound below, in words.
            continue;
        }
        slot = JointOp{next_joint_id_++, participant(caller), JointState::Preparing,
                       JointRefusal::None, std::move(parts), false};
        return JointBegin{true, slot.id, JointRefusal::None};
    }
    return JointBegin{false, 0, JointRefusal::Exhausted};
}

JointResult Switchboard::offer_claim_as(WeaveId claimant, std::uint64_t op_id, Value value) {
    if (!current_target_.valid() || !(claimant == current_target_)) {
        return JointResult{false, JointRefusal::NoLiveDelivery};
    }
    JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointResult{false, JointRefusal::NoSuchOperation};
    }
    if (op->state != JointState::Preparing) {
        return JointResult{false, JointRefusal::WrongState};
    }
    WeaveRecord* rec = find(claimant);
    if (rec == nullptr || !rec->alive) {
        return JointResult{false, JointRefusal::NotClaimant};
    }
    const ClaimKey key{claimant, std::string(), value.schema().name(), value.schema().version()};
    JointPart* part = nullptr;
    for (JointPart& p : op->parts) {
        if (p.key == key) {
            part = &p;
            break;
        }
    }
    if (part == nullptr) {
        // Bound for somebody else, or not bound at all — two different mistakes.
        for (const JointPart& p : op->parts) {
            if (p.key.schema_name == key.schema_name &&
                p.key.schema_version == key.schema_version && !(p.key.claimant == claimant)) {
                return JointResult{false, JointRefusal::NotClaimant};
            }
        }
        return JointResult{false, JointRefusal::NotBound};
    }
    if (!still(part->claimant)) {
        finish_joint(*op, JointState::Aborted, JointRefusal::ParticipantChanged);
        return JointResult{false, JointRefusal::ParticipantChanged};
    }
    const PersonalKey pk{claimant.value, key.schema_name, key.schema_version};
    const auto record = personal_claims_.find(pk);
    if (record == personal_claims_.end()) {
        finish_joint(*op, JointState::Aborted, JointRefusal::NoClaim);
        return JointResult{false, JointRefusal::NoClaim};
    }
    if (record->second.revision != part->revision) {
        finish_joint(*op, JointState::Aborted, JointRefusal::StaleRevision);
        return JointResult{false, JointRefusal::StaleRevision};
    }
    // THE SAME ONE GATE, AGAINST THE SAME DECLARED CLAIM-SET, as an ordinary claim.
    MadeClaim made = make_claim(*rec, std::move(value), record->second.revision);
    if (!made.result.accepted) {
        return JointResult{false, made.result.why == SenseRefusal::Undeclared
                                      ? JointRefusal::Undeclared
                                      : JointRefusal::GateRefused};
    }
    const std::size_t bytes = loom::serialize(made.record->value).size();
    if (bytes > kMaxJointOfferBytes) {
        return JointResult{false, JointRefusal::TooLarge};
    }
    part->offer = std::move(made.record->value);
    part->offer_bytes = bytes;
    return JointResult{true, JointRefusal::None};
}

JointResult Switchboard::commit_joint_as(WeaveId caller, const JointAuthority& authority,
                                         std::uint64_t op_id) {
    if (const JointRefusal why = joint_authority_check(caller, authority);
        why != JointRefusal::None) {
        return JointResult{false, why};
    }
    JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointResult{false, JointRefusal::NoSuchOperation};
    }
    // Somebody else's record is refused before it is touched: an operation id is never
    // authority, and a caller naming another operator's operation is not its operator. Asked
    // first and with no effect, so the record's state, reason, offers and notices owed stay
    // as they were, and its owner's own commit still finds it.
    if (!(op->operator_.who == caller)) {
        return JointResult{false, JointRefusal::NotOperator};
    }
    if (op->state != JointState::Preparing) {
        return JointResult{false, JointRefusal::WrongState};
    }
    // THE OPERATOR'S OWN RECORD, BOUND TO A LIFE OR INCARNATION THAT MOVED. Not
    // reachable in the ordinary course -- `invalidate_joint_for` retires every record
    // of an operator whose life or incarnation changed, at that transition, and the
    // authority check above refuses a caller the capability does not name -- but if
    // it is ever met, it is the lifecycle rule that answers, for the record's own
    // operator: aborted, as a bound claimant's change aborts it, never a stranger's act.
    if (!still(op->operator_)) {
        finish_joint(*op, JointState::Aborted, JointRefusal::ParticipantChanged);
        return JointResult{false, JointRefusal::ParticipantChanged};
    }
    // REVALIDATE EVERYTHING FIRST, AND ALLOCATE WHAT THE EXCHANGE NEEDS NOW. Past
    // this loop nothing can fail and nothing can run but this function.
    std::vector<ClaimRecord*> targets;
    targets.reserve(op->parts.size());
    for (const JointPart& p : op->parts) {
        if (!still(p.claimant)) {
            finish_joint(*op, JointState::Aborted, JointRefusal::ParticipantChanged);
            return JointResult{false, JointRefusal::ParticipantChanged};
        }
        const PersonalKey pk{p.key.claimant.value, p.key.schema_name, p.key.schema_version};
        const auto record = personal_claims_.find(pk);
        if (record == personal_claims_.end()) {
            finish_joint(*op, JointState::Aborted, JointRefusal::NoClaim);
            return JointResult{false, JointRefusal::NoClaim};
        }
        if (record->second.revision != p.revision) {
            finish_joint(*op, JointState::Aborted, JointRefusal::StaleRevision);
            return JointResult{false, JointRefusal::StaleRevision};
        }
        if (!p.offer.has_value()) {
            finish_joint(*op, JointState::Aborted, JointRefusal::OfferMissing);
            return JointResult{false, JointRefusal::OfferMissing};
        }
        targets.push_back(&record->second);
    }
    // THE PROTECTED EXCHANGE. Bus-private records only: a Value swap moves
    // pointers, three integers are assigned, a flag is set. No participant
    // code, no gate, no allocation, no observer, no I/O and no lifecycle path
    // runs between two iterations, and nothing below can throw. The old values
    // go into the parts and are released together, after the loop.
    for (std::size_t i = 0; i < op->parts.size(); ++i) {
        JointPart& p = op->parts[i];
        ClaimRecord& record = *targets[i];
        std::swap(record.value, *p.offer);
        record.author = p.claimant.who;
        record.author_life = p.claimant.life;
        record.author_incarnation = p.claimant.incarnation;
        ++record.revision;
        // PUBLISHED, NOT YET APPLIED: the claimant is owed a showing, and what it
        // comes to is recorded here and on the part, attributed to this operation
        // and this revision.
        record.application = JointApplication::Pending;
        record.published_by = op->id;
        record.published_revision = record.revision;
        p.application = JointApplication::Pending;
    }
    finish_joint(*op, JointState::Committed, JointRefusal::None); // releases the old values
    return JointResult{true, JointRefusal::None};
}

JointResult Switchboard::cancel_joint_as(WeaveId caller, const JointAuthority& authority,
                                         std::uint64_t op_id) {
    if (const JointRefusal why = joint_authority_check(caller, authority);
        why != JointRefusal::None) {
        return JointResult{false, why};
    }
    JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointResult{false, JointRefusal::NoSuchOperation};
    }
    if (!(op->operator_.who == caller)) {
        return JointResult{false, JointRefusal::NotOperator};
    }
    if (op->state != JointState::Preparing) {
        return JointResult{false, JointRefusal::WrongState};
    }
    // CANCELLED IS TERMINAL, NOT RELEASED: the record stays readable (Aborted,
    // Cancelled) until this operator releases it, like every other terminal record.
    finish_joint(*op, JointState::Aborted, JointRefusal::Cancelled);
    return JointResult{true, JointRefusal::None};
}

JointResult Switchboard::release_joint_as(WeaveId caller, const JointAuthority& authority,
                                          std::uint64_t op_id) {
    if (const JointRefusal why = joint_authority_check(caller, authority);
        why != JointRefusal::None) {
        return JointResult{false, why};
    }
    JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointResult{false, JointRefusal::NoSuchOperation}; // never begun, or released
    }
    if (!(op->operator_.who == caller)) {
        return JointResult{false, JointRefusal::NotOperator};
    }
    if (op->state == JointState::Preparing) {
        return JointResult{false, JointRefusal::WrongState}; // live: cancel it, do not lose it
    }
    // The operator's own retirement of a record it has consumed (SENSE-07): Committed, settled
    // or still owed, or Aborted. No later re-settlement is told to anybody, because the record
    // that would carry the word is gone; the id names nothing from here. A held claimant stays
    // held and is still repaired by a reload, and its next ordinary claim replaces the value.
    retire_joint(*op);
    return JointResult{true, JointRefusal::None};
}

void Switchboard::retire_joint(JointOp& op) noexcept {
    op = JointOp{}; // Missing, id 0, parts and offers gone; the id is never handed out again
}

JointStatus Switchboard::joint_status_as(WeaveId caller, const JointAuthority& authority,
                                         std::uint64_t op_id) {
    if (const JointRefusal why = joint_authority_check(caller, authority);
        why != JointRefusal::None) {
        return JointStatus{JointState::Missing, why};
    }
    const JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointStatus{JointState::Missing, JointRefusal::NoSuchOperation};
    }
    if (!(op->operator_.who == caller)) {
        return JointStatus{JointState::Missing, JointRefusal::NotOperator};
    }
    return status_of(*op);
}

JointStatus Switchboard::joint_status(std::uint64_t op_id) const noexcept {
    const JointOp* op = find_joint(op_id);
    if (op == nullptr) {
        return JointStatus{JointState::Missing, JointRefusal::NoSuchOperation};
    }
    return status_of(*op);
}

JointStatus Switchboard::status_of(const JointOp& op) const {
    JointStatus out;
    out.state = op.state;
    out.reason = op.reason;
    if (op.state != JointState::Committed) {
        return out; // nothing was published, so nothing is owed
    }
    // THE WORST PART DECIDES, and it is named: Failed over Lost over Declined over
    // Pending over Applied. A non-application is about ONE participant, and the
    // operator's words to a requester need to say which.
    out.application = JointApplication::Applied;
    const JointPart* about = nullptr;
    for (const JointPart& p : op.parts) {
        if (rank_of(p.application) > rank_of(out.application)) {
            out.application = p.application;
            about = (p.application == JointApplication::Failed ||
                     p.application == JointApplication::Lost ||
                     p.application == JointApplication::Declined)
                        ? &p
                        : nullptr;
        }
    }
    if (about != nullptr) {
        out.failed = about->key.claimant;
        out.failed_role = about->bound_role;
    }
    return out;
}

std::size_t Switchboard::joint_pending() const noexcept {
    std::size_t n = 0;
    for (const JointOp& op : joint_ops_) {
        n += op.state == JointState::Preparing ? 1 : 0;
    }
    return n;
}

std::size_t Switchboard::joint_records() const noexcept {
    std::size_t n = 0;
    for (const JointOp& op : joint_ops_) {
        n += op.state != JointState::Missing ? 1 : 0; // live, or terminal and unreleased
    }
    return n;
}

std::size_t Switchboard::joint_retained_bytes() const noexcept {
    std::size_t n = 0;
    for (const JointOp& op : joint_ops_) {
        for (const JointPart& p : op.parts) {
            n += p.offer.has_value() ? p.offer_bytes : 0;
        }
    }
    return n;
}

bool Switchboard::has_unobserved_publication(WeaveId id) const noexcept {
    for (const auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) == id.value &&
            entry.second.application == JointApplication::Pending) {
            return true;
        }
    }
    return false;
}

bool Switchboard::has_failed_application(WeaveId id) const noexcept {
    for (const auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) == id.value &&
            entry.second.application == JointApplication::Failed) {
            return true;
        }
    }
    return false;
}

JointApplication Switchboard::application_of(WeaveId id) const noexcept {
    JointApplication worst = JointApplication::None;
    for (const auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) == id.value &&
            rank_of(entry.second.application) > rank_of(worst)) {
            worst = entry.second.application;
        }
    }
    return worst;
}

void Switchboard::invalidate_joint_for(WeaveId changed) {
    // SELECTIVE, exactly as `invalidate_transactions_for` is: only operations
    // that BIND `changed`, and only when the fact they captured no longer holds.
    // A live operation is ended; an operation the bus had already ended (a
    // bound claim moved) is not touched -- but its operator is told now, if it
    // was not told yet, because the reply it may be waiting on from this
    // participant can no longer come.
    for (JointOp& op : joint_ops_) {
        if (op.state == JointState::Missing) {
            continue;
        }
        if (op.operator_.who == changed && !still(op.operator_)) {
            // The operator is replaced, removed, dead or revived (SENSE-07): nobody is left
            // to consume this record, and a successor inherits nothing, so it is retired now,
            // whatever its state. The claimants lose nothing: their facts live on their claim
            // records, so a hold, a repair and an owner's next claim are what they were.
            retire_joint(op);
            continue;
        }
        if (op.state != JointState::Preparing && op.state != JointState::Aborted) {
            continue;
        }
        for (const JointPart& p : op.parts) {
            if (p.key.claimant == changed && !still(p.claimant)) {
                if (op.state == JointState::Preparing) {
                    finish_joint(op, JointState::Aborted, JointRefusal::ParticipantChanged);
                }
                notify_joint_ended(op);
                break;
            }
        }
    }
    // Committed operations: a claimant removed before it was shown its published value is
    // Lost (its claim record went with it), and the operator is told. A reloaded or dead
    // claimant keeps its record, and its successor or revived life is shown. A part already
    // Failed keeps that word: its failure was told, and removal is the repair that ends the hold.
    if (find(changed) != nullptr) {
        return;
    }
    for (JointOp& op : joint_ops_) {
        if (op.state != JointState::Committed) {
            continue;
        }
        bool moved = false;
        for (JointPart& p : op.parts) {
            if (p.key.claimant == changed && p.application == JointApplication::Pending) {
                p.application = JointApplication::Lost;
                moved = true;
            }
        }
        if (moved) {
            settle_application(op);
        }
    }
}

void Switchboard::notify_joint_ended(JointOp& op) {
    // TO THE EXACT OPERATOR THAT BEGAN IT, ONCE, and only if it accepts the shape: an
    // ordinary ungated delivery from the bus (no sender life to stamp), refused
    // NotAccepted otherwise and seeding no refusal traffic because it has no weave sender.
    if (op.notified) {
        return;
    }
    op.notified = true;
    const WeaveRecord* who = find(op.operator_.who);
    if (who == nullptr || !who->alive || !still(op.operator_) ||
        !accept_match(*who, JointEnded::zen_name, JointEnded::zen_version)) {
        return;
    }
    JointEnded notice;
    notice.op = std::to_string(op.id);
    notice.reason = name_of(op.reason);
    const std::uint64_t seq = allocate_sequence();
    Message msg(to_value(notice), WeaveId{}, WeaveId{}, 0);
    Envelope env{std::move(msg), op.operator_.who, seq, false, {}, 0};
    env.fence = current_dispatch_fence_;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    queue_back(std::move(env));
}

void Switchboard::abort_joint_on_claim(const PersonalKey& key) {
    for (JointOp& op : joint_ops_) {
        if (op.state != JointState::Preparing) {
            continue;
        }
        for (const JointPart& p : op.parts) {
            if (p.key.claimant.value == std::get<0>(key) &&
                p.key.schema_name == std::get<1>(key) &&
                p.key.schema_version == std::get<2>(key)) {
                finish_joint(op, JointState::Aborted, JointRefusal::StaleRevision);
                break; // the owner that moved it is alive: it answers, in its own words
            }
        }
    }
}

Switchboard::Showing Switchboard::observe_published_claims(WeaveRecord& rec) {
    Showing out;
    // ALREADY HELD: nothing is shown again. Re-running a hook that did not complete
    // would retry whatever it half-did, on every read; the hold ends with a reload
    // (the successor is shown) or a removal, and with nothing else.
    for (const auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) == rec.id.value &&
            entry.second.application == JointApplication::Failed) {
            out.failed = true;
            out.held = true;
            out.op = entry.second.published_by;
            return out;
        }
    }
    // COLLECT THE KEYS FIRST, in a fixed order -- the publishing operation, then the
    // key -- because the hook runs weave code, and the iteration must not depend on
    // what that code does. The value each hook is shown is a COPY: no reach into
    // the record, exactly as an observer gets none.
    std::vector<std::pair<std::uint64_t, PersonalKey>> pending;
    for (const auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) == rec.id.value &&
            entry.second.application == JointApplication::Pending) {
            pending.emplace_back(entry.second.published_by, entry.first);
        }
    }
    std::sort(pending.begin(), pending.end());
    for (const auto& [published_by, key] : pending) {
        auto it = personal_claims_.find(key);
        if (it == personal_claims_.end() || it->second.application != JointApplication::Pending) {
            continue;
        }
        const Value shown = it->second.value;
        Weave::PublishedClaim answer = Weave::PublishedClaim::Failed;
        std::exception_ptr error;
        try {
            answer = rec.weave->claim_published(shown);
        } catch (...) {
            error = std::current_exception();
        }
        it = personal_claims_.find(key); // weave code ran: look again before writing
        if (it == personal_claims_.end()) {
            continue;
        }
        if (!error && answer == Weave::PublishedClaim::Applied) {
            note_application(key, it->second, JointApplication::Applied);
            continue;
        }
        if (!error && answer == Weave::PublishedClaim::Declined) {
            // DECLINED IS AN ANSWER, NOT A FAILURE:
            // the owner is functioning and keeps state of its own; it is not held, the
            // showing goes on to its next key, and the published value stands on this
            // record -- attributed Declined -- until the owner's next ordinary claim
            // replaces it. What a successor says when shown what its predecessor
            // prepared; what a snapshot or a delivery then finds is the owner's own truth.
            note_application(key, it->second, JointApplication::Declined);
            continue;
        }
        // THE FIRST FAILURE STOPS THE SHOWING. This key is Failed -- the weave is
        // held from here -- and every key after it stays Pending: never attempted,
        // so nothing of it is partial, and the successor of a reload is shown all
        // of them. The native exception, if there was one, goes back to the caller
        // AFTER the record says what happened, so nothing is swallowed and nothing
        // is lost with it.
        note_application(key, it->second, JointApplication::Failed);
        out.failed = true;
        out.op = published_by;
        out.error = error;
        break;
    }
    return out;
}

void Switchboard::note_application(const PersonalKey& key, ClaimRecord& record,
                                   JointApplication what) {
    record.application = what;
    JointOp* op = find_joint(record.published_by);
    if (op == nullptr) {
        // RELEASED SINCE -- by its operator, or by the bus when that operator changed.
        // The claim record alone keeps the fact, which is all the hold, the repair and
        // the owner's next claim ever read; nothing is owed to anybody about it.
        return;
    }
    for (JointPart& p : op->parts) {
        if (p.key.claimant.value == std::get<0>(key) && p.key.schema_name == std::get<1>(key) &&
            p.key.schema_version == std::get<2>(key)) {
            p.application = what;
            break;
        }
    }
    settle_application(*op);
}

void Switchboard::settle_application(JointOp& op) {
    const JointStatus now = status_of(op);
    if (now.application == JointApplication::Pending || now.application == JointApplication::None) {
        return; // still Pending somewhere: not settled
    }
    if (now.application == op.applied_told) {
        return; // said once
    }
    op.applied_told = now.application;
    const JointPart* about = nullptr;
    for (const JointPart& p : op.parts) {
        if (p.key.claimant == now.failed && p.application == now.application) {
            about = &p;
            break;
        }
    }
    notify_joint_applied(op, now.application, about);
}

void Switchboard::notify_joint_applied(JointOp& op, JointApplication what, const JointPart* about) {
    // TO THE EXACT OPERATOR THAT BEGAN IT, and only if it accepts the shape: the
    // same ungated bus notice `zen.JointEnded` is (no sender life to stamp, no
    // refusal traffic seeded). The record is the fact; this is the wake-up.
    const WeaveRecord* who = find(op.operator_.who);
    if (who == nullptr || !who->alive || !still(op.operator_) ||
        !accept_match(*who, JointApplied::zen_name, JointApplied::zen_version)) {
        return;
    }
    JointApplied notice;
    notice.op = std::to_string(op.id);
    notice.applied = what == JointApplication::Applied;
    notice.claimant = about == nullptr ? std::string() : std::to_string(about->key.claimant.value);
    notice.role = about == nullptr ? std::string() : about->bound_role;
    notice.reason = name_of(what);
    const std::uint64_t seq = allocate_sequence();
    Message msg(to_value(notice), WeaveId{}, WeaveId{}, 0);
    Envelope env{std::move(msg), op.operator_.who, seq, false, {}, 0};
    env.fence = current_dispatch_fence_;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    queue_back(std::move(env));
}

void Switchboard::reset_failed_application_for_successor(WeaveId id) noexcept {
    for (auto& entry : personal_claims_) {
        if (std::get<0>(entry.first) != id.value ||
            entry.second.application != JointApplication::Failed) {
            continue;
        }
        entry.second.application = JointApplication::Pending;
        if (JointOp* op = find_joint(entry.second.published_by)) {
            for (JointPart& p : op->parts) {
                if (p.key.claimant == id && p.application == JointApplication::Failed) {
                    p.application = JointApplication::Pending;
                }
            }
            op->applied_told = JointApplication::None; // the successor's settlement is news
        }
    }
}

void Switchboard::note_seam_refusal(WeaveId sender, WeaveId target, std::string_view claimed_name,
                                    std::uint32_t claimed_version, const Refusal& refusal,
                                    std::string_view addressed_role) {
    // Deliberately `refuse_now`'s body rather than a second mechanism — the only
    // difference is that no admitted Message exists to read a schema off, so the
    // CLAIMED name and version are passed in. Same seq, same journal slot, same
    // tap event: one refusal altitude, whichever side of the seam it happened on.
    const std::uint64_t seq = allocate_sequence();
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{Disposition::Refused, refusal}};
    BusEvent ev;
    ev.kind = EventKind::Refused;
    ev.seq = seq;
    ev.target = target;
    ev.sender = sender;
    ev.schema_name = std::string(claimed_name);
    ev.schema_version = claimed_version;
    // WHERE IT WAS GOING, on the doors that were told. `addressed_role` reads
    // the same here as on a delivery — the office the SENDER named — and it is
    // empty for the doors that named none, exactly as it is for a native
    // directed send and a native publication.
    ev.addressed_role = std::string(addressed_role);
    // No correlation: the emission never became a Message, so there is no
    // envelope to read one off, and manufacturing a 0 that LOOKED chosen would
    // be the fiction this function's own comment refuses everywhere else.
    ev.dispatch_parent = current_dispatch_seq_;
    ev.refusal = refusal;
    emit(ev);
}

void Switchboard::note_handler_failure() noexcept {
    // Guarded by the dispatch state rather than trusted: called outside a
    // delivery there is nothing this could be about, and a bit set then would
    // attach to whatever delivery came next.
    if (current_target_.valid()) {
        handler_reported_failure_ = true;
    }
}

Ticket Switchboard::enqueue_answer(WeaveId to, WeaveId as_sender, Message msg,
                                   std::uint64_t correlation, std::uint64_t requester_life,
                                   std::uint64_t requester_incarnation, TxnId preparation) {
    // Loom chooses the recipient and the correlation; the answerer chooses only
    // what it says. And Loom stamps WHICH requester the answer is for, so the
    // conversation survives in the envelope rather than only in the stack frame or
    // the registry record that produced it.
    msg.sender = as_sender;
    msg.reply_to = WeaveId{};
    msg.correlation = correlation;
    const std::uint64_t seq = allocate_sequence();
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    msg.provenance = Provenance::attested(Provenance::Kind::Answer, 0);
    Envelope env{std::move(msg), to, seq, /*gated=*/true, std::string{}, life_of(as_sender)};
    env.answer_target = AnswerTarget{true, requester_life, requester_incarnation};
    env.dispatch_parent = current_dispatch_seq_; // the delivery this was authored from
    // ...and WHICH ASK is being answered, carried out of the conversation the same
    // way it was carried in. Both answer doors reach this line, so an immediate
    // answer and one deferred across a dozen deliveries prove exactly the same
    // thing — which is why there is one readiness definition rather than two.
    env.preparation = preparation;
    // An answer belongs to the fence of the delivery it is SPENT from, not the ask's: a
    // deferred answer spent from a later, unrelated delivery is that delivery's work.
    env.fence = current_dispatch_fence_;
    queue_back(std::move(env));
    return Ticket{seq};
}

Ticket Switchboard::answer_as(WeaveId as_sender, Message msg) {
    // Three ways to have no authority, each a refusal of authority, distinct from the grant
    // check that still runs on a legitimate answer: nothing is being dispatched or the caller
    // is not the weave being dispatched (a Bus that outlived its delivery); the request came
    // from a root, so there is no requester; or this delivery's one answer is already spent.
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
                          authority_.requester_incarnation, authority_.preparation);
}

// ---- deferred answers (ANS-02) ---------------------------------------------
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
    // A life generation advances exactly when a weave comes back from the dead: registration
    // starts at 1, and a handler returning, an ordinary message and a live code reload leave
    // it alone. `!alive` must be read before the caller marks the weave alive, which is why
    // this is one function rather than a line copied into three revival paths.
    if (!rec.alive) {
        ++rec.life;
    }
}

void Switchboard::abandon_deferred_for(WeaveId id) {
    // A handler may end without ending the conversation; a life may not. The staleness sweep
    // below cannot see this: `kill` leaves the id and incarnation as they were, so a weave
    // revived from its own snapshot would come back holding its predecessor's answer rights.
    // ANS-04; docs/laws/answer-authority-laws.md
    // Both directions, whatever happens next (revival, last-known-good or quarantine): the
    // slot is reclaimed at the transition. Only conversations this weave is a party to.
    for (DeferredRecord& r : deferred_) {
        if (r.token != 0 && (r.respondent == id || r.requester == id)) {
            r = DeferredRecord{}; // the slot is free again
        }
    }
}

void Switchboard::forget_deferred_for(WeaveId id) {
    // Called when new code is committed behind an existing id (`swap_state`), and only there:
    // the incarnation that earned an unfinished conversation is gone, and a successor does not
    // inherit it. Death and removal are `abandon_deferred_for`'s, because a killed weave keeps
    // its id and incarnation. This is also how a leaked capability is reclaimed: it costs one
    // slot until its owner's code is replaced or its life ends.
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
                           authority_.correlation,
                           authority_.preparation}; // likewise: the ask, not a number
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
    // Every term is a different attack: the record exists, unspent and unreleased; the speaker
    // is the bound respondent (not a successor, the role's current holder or a copier of public
    // values); the respondent is still the same incarnation; and the requester still exists at
    // the incarnation that asked, so a successor at the same id is never answered.
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
    const TxnId preparation = rec->preparation;
    *rec = DeferredRecord{};

    // THE SAME DOOR THE IMMEDIATE ANSWER LEAVES BY, carrying the requester facts
    // the RECORD kept — the ones from when the ask was delivered, never today's.
    return enqueue_answer(to, as_sender, std::move(msg), correlation, requester_life,
                          requester_incarnation, preparation);
}

void Switchboard::release_deferred_as(WeaveId as_sender, const DeferredAnswer& answer) {
    // Abandonment is silent to the requester: there is no cancellation vocabulary. It is not
    // silent to the bus: the slot is reclaimed at once.
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
    // Authority is relative to the Loom that issued it. Any weave may stand up a decoy board of
    // its own and mint a genuine authority from it, so the check is the issuer: `issued_here`
    // fails for another board's authority and for one whose board was destroyed. It lives here
    // because holding an authority gives a consumer no way to ask.
    // LIFE-04; docs/laws/lifecycle-laws.md
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

// ---- live authority administration (GATE-05) -------------------------------

GrantChange Switchboard::delegate_authority_as(WeaveId caller, const GrantAuthority& authority,
                                               LiveAuthority requested) {
    GrantChange change;
    change.subject = authority.subject();
    // `caller` is who acted, for a Weaver's diagnostics. It is deliberately not
    // consulted below: authority here is possession of the capability, and adding
    // "...and you must also be somebody" would be a second rule that a magic name
    // could one day satisfy.
    (void)caller;

    // INERT FIRST. A default-constructed capability names no subject at all, which
    // is a different fact from naming one this board never had — and an
    // administrator holding one as an uninitialized member deserves to be told
    // which mistake it made.
    if (!authority.valid()) {
        change.outcome = GrantOutcome::NoAuthority;
        return change;
    }
    // THEN THE BOARD. Every Loom is its own authority domain: a capability minted
    // from a decoy Switchboard is entirely genuine and has no standing here, and
    // one whose board has been destroyed has no standing anywhere. Checked before
    // the registry is touched, so a foreign capability learns nothing about which
    // ids this board has.
    if (!issued_here(authority)) {
        change.outcome = GrantOutcome::ForeignBoard;
        return change;
    }
    WeaveRecord* subject = find(authority.subject());
    if (subject == nullptr) {
        // The subject is gone. A WeaveId is never reused, so this capability can
        // never come to govern anything again — it fails safe permanently rather
        // than waiting to be inherited by whoever mounts next.
        change.outcome = GrantOutcome::NoSuchSubject;
        return change;
    }
    change.previous = subject->delegated;
    change.installed = subject->delegated;

    // THE CEILING. The security-critical line of this file: a holder may install
    // any semantic subset of what the host named, and nothing else — so a narrow
    // Weaver cannot mint itself a root session, and an empty request (revocation)
    // is always within any ceiling.
    if (!authority.ceiling().contains(requested)) {
        change.outcome = GrantOutcome::ExceedsCeiling;
        return change;
    }

    // One state transition: grant, revoke, widen and narrow are all a replacement, so no
    // subject holds the old rules and the new, or neither, and nothing between the assignments
    // below dispatches or runs weave code. The claim moves before the old one is released, so
    // a shape both authorities name never falls to zero claims for one assignment.
    SchemaClaimScope next;
    registry_.claim_known(next, named_send_shapes(requested));
    subject->delegated = std::move(requested);
    subject->delegated_schemas = std::move(next);

    change.installed = subject->delegated;
    change.outcome = GrantOutcome::Installed;
    return change;
}

AuthorityView Switchboard::describe_authority_as(WeaveId caller,
                                                 const GrantAuthority& authority) const {
    (void)caller; // as above: the capability is the authority, not the caller
    AuthorityView view;
    if (!authority.valid() || !issued_here(authority)) {
        return view; // unavailable, and saying nothing about this board's contents
    }
    const WeaveRecord* subject = find(authority.subject());
    if (subject == nullptr) {
        return view;
    }
    view.available = true;
    view.subject = authority.subject();
    // SNAPSHOTS OF THE VERY VALUES THE BUS WILL READ, not a summary derived
    // alongside them. `AuthorityView::permits*` then calls the same
    // `effective_*` predicates `deliver_one` calls, so an administrator's picture
    // of what a subject may do cannot drift from what the subject may do.
    view.base = subject->grant.live();
    view.delegated = subject->delegated;
    return view;
}

std::size_t Switchboard::fanout(Message msg, bool gated, Provenance provenance) {
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
        const std::uint64_t seq = allocate_sequence();
        journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
        // Rebuilt field by field, so a published Message carries no provenance whatever the
        // caller's copy held. Every recipient's envelope is stamped, so each delivery asks
        // about a dead author for itself; an office-authored publication stamps the same
        // verified fact on every recipient's envelope.
        Envelope env{Message(msg.payload, msg.sender, msg.reply_to, msg.correlation), rec.id,
                     seq, gated, std::string{}, sender_life};
        env.msg.provenance = provenance;
        // Every recipient's envelope carries the same dispatch parent: one authorship moment
        // produced them all.
        env.dispatch_parent = current_dispatch_seq_;
        env.fence = current_dispatch_fence_; // ...and every one counts in that delivery's fence
        queue_back(std::move(env));
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

// ---- deliberate office authorship (MSG-07) ----------------------------------
//
// The authorization moment is authorship, never delivery. Each door asks whether this exact
// sender holds that role now, as it asks to speak as it; on yes it stamps the fact into the
// envelope's provenance, on no it refuses visibly. From then on the fact is history:
// deliver_one carries it untouched, and every other delivery law still runs.

bool Switchboard::holds_role_now(WeaveId as_sender, std::string_view as_role) const {
    if (!as_sender.valid() || as_role.empty()) {
        return false; // no identity holds no office; the empty name is no office
    }
    const auto it = roles_.find(std::string(as_role));
    return it != roles_.end() && it->second == as_sender;
}

Ticket Switchboard::refuse_office(WeaveId target, WeaveId as_sender, const Message& msg) {
    // Visible on the tap and in the journal with the precise reason — and the
    // caller receives the INVALID ticket, because nothing was queued. A refusal
    // ticket here would tell an office its statement had been sent; silence
    // would downgrade unauthorized office speech into a mystery. Neither is
    // this: the attempt is named, the statement went nowhere.
    (void)refuse_now(target, as_sender, msg, RefusalReason::RoleAuthorshipDenied);
    return Ticket{};
}

Ticket Switchboard::office_send_as(WeaveId as_sender, std::string_view as_role, WeaveId target,
                                   Message msg) {
    if (!holds_role_now(as_sender, as_role)) {
        return refuse_office(target, as_sender, msg);
    }
    msg.sender = as_sender;
    return enqueue_directed(target, std::move(msg), /*gated=*/true,
                            Provenance{}.with_authored_role(std::string(as_role)));
}

Ticket Switchboard::office_send_to_role_as(WeaveId as_sender, std::string_view as_role,
                                           std::string_view to_role, Message msg) {
    if (!holds_role_now(as_sender, as_role)) {
        return refuse_office(WeaveId{}, as_sender, msg);
    }
    // TWO ROLES, TWO FACTS, TWO FIELDS: the authored office rides the
    // provenance; the destination rides the envelope's role slot and is
    // resolved at delivery exactly as an ordinary send_to_role. Nothing
    // downstream can mistake one for the other because they never share a
    // representation.
    msg.sender = as_sender;
    return enqueue_role(std::string(to_role), std::move(msg), /*gated=*/true,
                        Provenance{}.with_authored_role(std::string(as_role)));
}

OfficePublication Switchboard::office_publish_as(WeaveId as_sender, std::string_view as_role,
                                                 Message msg) {
    if (!holds_role_now(as_sender, as_role)) {
        (void)refuse_office(WeaveId{}, as_sender, msg);
        return OfficePublication{}; // refused: not authored, and 0 means nothing
    }
    // No sealed-speech check is needed before the fanout, and not because it
    // was forgotten: a sealed weave holds no role — seal_weave refuses role
    // holders, and admission unseals before it binds — so a candidate already
    // failed the membership question above, with the precise reason.
    msg.sender = as_sender;
    const std::size_t recipients =
        fanout(std::move(msg), /*gated=*/true,
               Provenance{}.with_authored_role(std::string(as_role)));
    return OfficePublication{true, recipients};
}

Ticket Switchboard::office_send(std::string_view as_role, WeaveId target, Message msg) {
    // The root surface: a host program speaking with no weave identity. It
    // holds no office by definition, and the refusal says so on the tap rather
    // than silently returning the base default — "a root tried to author
    // office speech" is exactly the kind of thing an operator wants to see.
    (void)as_role;
    return refuse_office(target, WeaveId{}, msg);
}

Ticket Switchboard::office_send_to_role(std::string_view as_role, std::string_view to_role,
                                        Message msg) {
    (void)as_role;
    (void)to_role;
    return refuse_office(WeaveId{}, WeaveId{}, msg);
}

OfficePublication Switchboard::office_publish(std::string_view as_role, Message msg) {
    (void)as_role;
    (void)refuse_office(WeaveId{}, WeaveId{}, msg);
    return OfficePublication{};
}

void Switchboard::record(std::uint64_t seq, Disposition disposition, const Refusal& refusal) {
    JournalSlot& slot = journal_[seq % kJournalCapacity];
    if (slot.seq == seq) {
        // Still the slot's owner. If a wrap past kJournalCapacity already evicted this
        // seq (only possible when a single turn outruns the window), the guard leaves
        // the newer owner untouched and this outcome is simply forgotten — never
        // misattributed. Read-immediately consumers never reach that depth.
        slot.outcome = DeliveryOutcome{disposition, refusal};
    }
}

bool Switchboard::observer_registered(ObserverId id) const noexcept {
    for (const auto& observer : observers_) {
        if (observer.first == id) {
            return true;
        }
    }
    return false;
}

void Switchboard::emit(const BusEvent& event) {
    // Each event has its own view of the tap list (MSG-11). An observer may subscribe or
    // unsubscribe from inside a notification (the console and the bridge unsubscribe from
    // destructors), and walking the live vector then reads freed memory or skips the observer
    // that shifted into a vacated slot. The view is taken at entry, so every emission,
    // including one an observer causes, decides its recipients once.
    const std::vector<std::pair<ObserverId, std::shared_ptr<Observer>>> view = observers_;
    for (const auto& observer : view) {
        // A removal takes effect within the event it is made in: the console and the bridge
        // call `remove_observer` to stop a callback before the members it captured die, and
        // honouring the snapshot would make that a use-after-free. An addition waits, so a
        // subscriber added during an event does not hear it.
        if (!observer_registered(observer.first)) {
            continue;
        }
        // The strong reference is what makes self-removal safe: `remove_observer`
        // may drop the registration mid-call, and the callable still outlives its
        // own body.
        const std::shared_ptr<Observer> callback = observer.second;
        if (callback != nullptr && *callback) {
            (*callback)(event);
        }
    }
}

void Switchboard::deliver_one(Envelope env) {
    // WHAT THIS DISPATCH QUEUES IS COUNTED IN THIS ENVELOPE'S FENCE -- its handler's sends, and
    // the refusals, notices and showings its dispatch produces -- and the envelope itself stops
    // counting when this returns, by any exit.
    const FenceTurn fenced(*this, env.fence);
    // An admission is its own delivery (PR-08). It takes the whole turn: it moves production
    // topology and hands the candidate its activation, outside the ordinary authorization
    // path, because a committed activation is Loom's act, not the coordinator's speech, and a
    // mutable grant or sender life must not be able to unmake it.
    if (env.admission.present) {
        deliver_admission(std::move(env));
        return;
    }

    // Internal notices are disposable, incarnation-bound deliveries. They never
    // seed refusal-of-refusal traffic, and a successor cannot inherit one.
    const bool is_notice = env.msg.provenance.dispatch_refused();
    if (is_notice) {
        const WeaveRecord* recipient = find(env.target);
        if (recipient == nullptr || !recipient->alive ||
            recipient->life != env.sender_life ||
            recipient->incarnation != env.refusal_incarnation ||
            !accept_match(*recipient, DispatchRefused::zen_name, DispatchRefused::zen_version)) {
            return;
        }
    }

    BusEvent ev;
    ev.seq = env.seq;
    ev.target = env.target;
    ev.sender = env.msg.sender;
    ev.schema_name = env.msg.payload.schema().name();
    ev.schema_version = env.msg.payload.schema().version();
    // Three facts the envelope holds, set before any refusal branch so a refused delivery is
    // as legible as a delivered one: which conversation, which office was addressed, and which
    // delivery this was authored from.
    ev.correlation = env.msg.correlation;
    ev.addressed_role = env.role;
    ev.dispatch_parent = env.dispatch_parent;
    // The STAMPED authorship fact, read from the envelope — never a role_of()
    // lookup, which would report current membership instead of historical
    // authorship (MSG-07). Set before any refusal branch, so a refused
    // office-authored delivery still shows which office it was authored as.
    ev.authored_role = std::string(env.msg.provenance.authored_role());

    const WeaveRecord* author = env.gated ? find(env.msg.sender) : nullptr;
    const bool permitted = !env.gated ||
        (author != nullptr &&
         (env.role.empty()
              ? effective_permits(author->grant.live(), author->delegated, ev.schema_name,
                                  ev.schema_version, env.target)
              : effective_permits_role(author->grant.live(), author->delegated, ev.schema_name,
                                       ev.schema_version, env.role)));
    const auto refuse = [&](const Refusal& r) {
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        // Queue before calling arbitrary observers: neither their exceptions nor
        // their topology mutations change the already-made dispatch decision.
        try {
            notify_dispatch_refusal(env, ev, permitted);
        } catch (const std::bad_alloc&) {
            // Best-effort metadata allocation must not erase the original host evidence.
        } catch (const std::overflow_error&) {
            // No sequence can be reused to make a notice look like a newer attempt.
        }
        emit(ev);
    };

    // Capability authorization, for gated (weave-originated) messages only, before role
    // resolution and the gate, so a denied message reaches neither and an unauthorized sender
    // cannot learn whether a role is held. It asks "may you send this", not the gate's
    // conformance question; a role-targeted send is authorized by role, a direct one by WeaveId.
    if (env.gated) {
        const WeaveRecord* sender = find(env.msg.sender);
        // A weave-originated message belongs to the life that authored it (MSG-03). Ending a
        // dying participant's conversations (ANS-04) cannot reach a message it merely queued,
        // which would otherwise be delivered as speech from whatever now answers to the id.
        // Checked first, before the grant and role resolution, so a stale message reaches no
        // handler, no answer authority and not even whether a role is held.
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
            refuse(r);
            return;
        }
        // ---- the candidate boundary (PR-01) --------------------------------
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
                refuse(r);
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
            refuse(r);
            return;
        }
        // Effective authority at the moment of delivery (GATE-05): baseline union delegated,
        // read off the record the router just found, never captured when the message was
        // queued. That is live revocation: a message queued while the sender held a delegated
        // rule and delivered after the rule was taken back is refused here.
        if (!permitted) {
            const Refusal r{RefusalReason::CapabilityDenied, {}};
            refuse(r);
            return;
        }
    }

    // Resolve a role target to its current holder. An unheld role degrades exactly like an
    // unknown WeaveId -- NoSuchTarget, never the gate -- so an unmounted broker is
    // "unavailable", not a hole.
    if (!env.role.empty()) {
        auto it = roles_.find(env.role);
        if (it == roles_.end()) {
            const Refusal r{RefusalReason::NoSuchTarget, {}};
            refuse(r);
            return;
        }
        env.target = it->second;
        ev.target = env.target;
    }

    WeaveRecord* rec = find(env.target);
    if (rec == nullptr) {
        const Refusal r{RefusalReason::NoSuchTarget, {}};
        refuse(r);
        return;
    }
    if (!rec->alive) {
        const Refusal r{RefusalReason::TargetUnavailable, {}};
        refuse(r);
        return;
    }

    // An authenticated answer belongs to the life and incarnation that asked (ANS-03): the
    // requester's half of what MSG-03 does for the author. An ordinary send reaches whoever
    // occupies its destination; an answer names one conversation between two exact
    // participants, so the expectation rides only on envelopes that left by an answer door.
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
            refuse(r);
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
        refuse(r);
        return;
    }

    // The one gate, live path. admit() consumes the candidate and re-emits it
    // trusted on success; on failure the candidate is dropped and never seen.
    const Schema& door_schema = door != nullptr ? **door : *wildcard_door;
    Admission a = loom::admit(std::move(env.msg.payload), door_schema);
    if (!a.ok()) {
        const Refusal r{RefusalReason::GateRefused, a.first_error()};
        refuse(r);
        return;
    }

    Message trusted(std::move(a).value(), env.msg.sender, env.msg.reply_to, env.msg.correlation);
    trusted.provenance = env.msg.provenance; // Loom's own word, set at enqueue and only there
    // Shown before it runs, or not run (SENSE-06). A weave behind a value the bus published
    // under its key is shown it here, before its handler. A showing that does not complete, or
    // a weave already held, refuses this delivery: `ApplicationFailed` in the journal and on
    // the tap, `zen.DispatchRefused` to a sender that accepts it, and a native hook's exception
    // rethrown afterwards (MSG-10). Outside the delivery scope: a showing has no Mail.
    {
        const Showing shown = observe_published_claims(*rec);
        if (shown.failed) {
            const Refusal r{RefusalReason::ApplicationFailed, {}};
            refuse(r);
            if (shown.error) {
                std::rethrow_exception(shown.error);
            }
            return;
        }
    }
    // WHETHER THE HANDLER COMPLETES IS A FACT ABOUT THIS DELIVERY, so the bit is
    // cleared here rather than trusted to have been cleared by the last one.
    handler_reported_failure_ = false;
    std::exception_ptr failure;
    {
        // THE AMBIENT DELIVERY CONTEXT LIVES IN THIS BLOCK AND NOWHERE ELSE (MSG-10).
        // The guard is what makes "this stack frame" true on the path where the
        // handler does not return one — a throw leaving an answerable delivery behind
        // is a standing right to speak into a conversation that is over.
        const DeliveryScope delivering(*this);
        // THE REPLY AUTHORITY IS THIS STACK FRAME. It is created after routing has
        // chosen the recipient — so it names the incarnation that ACTUALLY received
        // the request, not the one the sender guessed or the one the role names now —
        // and it dies when the handler returns. A role changing hands after this
        // point hands the new holder nothing: it never received this request.
        current_target_ = env.target;
        // ...and which delivery that is, so a message this handler authors is stamped with
        // the delivery it was authored from; the seq is exactly as scoped as the target.
        current_dispatch_seq_ = env.seq;
        current_dispatch_parent_ = env.dispatch_parent;
        // ...AND IT REMEMBERS WHO ASKED, not merely where to send (ANS-03). Captured
        // HERE, at the delivery that earns the authority, so that an answer produced
        // later — this handler's, or a deferred one spent minutes from now — is bound
        // to the requester that actually asked rather than to whatever occupies that
        // id when the answer is finally written.
        const WeaveRecord* asker = find(env.msg.sender);
        // An ask seeds an answerable conversation; its answer does not seed another. Seeded
        // from every envelope, an answer-to-an-answer would inherit the ask's identity (and
        // `enqueue_answer` copies the correlation forward), so a later exchange could satisfy
        // "this delivery answers that ask" when it does not.
        const TxnId answerable = env.answer_target.present ? TxnId{} : env.preparation;
        if (!is_notice) {
            authority_ = ReplyAuthority{env.msg.sender,
                                        env.msg.correlation,
                                        /*spent=*/false,
                                        trusted.payload.schema_ptr(),
                                        asker == nullptr ? 0 : asker->life,
                                        asker == nullptr ? 0 : asker->incarnation,
                                        answerable};
        }
        // ...AND WHAT THIS DELIVERY IS, for a handler that must prove to the bus what
        // it just heard (PR-04). Every field comes from the envelope Loom built:
        // the provenance no ordinary enqueue can write, the sender stamp no weave can
        // choose, and the correlation an answer door copied from the ask. A handler
        // holding a Switchboard& can therefore say "this delivery is my readiness
        // answer" and be *checked*, rather than believed.
        delivery_ = DeliveryFacts{trusted.provenance.answers_ask(), env.msg.sender,
                                  env.msg.correlation, env.preparation};
        // The handler receives a WeaveBus bound to its own id — never the concrete
        // Switchboard — so anything it sends is stamped with its identity and gated
        // against its grant.
        WeaveBus weave_bus(*this, env.target);
        // THE EXCEPTION IS CAUGHT AND NOT SWALLOWED (MSG-10). It is held, so that
        // this delivery's abnormal exit becomes an observable FACT before the
        // stack keeps unwinding, and it is rethrown below unexamined and
        // untranslated. What is bought is the difference between a history that
        // says "the handler did not finish" and one that says nothing at all.
        const std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();
        try {
            rec->weave->handle(trusted, weave_bus); // may enqueue further deliveries
        } catch (...) {
            failure = std::current_exception();
        }
        ev.handler_elapsed_ns = elapsed_ns_since(started);
    } // ...and the authority does not outlive the handler, by ANY exit path.
    // Cleared before the journal and the tap: an observer of a Delivered event is not inside
    // the delivery and must not find one live.
    if (failure || handler_reported_failure_) {
        // NO JOURNAL OUTCOME, on either seam. "Delivered" would claim the handler
        // ran to completion and "Refused" would claim Loom declined it; both are
        // false and the slot's `Pending` already means the true thing — no
        // outcome was recorded (MSG-10). The tap gets the fact instead, because
        // an observer is the only thing that can carry it anywhere.
        ev.kind = EventKind::HandlerFailed;
        ev.payload = &trusted.payload;
        emit(ev);
        if (failure) {
            // ...and out to the host. An observer that throws from the emission above
            // replaces this exception with its own: ordinary C++ propagation, the trade
            // MSG-10 makes for an observer that throws on any event.
            std::rethrow_exception(failure);
        }
        return;
    }
    record(env.seq, Disposition::Delivered, Refusal{});
    ev.kind = EventKind::Delivered;
    ev.payload = &trusted.payload;
    emit(ev);
}

void Switchboard::drain_until_idle() {
    if (in_dispatch_) {
        return; // non-reentrant: a handler's sends were enqueued, not nested
    }
    // Scoped, because a native handler that throws unwinds straight past this
    // line — and a dispatch flag left standing makes every later turn believe
    // itself reentrant and return without delivering anything (MSG-10).
    const DispatchGuard dispatching(*this);
    stop_requested_ = false;
    while (!queue_.empty() && !stop_requested_) {
        Envelope env = std::move(queue_.front());
        queue_.pop_front();
        deliver_one(std::move(env));
    }
}

std::size_t Switchboard::pump_pending() {
    // The bound is a fact about the queue, taken once before anything runs: a handler's own
    // continuation queues behind it and is not part of this turn, so a self-re-arming producer
    // cannot hold the turn open. drain_until_idle() counts that continuation as its own work,
    // and so never finishes while the producer lives.
    return dispatch_at_most(queue_.size());
}

std::size_t Switchboard::dispatch_at_most(std::size_t budget) {
    if (in_dispatch_) {
        return 0; // non-reentrant, exactly as drain_until_idle() is
    }
    const DispatchGuard dispatching(*this); // unpoisoned by a throw, as drain_until_idle() is
    stop_requested_ = false;
    std::size_t dispatched = 0;
    // `dispatched < budget` is checked against deliveries ACTUALLY MADE, so an
    // envelope a handler enqueues during this loop is as bounded as one that was
    // already waiting. That is the whole point: the producer this exists to
    // contain is precisely the one that re-arms itself from inside its handler.
    while (dispatched < budget && !queue_.empty() && !stop_requested_) {
        Envelope env = std::move(queue_.front());
        queue_.pop_front();
        deliver_one(std::move(env));
        ++dispatched;
    }
    return dispatched;
}

// ---- fences: when what one send set in motion has been dispatched --------------------------

const char* name_of(FenceState s) noexcept {
    switch (s) {
    case FenceState::Unknown:
        return "Unknown";
    case FenceState::Open:
        return "Open";
    case FenceState::Settled:
        return "Settled";
    }
    return "?";
}

Switchboard::FenceRecord* Switchboard::find_fence(std::uint64_t id) noexcept {
    if (id == 0) {
        return nullptr;
    }
    for (FenceRecord& f : fences_) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

const Switchboard::FenceRecord* Switchboard::find_fence(std::uint64_t id) const noexcept {
    if (id == 0) {
        return nullptr;
    }
    for (const FenceRecord& f : fences_) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

void Switchboard::queue_back(Envelope env) {
    if (FenceRecord* f = find_fence(env.fence)) {
        ++f->queued;
    }
    queue_.push_back(std::move(env));
}

void Switchboard::fence_dispatched(std::uint64_t id) noexcept {
    if (FenceRecord* f = find_fence(id)) {
        if (f->queued > 0) {
            --f->queued;
        }
    }
}

std::uint64_t Switchboard::begin_fence() {
    if (fences_.size() >= kMaxFences) {
        return 0;
    }
    const std::uint64_t id = next_fence_++;
    fences_.push_back(FenceRecord{id, 0});
    return id;
}

namespace {
/// The one enqueue a fenced host send makes is counted in the fence it opened, and in no other:
/// the ambient fence is that fence for exactly the length of the call, and restored after it by
/// any exit.
struct AmbientFence {
    std::uint64_t& slot;
    std::uint64_t was;
    AmbientFence(std::uint64_t& s, std::uint64_t fence) noexcept : slot(s), was(s) { slot = fence; }
    ~AmbientFence() { slot = was; }
    AmbientFence(const AmbientFence&) = delete;
    AmbientFence& operator=(const AmbientFence&) = delete;
};
} // namespace

Ticket Switchboard::send_as_fenced(WeaveId as_sender, WeaveId target, Message msg, Fence* fence) {
    if (fence == nullptr) {
        return Ticket{}; // no owner could retain and release the fence
    }
    *fence = Fence{};
    const std::uint64_t id = begin_fence();
    if (id == 0) {
        return Ticket{}; // at the bound: nothing is queued, and the caller is told so
    }
    Ticket t;
    {
        const AmbientFence ambient(current_dispatch_fence_, id);
        t = send_as(as_sender, target, std::move(msg));
    }
    if (!t.valid()) {
        release_fence(Fence{id});
        return t;
    }
    if (fence != nullptr) {
        *fence = Fence{id};
    }
    return t;
}

Ticket Switchboard::send_as_to_role_fenced(WeaveId as_sender, std::string_view role, Message msg,
                                           Fence* fence) {
    if (fence == nullptr) {
        return Ticket{}; // no owner could retain and release the fence
    }
    *fence = Fence{};
    const std::uint64_t id = begin_fence();
    if (id == 0) {
        return Ticket{};
    }
    Ticket t;
    {
        const AmbientFence ambient(current_dispatch_fence_, id);
        t = send_as_to_role(as_sender, role, std::move(msg));
    }
    if (!t.valid()) {
        release_fence(Fence{id});
        return t;
    }
    if (fence != nullptr) {
        *fence = Fence{id};
    }
    return t;
}

FenceState Switchboard::fence_state(Fence fence) const noexcept {
    const FenceRecord* f = find_fence(fence.value);
    if (f == nullptr) {
        return FenceState::Unknown;
    }
    return f->queued == 0 ? FenceState::Settled : FenceState::Open;
}

void Switchboard::release_fence(Fence fence) noexcept {
    for (auto it = fences_.begin(); it != fences_.end(); ++it) {
        if (it->id == fence.value) {
            fences_.erase(it);
            return;
        }
    }
}

DeliveryOutcome Switchboard::outcome(Ticket t) const {
    if (t.seq == 0 || (next_seq_ != 0 && t.seq >= next_seq_)) {
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
    observers_.emplace_back(id, std::make_shared<Observer>(std::move(obs)));
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

std::string Switchboard::snapshot_bytes(WeaveId id) {
    return snapshot_bytes(id, SnapshotAccess::Ordinary);
}

std::string Switchboard::snapshot_bytes(WeaveId id, SnapshotAccess access) {
    WeaveRecord* rec = find(id);
    if (rec == nullptr) {
        throw std::invalid_argument("snapshot_bytes: no such weave");
    }
    if (access == SnapshotAccess::Diagnostic) {
        // EXPLICITLY AS IT IS: nothing shown, nothing recorded, nothing refused. The
        // one door that reads a held weave, and it says so in its name.
        return loom::serialize(rec->weave->snapshot());
    }
    // Joint publication: the bytes a reload carries must agree with what the bus
    // has already published about this weave, so it is shown any joint-published
    // claim of its own first.
    const Showing shown = observe_published_claims(*rec);
    if (shown.failed && access == SnapshotAccess::Ordinary) {
        // NOT SERVED AS A NORMAL SNAPSHOT of a state the weave no longer stands
        // behind. The native hook's own exception is re-raised (the record already
        // says Failed); a held weave, or a loaded one whose status said no, is
        // refused in the bus's words.
        if (shown.error) {
            std::rethrow_exception(shown.error);
        }
        throw ApplicationFailedError(
            "snapshot_bytes: weave " + std::to_string(id.value) +
            " could not apply the value a joint operation published under its claim" +
            (shown.op != 0 ? " (operation " + std::to_string(shown.op) + ")" : std::string()) +
            "; it is held until it is reloaded or removed -- SnapshotAccess::Diagnostic reads "
            "it as it is");
    }
    // Reload: a failed showing is on the record (the successor will be shown again)
    // and the bytes are returned as they are.
    return loom::serialize(rec->weave->snapshot());
}

bool Switchboard::seal_weave(WeaveId candidate, WeaveId coordinator) {
    WeaveRecord* rec = find(candidate);
    const WeaveRecord* owner = find(coordinator);
    // A dead coordinator cannot own a preparation: it cannot converse, so the candidate would
    // be sealed to a correspondent that never answers. A sealed candidate is not resealable:
    // changing owners would hand a prepared candidate to another transaction, and there is no
    // transfer.
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

std::string Switchboard::role_of(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec == nullptr ? std::string{} : rec->role;
}

bool Switchboard::sealed(WeaveId id) const {
    const WeaveRecord* rec = find(id);
    return rec != nullptr && rec->sealed_by.valid();
}


bool Switchboard::commit_candidate(WeaveId candidate, WeaveId incumbent,
                                   const std::string& role) {
    // Every precondition first, so a refusal changes nothing: with nothing to undo, a commit
    // is never half-applied.
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

    // ...and then the whole change, with no delivery between any two lines of it. No lock is
    // needed: dispatch is single-threaded and non-reentrant, so an observer's next delivery
    // precedes all of this or follows all of it. The same change as several ordinary messages
    // would not be atomic; that is the observable window a `zen.SwapWeave` has.
    cand->sealed_by = CandidateOwner{};
    inc->role.clear();
    cand->role = role;
    held->second = candidate;
    return true;
}

AdmitRefusal Switchboard::admission_blocked(const ParticipantRef& candidate,
                                            const ParticipantRef& incumbent,
                                            const CandidateOwner& owner,
                                            const std::string& role) const {
    // ONE FUNCTION, ASKED TWICE (PR-03). Scheduling an admission and dispatching
    // it must require exactly the same world, and the cheapest guarantee of that
    // is that there is only one place the question is written down. Every
    // participant is checked as an exact life and incarnation, so a queued
    // admission cannot land on a successor at the same address.
    const WeaveRecord* cand = find(candidate.who);
    if (cand == nullptr || !cand->alive || !cand->sealed_by.valid() ||
        cand->life != candidate.life || cand->incarnation != candidate.incarnation) {
        return AdmitRefusal::NotACandidate;
    }
    const WeaveRecord* inc = find(incumbent.who);
    if (inc == nullptr || !inc->alive || inc->sealed_by.valid() || inc->life != incumbent.life ||
        inc->incarnation != incumbent.incarnation) {
        return AdmitRefusal::IncumbentUnfit;
    }

    // The owner must still be the owner (PR-03). A preparation belongs to a life: a host caller
    // with a good lifecycle authority must not admit a candidate whose coordinator died and
    // revived, was reloaded or was removed. Two halves: the seal must still name this exact
    // owner (it could have been resealed), and that owner must still stand at its address.
    // docs/laws/replacement-laws.md
    if (!owns_seal(cand->sealed_by, owner.who, owner.life, owner.incarnation)) {
        return AdmitRefusal::OwnerChanged;
    }
    const WeaveRecord* owner_rec = find(owner.who);
    if (owner_rec == nullptr || !owner_rec->alive || owner_rec->life != owner.life ||
        owner_rec->incarnation != owner.incarnation) {
        return AdmitRefusal::OwnerChanged;
    }

    if (role.empty()) {
        return AdmitRefusal::RoleNotHeld;
    }
    const auto held = roles_.find(role);
    if (held == roles_.end() || !(held->second == incumbent.who)) {
        return AdmitRefusal::RoleNotHeld;
    }
    return AdmitRefusal::None;
}

std::optional<Value> Switchboard::activation_deliverable(const WeaveRecord& candidate,
                                                         Value payload) const {
    // The recipient's half of the contract, asked before anything moves: a candidate that does
    // not accept `zen.Activated`, found at delivery, is found after the role has moved. The
    // accept-set door and the gate are both asked against the exact payload to be delivered.
    // The answer is stable: the accept-set is fixed at registration (reload refuses a drifted
    // one) and `admit()` is pure, so the dispatch, which asks again, answers the same way.
    const std::string name(payload.schema().name());
    const std::uint32_t version = payload.schema().version();
    const std::shared_ptr<const Schema>* door = accept_match(candidate, name, version);
    std::shared_ptr<const Schema> wildcard_door;
    if (door == nullptr && candidate.accepts_any) {
        wildcard_door = resolve_schema(name, version);
    }
    if (door == nullptr && !wildcard_door) {
        return std::nullopt;
    }
    Admission a = loom::admit(std::move(payload), door != nullptr ? **door : *wildcard_door);
    if (!a.ok()) {
        return std::nullopt;
    }
    return std::move(a).value();
}

AdmitResult Switchboard::admit_candidate(WeaveId candidate, WeaveId incumbent,
                                         const std::string& role,
                                         const LifecycleAuthority& authority,
                                         Message activation, std::int64_t sequence) {
    // The direct host primitive IS the shared one, with no transaction to end.
    return schedule_admission(candidate, incumbent, role, authority, std::move(activation),
                              sequence, TxnId{});
}

AdmitResult Switchboard::schedule_admission(WeaveId candidate, WeaveId incumbent,
                                            const std::string& role,
                                            const LifecycleAuthority& authority,
                                            Message activation, std::int64_t sequence,
                                            TxnId txn) {
    // EVERY PRECONDITION FIRST — including the authority, so an unattested caller
    // cannot schedule a change to production topology.
    if (!issued_here(authority)) {
        return {false, AdmitRefusal::ForeignAuthority, Ticket{}};
    }
    const ParticipantRef cand_ref = participant(candidate);
    const ParticipantRef inc_ref = participant(incumbent);
    const WeaveRecord* cand = find(candidate);
    const CandidateOwner owner = cand == nullptr ? CandidateOwner{} : cand->sealed_by;
    const AdmitRefusal blocked = admission_blocked(cand_ref, inc_ref, owner, role);
    if (blocked != AdmitRefusal::None) {
        return {false, blocked, Ticket{}};
    }

    // ---- can the candidate receive its own first breath? ---------------------
    // Asked before a single field moves, against the candidate's real door and gate, so a
    // weave that cannot take the activation is refused as a candidate. The trusted result is
    // thrown away and the dispatch re-admits the pristine payload: prevalidation refuses
    // early, and never carries a pre-gated value past the one gate.
    if (!activation_deliverable(*cand, activation.payload)) {
        return {false, AdmitRefusal::CandidateContract, Ticket{}};
    }

    // ---- activation first -----------------------------------------------------
    // Role resolution happens at delivery, so a role-addressed message queued earlier would
    // reach the candidate before it is told it is alive. The activation goes just ahead of the
    // first queued envelope that could reach the candidate (addressed to the role or to it),
    // and the topology changes at that point: everything ahead still reaches the incumbent,
    // everything behind arrives after the candidate is told. Nothing is dropped or reordered.
    // PR-05; docs/laws/replacement-laws.md
    const std::uint64_t seq = allocate_sequence();
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    activation.sender = owner.who;
    activation.provenance = Provenance::attested(Provenance::Kind::Activation, sequence);
    // Ungated: a committed activation is Loom's own act, authorized by the authority checked
    // above, not the coordinator's speech, so no later question of the gated path can unmake
    // it. The sender stamp stays the coordinator's because `zen.Activated`'s lineage rule is
    // per attesting operator: it says who admitted, not who spoke.
    Envelope act{std::move(activation), candidate, seq, /*gated=*/false, std::string{},
                 /*sender_life=*/0};
    act.admission = PendingAdmission{true, cand_ref, inc_ref, owner, role, txn};
    act.dispatch_parent = current_dispatch_seq_; // the delivery this was authored from
    act.fence = current_dispatch_fence_;
    auto at = queue_.begin();
    for (; at != queue_.end(); ++at) {
        if (at->target == candidate || (!at->role.empty() && at->role == role)) {
            break;
        }
    }
    if (FenceRecord* f = find_fence(act.fence)) {
        ++f->queued; // inserted rather than appended, and counted all the same
    }
    queue_.insert(at, std::move(act));
    return {true, AdmitRefusal::None, Ticket{seq}};
}

void Switchboard::deliver_admission(Envelope env) {
    BusEvent ev;
    ev.seq = env.seq;
    ev.target = env.target;
    ev.sender = env.msg.sender;
    ev.schema_name = env.msg.payload.schema().name();
    ev.schema_version = env.msg.payload.schema().version();

    const auto refuse = [&](RefusalReason reason) {
        const Refusal r{reason, {}};
        record(env.seq, Disposition::Refused, r);
        ev.kind = EventKind::Refused;
        ev.refusal = r;
        emit(ev);
    };

    // ---- 1. is this still the admission that was scheduled? ------------------
    // A transaction-borne admission must still be the same transaction, in the state that
    // scheduled it. An abort while pending erases the record, so the queued envelope refuses:
    // a pending admission cannot be revived, and no second terminal outcome is written.
    PreparedReplacement* txn = nullptr;
    if (env.admission.txn.valid()) {
        txn = find_txn(env.admission.txn);
        if (txn == nullptr || txn->state != TxnState::AdmissionPending) {
            refuse(RefusalReason::AdmissionRevoked);
            return;
        }
    }

    // ---- 2. does it still describe the world? --------------------------------
    if (admission_blocked(env.admission.candidate, env.admission.incumbent, env.admission.owner,
                          env.admission.role) != AdmitRefusal::None) {
        if (txn != nullptr) {
            PreparedReplacement copy = *txn;
            finish_txn(copy, TxnState::Aborted, TxnReason::AdmissionRefused);
        }
        refuse(RefusalReason::AdmissionRevoked);
        return;
    }

    // ---- 3. is the activation deliverable? -----------------------------------
    // Asked again rather than assumed, and before anything moves. The accept-set is fixed and
    // the gate is pure, so the answer should not have changed; the order makes the law hold
    // regardless: an undeliverable activation refuses and the incumbent is still the service.
    WeaveRecord* cand = find(env.admission.candidate.who);
    std::optional<Value> admitted;
    if (cand != nullptr) {
        admitted = activation_deliverable(*cand, std::move(env.msg.payload));
    }
    if (!admitted) {
        if (txn != nullptr) {
            PreparedReplacement copy = *txn;
            finish_txn(copy, TxnState::Aborted, TxnReason::AdmissionRefused);
        }
        refuse(RefusalReason::AdmissionRevoked);
        return;
    }

    // ---- 4. THE TOPOLOGY CHANGE, with no delivery in between -----------------
    //
    // There is no lock and none is needed: dispatch is non-reentrant and takes
    // one envelope at a time, so an observer's next delivery either
    // precedes all of this or follows all of it — and what follows it is this
    // candidate's own activation, below, with nothing whatever between them.
    WeaveRecord* inc = find(env.admission.incumbent.who);
    const CandidateOwner owner = env.admission.owner;
    cand->sealed_by = CandidateOwner{};
    inc->role.clear();
    cand->role = env.admission.role;
    roles_.find(env.admission.role)->second = env.admission.candidate.who;
    // The incumbent is sealed FOR RETIREMENT, to the same coordinator: it stops
    // receiving production entirely — not merely role traffic — and remains
    // reachable for the private retirement conversation. Moving the role alone
    // would leave it publicly direct-addressable, which is a second live service.
    inc->sealed_by = owner;

    // ---- 5. and only now is the transaction Committed ------------------------
    // After the topology moved and the activation was proven deliverable: nothing between
    // here and the handler call below can refuse. Terminalizing before `handle()` keeps weave
    // code out of the transaction's bookkeeping, and nothing can observe the difference.
    if (txn != nullptr) {
        PreparedReplacement copy = *txn;
        finish_txn(copy, TxnState::Committed, TxnReason::None);
    }

    // ---- 6. the candidate's first breath -------------------------------------
    // It is not a question (LIFE-05): there is no reply authority. The stamped sender is the
    // operator that admitted the candidate, not a requester, so an authority naming it would
    // let the candidate answer a request nobody made. With no valid requester, `answer()`
    // refuses visibly and `defer_answer()` returns an invalid capability before the deferred
    // registry is touched. `current_target_`, the delivery facts and the attestation stand.
    Message trusted(std::move(*admitted), env.msg.sender, env.msg.reply_to, env.msg.correlation);
    trusted.provenance = env.msg.provenance; // Loom's own word, set at enqueue and only there
    // SHOWN BEFORE ITS FIRST BREATH, OR NOT BREATHED -- see deliver_one. A candidate is
    // sealed and offers nothing, so a pending publication here is unreachable by the
    // mechanism; the discipline is kept in one shape at both doors regardless. The
    // topology has already moved (PR-08); what is refused is the activation's
    // delivery, said as such.
    {
        const Showing shown = observe_published_claims(*cand);
        if (shown.failed) {
            refuse(RefusalReason::ApplicationFailed);
            if (shown.error) {
                std::rethrow_exception(shown.error);
            }
            return;
        }
    }
    handler_reported_failure_ = false;
    std::exception_ptr failure;
    {
        // Scoped exactly as the ordinary path is (MSG-10): a candidate's very
        // first breath is still native code, and it may still throw. The topology
        // has already moved and the transaction has already committed, both
        // deliberately — what must not also happen is the bus being left inside a
        // delivery that ended.
        const DeliveryScope delivering(*this);
        current_target_ = env.target;
        current_dispatch_seq_ = env.seq;
        current_dispatch_parent_ = env.dispatch_parent;
        authority_ = ReplyAuthority{};
        delivery_ = DeliveryFacts{trusted.provenance.answers_ask(), env.msg.sender,
                                  env.msg.correlation, TxnId{}};
        WeaveBus weave_bus(*this, env.target);
        const std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();
        try {
            cand->weave->handle(trusted, weave_bus);
        } catch (...) {
            failure = std::current_exception();
        }
        ev.handler_elapsed_ns = elapsed_ns_since(started);
    }
    if (failure || handler_reported_failure_) {
        // A first breath that did not finish is still a first breath that
        // happened: the topology has moved and the transaction has committed,
        // both already, and none of that is unmade by saying so (MSG-10).
        ev.kind = EventKind::HandlerFailed;
        ev.payload = &trusted.payload;
        emit(ev);
        if (failure) {
            std::rethrow_exception(failure);
        }
        return;
    }
    record(env.seq, Disposition::Delivered, Refusal{});
    ev.kind = EventKind::Delivered;
    ev.payload = &trusted.payload;
    emit(ev);
}

// ---- Prepared replacement (PR-02) ------------------------------------------

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
    // Ordering is the whole correctness argument (PR-06). Cleanup discards the candidate,
    // which re-enters `invalidate_transactions_for`; were this transaction still registered,
    // the hook would end it a second time. So the record leaves the active registry first and
    // the re-entrant hook finds nothing: non-reentrancy by structure, not by a flag. `txn` is
    // a caller-owned copy, so its facts stay valid after the entry is erased.
    // docs/laws/replacement-laws.md
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
    //    (MSG-03). Only ever a SEALED weave: an admitted candidate is a live
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
    // Selective: only transactions that bind `changed`, and only when the fact they captured
    // no longer holds. Finishing a transaction can unregister its candidate, which calls back
    // into this function, so the loop rescans from the start after each finish.
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
    // Joint publication: the same transition ends every joint operation that
    // bound `changed` as operator or claimant. Every lifecycle path — removal,
    // death, revival, reload, swap — already comes through here.
    invalidate_joint_for(changed);
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
    // One incumbent, one replacement, and one candidate, one replacement: otherwise a sealed
    // candidate could be promised to two incumbents, one readiness would answer both, and
    // aborting one would unregister the candidate under the other. Refused before a slot is
    // consumed; the two causes are named apart because they send an operator to different places.
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

// ---- the preparation conversation (PR-04) -----------------------------------
//
//     A transaction becomes ready only when the exact sealed candidate
//     authentically answers the exact preparation request that belongs to that
//     transaction.

TxnReason Switchboard::vanished_transaction_reason(TxnId id) const {
    // An id below the next one is an id this Loom MINTED, so the transaction it
    // named existed and is over; anything else was never a transaction here. The
    // counter answers that without consulting — still less resurrecting — any
    // terminal record, which is deliberate: a terminal result belongs to its
    // operator and is not a lookup table for latecomers.
    return id.valid() && id.value < next_txn_id_ ? TxnReason::LateReadiness
                                                 : TxnReason::NoSuchTransaction;
}

TxnResult Switchboard::ask_candidate_to_prepare(TxnId id, Message ask) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        return {false, id, vanished_transaction_reason(id)};
    }
    if (t->state != TxnState::Preparing) {
        return {false, id, TxnReason::WrongState};
    }
    if (t->conversation != Conversation::NotAsked) {
        // ONE CONVERSATION. A second ask would leave the candidate holding an
        // answer right for a correlation the transaction has stopped expecting,
        // and would make "which of my asks is this answering?" a question the
        // design has to have an opinion about. It does: there is only one.
        return {false, id, TxnReason::PreparationAlreadyAsked};
    }
    // The world must still be the one the transaction bound — checked BEFORE the
    // ask is queued, so a preparation never begins under a coordinator that can no
    // longer own it or against a candidate that is no longer sealed to it.
    if (!still(t->op) || !still(t->coordinator) || !still(t->incumbent) ||
        !still(t->candidate)) {
        return {false, id, TxnReason::PreconditionFailed};
    }
    const WeaveRecord* cand = find(t->candidate.who);
    if (cand == nullptr || !owns_seal(cand->sealed_by, t->coordinator.who, t->coordinator.life,
                                      t->coordinator.incarnation)) {
        return {false, id, TxnReason::CandidateChanged};
    }

    // The ask is the coordinator's speech, sent as its own would be: stamped with its id,
    // gated against its grant, and through the seal only because the sender is the owner. It
    // adds a fact the transaction will recognise, not a right. The correlation is Loom's: the
    // caller's is written over, as `enqueue_answer` writes over an answerer's.
    const std::uint64_t correlation = next_preparation_correlation_++;
    ask.sender = t->coordinator.who;
    ask.reply_to = WeaveId{};
    ask.correlation = correlation;
    (void)enqueue_directed(t->candidate.who, std::move(ask), /*gated=*/true, Provenance{}, id);

    t->preparation_correlation = correlation;
    t->conversation = Conversation::Open;
    return {true, id, TxnReason::None};
}

TxnResult Switchboard::accept_authenticated_readiness(PreparedReplacement& txn) {
    // WHAT THE *WORLD* MUST STILL BE. The caller has already established who
    // spoke; these are the facts about everyone else, re-read from the registry
    // rather than from anything the transaction remembers being told.
    if (!still(txn.op)) {
        return {false, txn.id, TxnReason::OperatorChanged};
    }
    if (!still(txn.coordinator)) {
        return {false, txn.id, TxnReason::CoordinatorChanged};
    }
    if (!still(txn.incumbent)) {
        return {false, txn.id, TxnReason::IncumbentChanged};
    }
    if (!still(txn.candidate)) {
        return {false, txn.id, TxnReason::CandidateChanged};
    }
    const WeaveRecord* cand = find(txn.candidate.who);
    if (cand == nullptr || !owns_seal(cand->sealed_by, txn.coordinator.who,
                                      txn.coordinator.life, txn.coordinator.incarnation)) {
        // A candidate that is no longer sealed to this exact coordinator is not
        // this transaction's candidate, whatever it just said.
        return {false, txn.id, TxnReason::CandidateChanged};
    }
    const auto held = roles_.find(txn.role);
    if (held == roles_.end() || !(held->second == txn.incumbent.who)) {
        // Readiness is about the successor; the role moving is about the world.
        // Becoming Ready over a role that has already drifted would promise a
        // commit that could only refuse.
        return {false, txn.id, TxnReason::RoleChanged};
    }
    txn.state = TxnState::Ready;
    return {true, txn.id, TxnReason::None};
}

TxnResult Switchboard::accept_preparation_answer(TxnId id, PreparationAnswer answer) {
    PreparedReplacement* t = find_txn(id);
    if (t == nullptr) {
        // The payload named a transaction that is over, or one that never was.
        // Neither revives anything, and neither produces a second terminal result.
        return {false, id, vanished_transaction_reason(id)};
    }
    if (t->state != TxnState::Preparing) {
        return {false, id, TxnReason::WrongState};
    }

    // ---- whose voice is this? ------------------------------------------------
    // Every term is read from the delivery being dispatched, never from an argument: the
    // conversation is open; this delivery is that ask's answer (a bus-private fact, unlike a
    // correlation); Loom attests it as an answer; the caller is `current_target_`; the speaker
    // is the candidate; the correlation matches (redundant, and kept so no wall stands alone).
    // A failure refuses the command only: the transaction a forger named is left as it was.
    if (t->conversation != Conversation::Open || !delivery_.answers_ask ||
        !(delivery_.preparation == t->id) || !current_target_.valid() ||
        !(current_target_ == t->coordinator.who) || !(delivery_.sender == t->candidate.who) ||
        delivery_.correlation != t->preparation_correlation) {
        return {false, id, TxnReason::InvalidReadiness};
    }

    // The conversation is spent either way. An authentic answer is heard once,
    // and "it said no" consumes it exactly as "it said yes" does.
    t->conversation = Conversation::Consumed;

    if (answer == PreparationAnswer::Refused) {
        // THE CANDIDATE'S OWN VERDICT, and the ordinary ending law applies
        // unchanged: exactly one terminal outcome, the slot returned, the sealed
        // candidate discarded, and an incumbent that never learned any of it
        // happened.
        PreparedReplacement copy = *t;
        finish_txn(copy, TxnState::Aborted, TxnReason::CandidateRefused);
        return {true, id, TxnReason::CandidateRefused};
    }
    return accept_authenticated_readiness(*t);
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
        // Preparing is refused: a commit before readiness is what the transaction prevents.
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
    const AdmitResult admitted =
        schedule_admission(snapshot.candidate.who, snapshot.incumbent.who, snapshot.role,
                           authority, std::move(activation), sequence, id);
    PreparedReplacement* again = find_txn(id);
    if (again == nullptr) {
        return {false, id, TxnReason::NoSuchTransaction}; // aborted underneath us
    }
    if (!admitted.scheduled) {
        PreparedReplacement copy = *again;
        finish_txn(copy, TxnState::Aborted, TxnReason::AdmissionRefused);
        return {false, id, TxnReason::AdmissionRefused};
    }

    // SCHEDULED, NOT COMMITTED (PR-07). The envelope now in the queue will do the
    // whole admission and terminalize this transaction when it lands. Until then
    // the incumbent is the service, the candidate is sealed, the slot is held and
    // the candidate is still exclusively promised here — and this transaction can
    // still be aborted, in which case the queued admission finds no record and
    // refuses rather than reviving anything.
    again->state = TxnState::AdmissionPending;
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

bool Switchboard::take_outcome(WeaveId op, TxnId id, TxnOutcome& out) {
    // The id NARROWS the question; it never widens the authority. Everything the
    // wider overload requires — the exact operator life and incarnation — is
    // required here identically, so possessing a transaction id buys a stranger
    // nothing it did not already have.
    const ParticipantRef now = participant(op);
    for (std::size_t i = 0; i < outcomes_.size(); ++i) {
        if (outcome_of_[i] == now && outcomes_[i].id == id) {
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
    // What the announcement needs is read before any hook runs: aborting a prepared
    // replacement discards its candidate, and if this weave is that candidate the hook erases
    // the record `rec` points at.
    BusEvent ev;
    ev.kind = EventKind::Died;
    ev.target = id;
    ev.schema_name = rec->state_schema->name();
    ev.schema_version = rec->state_schema->version();

    // Committing the death includes ending its unfinished conversations (ANS-04)
    // and its unfinished transactions (PR-02), both BEFORE the announcement so
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
        begin_new_life(*rec); // a revival is a NEW LIFE behind the same id (MSG-03)
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
    // Joint publication: the snapshot a swap revives from was taken through `snapshot_bytes`,
    // which showed the predecessor every pending publication first, so one it applied is in
    // these bytes and not shown again. One it could not apply is not: it returns to Pending
    // and the successor is shown it, which is what makes a reload the repair of a held weave.
    reset_failed_application_for_successor(id);
    // A SWAP CAN ALSO BE A REVIVAL: this path marks the weave alive whatever it was
    // before, so if it was dead, this is a new life as well as new code. If it was
    // alive, the life continues — a live code reload is not a death, and speech
    // already in the queue is still that same living weave's (MSG-03).
    begin_new_life(*rec);
    rec->alive = true;
    // NEW CODE BEHIND A STABLE ID IS A NEW INCARNATION (ANS-02). The WeaveId is
    // deliberately unchanged — that is what reload means — so this counter is the
    // only thing that distinguishes the successor from the incarnation that may
    // have earned a deferred answer. Bumping it here, then forgetting that
    // weave's unfinished conversations, is what keeps handler-surviving authority
    // from quietly becoming reload-surviving authority.
    ++rec->incarnation;
    // The claim-set belongs to the code (SENSE-04), so new code declares it again. The weave's
    // latest claims stay: a reload is not the claimant's death, and a reading carries the
    // incarnation a claim was made under. The claim moves without a gap (LIFE-08): the
    // successor's vocabulary is claimed before the predecessor's is dropped, so a shape both
    // declare never stops resolving.
    {
        // THE EMIT-SET BELONGS TO THE CODE TOO, and is re-read for the same reason.
        std::vector<std::shared_ptr<const Schema>> fresh;
        for (auto& s : rec->weave->claimed_schemas()) {
            if (s) {
                fresh.push_back(std::move(s));
            }
        }
        std::vector<std::shared_ptr<const Schema>> fresh_emits;
        for (auto& s : rec->weave->emitted_schemas()) {
            if (s) {
                fresh_emits.push_back(std::move(s));
            }
        }
        // The successor's whole vocabulary is its CLOSURE, as at registration:
        // every component every declared shape nests, so the same wall reads the
        // same set through both doors and a code swap cannot narrow it.
        std::vector<std::shared_ptr<const Schema>> vocabulary;
        auto declare = [&vocabulary](const std::shared_ptr<const Schema>& s) {
            collect_referenced(*s, vocabulary);
            vocabulary.push_back(s);
        };
        for (const auto& s : rec->accept) {
            declare(s);
        }
        for (const auto& s : fresh) {
            declare(s);
        }
        for (const auto& s : fresh_emits) {
            declare(s);
        }
        declare(rec->state_schema);
        SchemaClaimScope next = registry_.claim(vocabulary);
        // The grant did not change under the new code, so its producer claim is
        // re-taken into the successor scope. Forgetting this would let a code
        // swap quietly drop a shape the weave is still authorized to speak.
        registry_.claim_known(next, named_send_shapes(rec->grant));
        for (auto& s : fresh) {
            if (auto canon = registry_.lookup(s->name(), s->version())) {
                s = std::move(canon);
            }
        }
        for (auto& s : fresh_emits) {
            if (auto canon = registry_.lookup(s->name(), s->version())) {
                s = std::move(canon);
            }
        }
        rec->claims = std::move(fresh);
        rec->emits = std::move(fresh_emits);
        rec->schemas = std::move(next); // acquire-then-release: the overlap is the point
    }
    forget_deferred_for(id);
    // New code, or a revival, is a new participant to a transaction, which must become
    // terminal promptly: a coordinator reloaded under a Ready transaction is noticed here,
    // not at commit.
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
