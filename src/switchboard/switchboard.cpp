// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/switchboard/switchboard.hpp>

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace loom {

namespace {

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

    // Seed last-known-good from an initial snapshot, gated against its own schema.
    Value snap = incoming->snapshot();
    std::shared_ptr<const Schema> state_schema = snap.schema_ptr();

    // ONE TRANSACTION FOR THE WHOLE VOCABULARY (LIFE-08). Every shape this weave
    // needs resolvable is claimed together, so a disagreement about the LAST of
    // them leaves no trace of the first: before this line the registry knew
    // nothing new, and if it throws it still knows nothing new. The previous
    // shape registered each schema as it went, and a conflict partway down the
    // accept-set left the earlier ones published under a weave that never came
    // into existence.
    //
    // The claim also outlives nothing: it lives in the record built below, and
    // dies when that record is erased.
    std::vector<std::shared_ptr<const Schema>> vocabulary = accept;
    vocabulary.insert(vocabulary.end(), claims.begin(), claims.end());
    vocabulary.push_back(state_schema);
    SchemaClaimScope schemas = registry_.claim(vocabulary);
    // ...AND THE SHAPES THIS WEAVE MAY SPEAK BUT DOES NOT DEFINE (LIFE-08).
    //
    // A weave's accept-set is what it will HEAR; its grant's named send rules are
    // what it may SAY — and a producer's bytes need the shape resolvable at the
    // seam just as much as a consumer's door does. A storage client authorized for
    // `StoragePut v1` keeps needing that shape to mean something after the broker
    // that defined it unmounts, or its next send stops being an honest "nobody
    // holds that role" and becomes "I have never heard of that shape".
    //
    // Claimed BY KEY, because a producer has no definition to offer: it pins what
    // the system already knows and skips what it does not, so a shape nobody ever
    // published stays unpublished and the emission meets the seam (MSG-08). A
    // wildcard rule names nothing and claims nothing — `allow_any` declares no
    // vocabulary to depend on.
    // docs/laws/lifecycle-laws.md
    registry_.claim_known(schemas, named_send_shapes(grant));
    // Adopt the canonical owners the registry settled on, so every weave that
    // accepts a shape holds the SAME Schema object for it — exactly what
    // `register_schema(...).schema` handed back before. `state_schema` keeps the
    // weave's own object, also as before: it is the door its own snapshot is
    // admitted against, and the gate compares content, never pointers.
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
    // A WEAVE OUTLIVES ITS OWN CALLBACK (LIFE-06).
    //
    // FIRST — before the lookup and before any mutation whatever — because the
    // entire content of this refusal is that NOTHING happened: no role released,
    // no office or personal claim forgotten, no conversation abandoned, no
    // transaction invalidated, no registry entry erased, no ownership transferred.
    // A check placed one line later would be a check that undoes things.
    //
    // IT MUST REFUSE RATHER THAN DEFER: a successful removal hands back a
    // `unique_ptr` the caller may reset on the next line, so "erase now, destroy
    // later" is not an implementation this signature can have. Loom cannot both
    // transfer unique ownership and secretly retain the object.
    //
    // `current_target_` IS THE EXACT OBJECT WHOSE MEMBER FUNCTION IS RUNNING —
    // assigned around each of the two `Weave::handle` calls (`deliver_one` and
    // `deliver_admission`) and cleared by `DeliveryScope` on every exit path
    // including a throw. Deliberately NOT `in_dispatch_`, which names a much wider
    // interval: a dispatch turn may legitimately remove a DIFFERENT weave — the
    // transaction layer does exactly that when a candidate refuses preparation —
    // and freezing the registry for a whole turn would make one participant's
    // delivery everybody's problem.
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
    // Its unfinished conversations end with it, in both directions: it can no
    // longer answer, and nothing can be answered TO it. Unconditional (ANS-04):
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
                                     Provenance provenance, TxnId preparation) {
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    msg.provenance = std::move(provenance);
    // AND EVERY ENQUEUE PATH ALSO DECIDES WHOSE LIFE IS SPEAKING (MSG-03). The
    // stamp is read from the bus's own record of the sender, never from anything
    // the caller supplied, for exactly the reason provenance is: a weave hands the
    // bus a Message, and the bus decides the facts about it.
    const std::uint64_t life = gated ? life_of(msg.sender) : 0;
    Envelope env{std::move(msg), target, seq, gated, std::string{}, life};
    env.preparation = preparation; // invalid for every caller but one
    queue_.push_back(std::move(env));
    return Ticket{seq};
}

Ticket Switchboard::enqueue_role(std::string role, Message msg, bool gated,
                                 Provenance provenance) {
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
    // The same single assignment that makes provenance unforgeable on the
    // directed path: ordinary callers pass nothing and the default ERASES
    // whatever the Message carried; only the office-authorship door passes a
    // verified fact.
    msg.provenance = std::move(provenance);
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

void Switchboard::note_seam_refusal(WeaveId sender, WeaveId target, std::string_view claimed_name,
                                    std::uint32_t claimed_version, const Refusal& refusal) {
    // Deliberately `refuse_now`'s body rather than a second mechanism — the only
    // difference is that no admitted Message exists to read a schema off, so the
    // CLAIMED name and version are passed in. Same seq, same journal slot, same
    // tap event: one refusal altitude, whichever side of the seam it happened on.
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{Disposition::Refused, refusal}};
    BusEvent ev;
    ev.kind = EventKind::Refused;
    ev.seq = seq;
    ev.target = target;
    ev.sender = sender;
    ev.schema_name = std::string(claimed_name);
    ev.schema_version = claimed_version;
    ev.refusal = refusal;
    emit(ev);
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
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    msg.provenance = Provenance::attested(Provenance::Kind::Answer, 0);
    Envelope env{std::move(msg), to, seq, /*gated=*/true, std::string{}, life_of(as_sender)};
    env.answer_target = AnswerTarget{true, requester_life, requester_incarnation};
    // ...and WHICH ASK is being answered, carried out of the conversation the same
    // way it was carried in. Both answer doors reach this line, so an immediate
    // answer and one deferred across a dozen deliveries prove exactly the same
    // thing — which is why there is one readiness definition rather than two.
    env.preparation = preparation;
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
    // This needs its own function because the staleness sweep below CANNOT
    // express it: `kill` leaves the id and the incarnation exactly as they were,
    // so every record still looks perfectly current. Without this, a crashed weave
    // revived from its own snapshot — the isolation supervisor's ordinary recovery
    // path — would come back holding its predecessor's answer rights.
    // ANS-04; docs/laws/answer-authority-laws.md
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
    const TxnId preparation = rec->preparation;
    *rec = DeferredRecord{};

    // THE SAME DOOR THE IMMEDIATE ANSWER LEAVES BY, carrying the requester facts
    // the RECORD kept — the ones from when the ask was delivered, never today's.
    return enqueue_answer(to, as_sender, std::move(msg), correlation, requester_life,
                          requester_incarnation, preparation);
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
    // Requiring a Switchboard to MINT one says nothing about WHICH Switchboard, so
    // the check has to be the issuer. An ordinary weave may legally stand up a
    // decoy board of its own and mint a genuine authority from it — a Switchboard
    // is an ordinary object — and the only thing that decoy cannot be is THIS
    // Loom. Accepting an unchecked authority here would spend it anyway.
    //
    // The check lives here rather than in any consumer: holding an authority gives
    // you no way to ask the question and no standing to answer it. `issued_here`
    // also fails for an authority whose board has been destroyed — the lifetime
    // rule, not a special case.
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

    // ONE STATE TRANSITION. Grant, revoke, widen and narrow are the same act — a
    // replacement — so there is no window in which a subject holds the old rules
    // and the new ones, or neither. Nothing between the two assignments below can
    // observe an intermediate state: the board is single-threaded and neither line
    // dispatches, pumps, or calls anything a weave wrote.
    //
    // The claim moves BEFORE the old one is released (acquire, then release),
    // so a shape named by both the outgoing and incoming authority never falls to
    // zero claims and stops resolving for the length of one assignment.
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
        const std::uint64_t seq = next_seq_++;
        journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}}; // Pending, owns seq
        // Rebuilt field by field, which also means a published Message carries no
        // provenance whatever the caller's copy held. EVERY recipient's envelope is
        // stamped, so a publication cannot fan out past a dead author on the
        // strength of one check: each delivery answers the question for itself.
        // An office-authored publication (the one caller that passes a
        // provenance) stamps the SAME verified fact on every recipient's
        // envelope — one authorship moment, one fact, every listener.
        Envelope env{Message(msg.payload, msg.sender, msg.reply_to, msg.correlation), rec.id,
                     seq, gated, std::string{}, sender_life};
        env.msg.provenance = provenance;
        queue_.push_back(std::move(env));
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
// THE AUTHORIZATION MOMENT IS HERE — authorship/enqueue, never delivery. Each
// door asks the one question ("does this exact sender hold that role NOW, as it
// deliberately asks to speak as it?"), stamps the verified fact into the
// envelope's provenance on yes, and refuses visibly on no. From here on the
// fact is HISTORY: deliver_one carries it untouched, rechecks nothing about it,
// and every independent delivery law (sender life, the seal, the grant,
// routing) still runs exactly as it always did.

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
        // seq (only possible when a single pump outruns the window), the guard leaves
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
    // EACH EVENT HAS ITS OWN VIEW OF THE TAP LIST (MSG-11).
    //
    // An observer may subscribe or unsubscribe from inside a notification — the
    // console and the bridge already do the second, from destructors — and the
    // live container is a vector, so walking it while a callback grows it read
    // freed memory and walking it while a callback erased from it silently
    // SKIPPED the observer that shifted into the vacated slot. Neither was
    // documented; the skip was not even loud.
    //
    // The view is taken here, at entry, so every emission — including one an
    // observer causes from inside another — decides its own recipients once.
    const std::vector<std::pair<ObserverId, std::shared_ptr<Observer>>> view = observers_;
    for (const auto& observer : view) {
        // A REMOVAL TAKES EFFECT WITHIN THE EVENT IT IS MADE IN, and that is the
        // one place this deliberately departs from a pure snapshot. Both the
        // console and the bridge call `remove_observer` to stop a callback
        // *before the members it captured die*; honouring the snapshot instead
        // would turn that idiom into a use-after-free. An ADDITION is the
        // opposite case — nothing is unsafe about waiting — so it waits, and the
        // event a subscriber was added during is not one it hears.
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
    // AN ADMISSION IS ITS OWN DELIVERY (PR-08). It takes the whole turn: it
    // moves production topology and hands the candidate its activation, and it
    // does not travel the ordinary authorization path below — a committed
    // activation is Loom's act, not the coordinator's speech, and re-deriving its
    // standing from a mutable grant or a mutable sender life is exactly how a
    // successful admission used to become a service that was never told it was
    // alive.
    if (env.admission.present) {
        deliver_admission(std::move(env));
        return;
    }

    BusEvent ev;
    ev.seq = env.seq;
    ev.target = env.target;
    ev.sender = env.msg.sender;
    ev.schema_name = env.msg.payload.schema().name();
    ev.schema_version = env.msg.payload.schema().version();
    // The STAMPED authorship fact, read from the envelope — never a role_of()
    // lookup, which would report current membership instead of historical
    // authorship (MSG-07). Set before any refusal branch, so a refused
    // office-authored delivery still shows which office it was authored as.
    ev.authored_role = std::string(env.msg.provenance.authored_role());

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
        // A WEAVE-ORIGINATED MESSAGE BELONGS TO THE LIFE THAT AUTHORED IT (MSG-03).
        //
        // QUEUEING IS THE GAP THIS CLOSES. Ending a dying participant's
        // conversations (ANS-04) cannot reach a message it had merely QUEUED, which
        // names no conversation yet. Delivered later, that message would become
        // speech from whatever now answers to the same id — the same weave revived,
        // mid-sentence from a life that ended.
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
        // EFFECTIVE AUTHORITY, AT THE MOMENT OF DELIVERY (GATE-05). Baseline union
        // delegated, read off the record the router just found — never a value
        // captured when this message was queued.
        //
        // That distinction is the whole of live revocation. A message authored
        // while the sender held a delegated rule, queued, and delivered after an
        // administrator took that rule back, is refused here: what was true at
        // send time buys nothing, because nothing on the envelope remembers it.
        // Approval changes authority, not history — and so does withdrawal.
        const bool permitted =
            sender != nullptr &&
            (env.role.empty()
                 ? effective_permits(sender->grant.live(), sender->delegated, ev.schema_name,
                                     ev.schema_version, env.target)
                 : effective_permits_role(sender->grant.live(), sender->delegated, ev.schema_name,
                                          ev.schema_version, env.role));
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
    // (ANS-03). MSG-03 binds a message to the life that AUTHORED it, protecting
    // the answerer's side; this is the other half — the participant the answer was
    // earned FOR.
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
        // ...AND IT REMEMBERS WHO ASKED, not merely where to send (ANS-03). Captured
        // HERE, at the delivery that earns the authority, so that an answer produced
        // later — this handler's, or a deferred one spent minutes from now — is bound
        // to the requester that actually asked rather than to whatever occupies that
        // id when the answer is finally written.
        const WeaveRecord* asker = find(env.msg.sender);
        // AN ASK SEEDS AN ANSWERABLE CONVERSATION; ITS ANSWER DOES NOT SEED ANOTHER.
        //
        // Found by reading rather than by a failing test. Seeding this from every
        // envelope meant an answer-to-an-answer INHERITED the ask's identity — and
        // since `enqueue_answer` also copies the correlation forward at each hop, a
        // coordinator that answered the readiness instead of consuming it could have
        // a later, unrelated exchange satisfy every term:
        //
        //   ask (preparation = T)  ->  answer (T)  ->  answer-to-it (T)  ->  answer (T)
        //                                                                    ^ not the
        //                                                                      ask's answer
        //
        // The payload is not what decides readiness, so nothing could be smuggled
        // through it — but "this delivery answers THAT ask" would have been false,
        // which is the one thing this field exists to make exactly true.
        const TxnId answerable = env.answer_target.present ? TxnId{} : env.preparation;
        authority_ = ReplyAuthority{env.msg.sender,
                                    env.msg.correlation,
                                    /*spent=*/false,
                                    trusted.payload.schema_ptr(),
                                    asker == nullptr ? 0 : asker->life,
                                    asker == nullptr ? 0 : asker->incarnation,
                                    answerable};
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
        rec->weave->handle(trusted, weave_bus); // may enqueue further deliveries
    } // ...and the authority does not outlive the handler, by ANY exit path.
    // Cleared BEFORE the journal and the tap, exactly as it was when the three
    // assignments sat here: an observer of a Delivered event is not inside the
    // delivery and must not find one live.
    record(env.seq, Disposition::Delivered, Refusal{});
    ev.kind = EventKind::Delivered;
    ev.payload = &trusted.payload;
    emit(ev);
}

void Switchboard::pump() {
    if (in_dispatch_) {
        return; // non-reentrant: a handler's sends were enqueued, not nested
    }
    // Scoped, because a native handler that throws unwinds straight past this
    // line — and a dispatch flag left standing makes every later pump believe
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
    // The bound is a FACT ABOUT THE QUEUE, taken once, before anything runs —
    // which is the whole difference from a number the caller supplies. A
    // handler's own continuation is enqueued behind this snapshot and is simply
    // not part of this turn, so a self-re-arming producer cannot hold the turn
    // open and a busy bus still clears its backlog in one go.
    return dispatch_at_most(queue_.size());
}

std::size_t Switchboard::dispatch_at_most(std::size_t budget) {
    if (in_dispatch_) {
        return 0; // non-reentrant, exactly as pump() is
    }
    const DispatchGuard dispatching(*this); // and unpoisoned by a throw, exactly as pump() is
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

    // THE OWNER MUST STILL BE THE OWNER (PR-03).
    //
    // Without this, the strongest act in the system — moving production topology —
    // would rest on a stale fact: a trusted host caller holding a perfectly good
    // lifecycle authority could admit a candidate whose coordinator had died and
    // revived, been reloaded into new code, or been removed entirely. A preparation
    // belongs to a LIFE, and that life is over.
    //
    // Two halves, and both matter: the seal on the record must still name this
    // exact owner (it could have been discarded and resealed to somebody else),
    // and that owner must still be the participant standing at that address.
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
    // THE RECIPIENT'S HALF OF THE CONTRACT, asked before anything moves.
    //
    // A candidate without the activation contract is not admissible: discovering
    // at delivery that the new service never accepted `zen.Activated` is
    // discovering it AFTER the role has moved, which is the whole defect. Both
    // questions the ordinary path would ask later are asked here — the accept-set
    // door and the gate — against the exact payload this admission will deliver.
    //
    // The answer is stable by construction, which is what makes prevalidation a
    // guarantee rather than a hope: a weave's accept-set is fixed at registration
    // (neither `swap_state` nor `reload` rewrites it — reload refuses outright on
    // a drifted accept-set), and `admit()` is a pure function of a payload and a
    // schema. So a door that answers here answers the same way at dispatch, where
    // it is asked again anyway.
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

    // ---- CAN THE CANDIDATE RECEIVE ITS OWN FIRST BREATH? ---------------------
    //
    // Asked here, before a single field moves. The payload is validated against
    // the candidate's real door and its real gate; a weave that cannot take the
    // activation is refused as a candidate rather than admitted and then left
    // unable to hear about it. The trusted result is thrown away — the dispatch
    // re-admits the pristine payload — because prevalidation exists to REFUSE
    // early, not to smuggle a pre-gated value past the one gate.
    if (!activation_deliverable(*cand, activation.payload)) {
        return {false, AdmitRefusal::CandidateContract, Ticket{}};
    }

    // ---- ACTIVATION FIRST, and this is where the queue resisted the model -----
    //
    // Role resolution is a DELIVERY-time decision, so a role-addressed message
    // enqueued before this moment resolves to whoever holds the role when it is
    // finally dispatched. Appending the activation at the tail would let ordinary
    // production reach a weave that has not yet been told it is alive.
    //
    // The envelope is placed immediately ahead of the FIRST queued envelope that
    // could reach this candidate: one addressed to the role being committed, or to
    // the candidate itself. That is the narrowest placement that makes activation
    // the candidate's first live delivery. Every other message keeps its order,
    // and nothing is dropped — the alternative (discarding the older traffic) would
    // buy ordering with silence.
    //
    // THE TOPOLOGY CHANGE HAPPENS AT THAT SAME POINT, which is what keeps the
    // ordering law true: everything ahead of the envelope was queued while the
    // incumbent was the service and still resolves to the incumbent, and everything
    // behind it — including anything this call's caller enqueues next — arrives
    // after the candidate has been told.
    // PR-05; docs/laws/replacement-laws.md
    const std::uint64_t seq = next_seq_++;
    journal_[seq % kJournalCapacity] = JournalSlot{seq, DeliveryOutcome{}};
    activation.sender = owner.who;
    activation.provenance = Provenance::attested(Provenance::Kind::Activation, sequence);
    // UNGATED, and this is the phase's semantic decision made structural. A
    // committed activation is not the coordinator's speech to be authorized
    // against its grant and its life — it is Loom's own act, authorized by the
    // authority checked above and performed as part of the admission. So it does
    // not travel the gated path, and none of that path's later questions can
    // unmake a commitment already made. The sender stamp remains the coordinator's
    // because the CONSUMER needs it: `zen.Activated`'s lineage rule is per
    // attesting operator. It describes who admitted; it does not claim who spoke.
    Envelope act{std::move(activation), candidate, seq, /*gated=*/false, std::string{},
                 /*sender_life=*/0};
    act.admission = PendingAdmission{true, cand_ref, inc_ref, owner, role, txn};
    auto at = queue_.begin();
    for (; at != queue_.end(); ++at) {
        if (at->target == candidate || (!at->role.empty() && at->role == role)) {
            break;
        }
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
    //
    // A transaction-borne admission must still be the SAME transaction, still in
    // the state that scheduled it. An abort while pending erases the record, so
    // there is nothing here to find and the queued envelope simply refuses — a
    // pending admission cannot be revived, and no second terminal outcome is ever
    // written, because ending it already wrote the only one.
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
    //
    // Re-asked rather than assumed, and asked BEFORE anything moves. The answer
    // cannot have changed since scheduling — an accept-set is fixed at
    // registration and the gate is pure — but "cannot have changed" is a claim
    // about today's code, and the ordering here is what makes the law true
    // regardless: if the candidate cannot receive its activation, the admission
    // refuses and the incumbent is still the service.
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
    // There is no lock and none is needed: `pump()` is non-reentrant and
    // dispatches one envelope at a time, so an observer's next delivery either
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
    //
    // AFTER the topology moved and AFTER the activation was proven deliverable —
    // which is exactly what "guaranteed" has to mean. Nothing between this line
    // and the handler call below can refuse: the payload is admitted and in hand,
    // the recipient is resolved and alive. Terminalizing here rather than after
    // `handle()` keeps arbitrary weave code out of the transaction's bookkeeping,
    // and the two orderings are observationally identical because nothing can
    // observe the gap.
    if (txn != nullptr) {
        PreparedReplacement copy = *txn;
        finish_txn(copy, TxnState::Committed, TxnReason::None);
    }

    // ---- 6. the candidate's first breath -------------------------------------
    //
    // AND IT IS NOT A QUESTION (LIFE-05). The delivery context below is set by
    // hand rather than by copying `deliver_one`'s, and the difference is the one
    // field that is deliberately absent: THERE IS NO REPLY AUTHORITY.
    //
    // Building this in the ordinary path's image would fabricate a requester: the
    // stamped sender is the OPERATOR that admitted this candidate, not a weave that
    // asked it anything, so an authority naming it would let a candidate answer a
    // request nobody made — queueing a real, provenance-carrying answer to a
    // coordinator that never spoke.
    //
    // It needs no new machinery, because the model already has this category:
    // `answer_as` and `defer_answer_as` both refuse when there is no valid
    // requester — the case they document as "the request came from a root, so
    // there is no requester to answer". Loom's own act belongs in exactly that
    // category. `answer()` therefore queues nothing and refuses visibly, and
    // `defer_answer()` returns an invalid capability BEFORE the deferred
    // registry is touched, so no bounded capacity is consumed.
    //
    // Everything truthful is kept. `current_target_` still names the exact
    // candidate, so the bus still knows who is being dispatched; the delivery
    // facts still describe what this delivery IS (not an answer, from this
    // admitter, no conversation); and the attestation on the message is
    // untouched, so activation is still authentic and still exactly once.
    // Ordinary sends are unaffected — they never consulted this.
    Message trusted(std::move(*admitted), env.msg.sender, env.msg.reply_to, env.msg.correlation);
    trusted.provenance = env.msg.provenance; // Loom's own word, set at enqueue and only there
    {
        // Scoped exactly as the ordinary path is (MSG-10): a candidate's very
        // first breath is still native code, and it may still throw. The topology
        // has already moved and the transaction has already committed, both
        // deliberately — what must not also happen is the bus being left inside a
        // delivery that ended.
        const DeliveryScope delivering(*this);
        current_target_ = env.target;
        authority_ = ReplyAuthority{};
        delivery_ = DeliveryFacts{trusted.provenance.answers_ask(), env.msg.sender,
                                  env.msg.correlation, TxnId{}};
        WeaveBus weave_bus(*this, env.target);
        cand->weave->handle(trusted, weave_bus);
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
    // ORDERING IS THE WHOLE CORRECTNESS ARGUMENT (PR-06).
    //
    // Cleanup discards the candidate, discarding a candidate is a lifecycle change,
    // and a lifecycle change re-enters `invalidate_transactions_for`. If this
    // transaction were still in the active registry at that moment the hook would
    // rediscover it and end it a SECOND time — two terminal truths for one promise,
    // and two slots consumed in a bounded store that then evicts somebody else's
    // result early.
    //
    // So the record leaves the active registry FIRST, and the re-entrant hook has
    // nothing to find. That is structural non-reentrancy rather than a "currently
    // finishing" flag, which would have to be honoured at every future call site
    // instead of being true at one.
    // docs/laws/replacement-laws.md
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

    // THE ASK IS THE COORDINATOR'S SPEECH, and it is sent exactly as the
    // coordinator's own speech would be: stamped with its id, gated against its
    // grant at delivery, and admitted through the seal only because the sender IS
    // the owner. Nothing here widens what a coordinator may say — it adds a fact
    // the transaction will later recognise, not a right.
    //
    // The correlation is Loom's. The caller's is written over, exactly as
    // `enqueue_answer` writes over an answerer's.
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

    // ---- WHOSE VOICE IS THIS? ------------------------------------------------
    //
    // Every term below is read from the delivery Loom is dispatching right now,
    // never from an argument. `id` names the record; it authorizes nothing.
    //
    //   the conversation is open      - one ask, one answer, consumed once
    //   this delivery IS that ask's answer - the bus-private envelope fact; a
    //                                   correlation is a number anyone may write,
    //                                   this is not
    //   Loom attests it as an answer  - provenance no ordinary enqueue can set
    //   the caller is the coordinator - `current_target_`, i.e. who is being
    //                                   dispatched, not who says they are
    //   the speaker is the candidate  - the bus's sender stamp
    //   the correlation matches       - redundant while `preparation` holds, and
    //                                   kept because a silent redundancy is how
    //                                   the remaining wall gets removed by
    //                                   somebody who thought it was the only one
    //
    // A failure here is a refusal of the COMMAND and nothing more: no state
    // moves, no outcome is recorded, and the transaction the forger named is left
    // exactly as legitimate as it was. Hostile traffic does not get to end
    // somebody else's promise.
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
    // WHAT THE ANNOUNCEMENT NEEDS IS READ BEFORE ANY HOOK RUNS, and that is not
    // tidiness: aborting a prepared replacement discards its candidate, and if THIS
    // weave is that candidate the hook erases the very record `rec` points at.
    // ASan found it; the Debug lane did not.
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
    // THE CLAIM-SET BELONGS TO THE CODE (SENSE-04), so new code re-declares it. A
    // native swap changes nothing here (the same object answers the same way); a
    // dynamic reload has rebound its library underneath, and the successor's
    // contract is its own. Re-reading is the only way this record cannot end up
    // describing code that is gone.
    //
    // The weave's existing latest claims are NOT dropped: a reload is not a death
    // of the claimant, and the reading already carries the incarnation the claim
    // was made under, so a consumer can see for itself that a claim predates the
    // current code rather than having it silently withdrawn.
    //
    // AND THE CLAIM MOVES WITHOUT A GAP (LIFE-08). The successor's whole vocabulary
    // is claimed BEFORE the predecessor's claim is dropped, so a shape both
    // declare is at two claims for the length of one assignment and never falls
    // to zero. There is no instant in a code swap when a shape the weave still
    // accepts stops resolving.
    {
        std::vector<std::shared_ptr<const Schema>> fresh;
        for (auto& s : rec->weave->claimed_schemas()) {
            if (s) {
                fresh.push_back(std::move(s));
            }
        }
        std::vector<std::shared_ptr<const Schema>> vocabulary = rec->accept;
        vocabulary.insert(vocabulary.end(), fresh.begin(), fresh.end());
        vocabulary.push_back(rec->state_schema);
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
        rec->claims = std::move(fresh);
        rec->schemas = std::move(next); // acquire-then-release: the overlap is the point
    }
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
