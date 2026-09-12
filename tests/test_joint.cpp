// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// JOINT PUBLICATION OF LATEST CLAIMS (SENSE-06, SENSE-07; docs/reference/joint-publication.md).
//
// The mechanism under test is the section header in zen/switchboard/sense.hpp. What each
// case pins:
//
//   J1  one boundary          two claims change in one delivery; a reader before it sees
//                             both old values, one after it both new, and there is no
//                             delivery in which it could see one of each
//   J2  the hook before work  a claimant standing behind its published claim is shown it
//                             before its next handler runs, and before its snapshot
//   J3  an edit invalidates   the claimant's own ordinary claim over a bound key aborts
//                             the operation before commit; the other owner is untouched
//   J4  a reload invalidates  new code behind a bound claimant aborts it; nothing exchanged
//   J5  removal releases      a removed claimant aborts it and its offers are released
//   J6  authority             the wrong operator, another board's authority, a claimant
//                             outside the ceiling, a busy key, a forged id, an offer by a
//                             stranger, an oversized offer, an undeclared shape, a commit
//                             with an offer missing, and a cancelled operation
//   J7  loaded claimant       a real image offers across the seam and hears the published
//                             value back across it, before its next handle and before a
//                             reload's snapshot; the operation bound to the old incarnation
//                             is aborted by the reload
//   J8  bounds                one live operation per key, the slot and key bounds, a
//                             terminal record kept until its operator releases it, genuine
//                             exhaustion by unreleased records, and release's refusals
//   J9  nothing between       the commit's exchange runs no participant code: a claimant
//                             that counts its deliveries counts none during the commit
//   J10 the operator is told  when a bound participant is removed or replaced, the
//                             operator that accepts zen.JointEnded hears it, once, naming
//                             the operation and the record's reason -- also for an
//                             operation the bus had already ended; a moved claim alone
//                             tells it nothing (the owner answers); the record is the fact
//   J11-J16: publication is not application --
//                             a showing that fails holds the participant, attributably,
//                             loaded and native alike; the substrate's mutation doors
//                             reach the claim. Their own list heads that block.
//   J17-J21: publication is not the end of an
//                             outcome's lifetime, a queued notification is not consumption,
//                             and a repaired owner is not proof its old operation applied --
//                             records kept until released, exhaustion in words, the operator's
//                             lifetime, and the third answer, Declined, native and loaded.
//                             Their own list heads that block.
//   J22 a refused replacement runs nothing of the incumbent's (its own list head).
//   J23-J24: authority is exact and reaches only its holder's own records --
//                             a foreign operator's VALID capability, named with another
//                             operator's operation id, is refused NotOperator before the
//                             record is touched, and the owner still commits (J23); a
//                             capability names the operator's exact life and incarnation,
//                             expires with either (swap, death and revival), cannot reach a
//                             record the successor began, and the host authorizes the
//                             successor by minting again (J24). J6's copied capability and
//                             J19's operator lifetime are the neighbours, not the same
//                             predicates: J6 never reaches the record, J19 retires it.

#include <doctest.h>

#include "switchboard_fixtures.hpp"
#include "weavelib/joint_protocol.hpp"

#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace sbfx;
using namespace joint_test;

namespace {

// ---- the participants -----------------------------------------------------------

/// The document-side owner: claims a DocFact, offers the next one when told, and folds
/// a joint-published one into its state through the hook.
class Doc : public WeaveBase<Doc, ProbeState, Accept<Cmd>, Emit<>, Claims<DocFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        ++state_.deliveries;
        state_.seen_at_delivery = state_.path;
        if (c.verb == "claim") {
            state_.path = c.path;
            state_.epoch = c.epoch;
            last_claim = mail.claim(DocFact{state_.path, state_.epoch});
        } else if (c.verb == "offer") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), DocFact{c.path, c.epoch});
        } else if (c.verb == "offer-stray") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), Stray{7});
        } else if (c.verb == "offer-huge") {
            last = mail.offer(static_cast<std::uint64_t>(c.op),
                              DocFact{std::string(static_cast<std::size_t>(c.epoch), 'x'), 1});
        } else if (c.verb == "arm-fail") {
            fail_next = true; // the next showing counts its attempt and fails before applying
        } else if (c.verb == "arm-decline") {
            decline_next = true; // ...or answers that it keeps its own state (not held)
        } else if (c.verb == "arm-fail-quietly") {
            fail_quietly_next = true; // ...or answers Failed without an exception (held)
        }
    }
    /// THE THREE-WORD FORM of the hook: the
    /// View below keeps the `void` form and the loaded probe the `bool` form.
    Weave::PublishedClaim on_claim_published(const DocFact& published) {
        ++state_.published_seen;
        if (fail_next) {
            fail_next = false;
            throw std::runtime_error("the document owner could not apply its published claim");
        }
        if (fail_quietly_next) {
            fail_quietly_next = false;
            return Weave::PublishedClaim::Failed;
        }
        if (decline_next) {
            decline_next = false;
            return Weave::PublishedClaim::Declined; // A is kept; nothing of B is taken
        }
        state_.path = published.path;
        state_.epoch = published.epoch;
        return Weave::PublishedClaim::Applied;
    }
    JointResult last{};
    SenseClaimResult last_claim{};
    bool fail_next = false;
    bool decline_next = false;
    bool fail_quietly_next = false;
    const ProbeState& state() const { return state_; }
};

/// A participating owner whose EXPOSED state can be written through the substrate's own
/// mutation doors (`zen.PokeWrite`, `zen.PokeResetState`), and whose `after_delivery`
/// mirrors that state into its claim -- the reusable SDK contract J15/J16 pin. Its
/// claim must follow its state whichever door moved it.
struct MirrorState {
    std::string path = "A";
    std::int64_t epoch = 1;
    std::int64_t hook_runs = 0; ///< times after_delivery ran
    ZEN_EXPOSE();
    ZEN_SHAPE(MirrorState, 1, ZEN_FIELD(path), ZEN_FIELD(epoch), ZEN_FIELD(hook_runs));
};

class Mirror : public WeaveBase<Mirror, MirrorState, Accept<Cmd>, Emit<>, Claims<DocFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        if (c.verb == "claim") {
            state_.path = c.path;
            state_.epoch = c.epoch;
            last_claim = mail.claim(DocFact{state_.path, state_.epoch});
            if (last_claim.accepted) {
                claimed_path = state_.path;
                claimed_epoch = state_.epoch;
            }
        } else if (c.verb == "offer") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), DocFact{c.path, c.epoch});
        }
    }
    void on_claim_published(const DocFact& published) {
        ++published_seen;
        state_.path = published.path;
        state_.epoch = published.epoch;
        claimed_path = state_.path;
        claimed_epoch = state_.epoch;
    }
    void after_delivery(Mail& mail) {
        ++state_.hook_runs;
        if (state_.path != claimed_path || state_.epoch != claimed_epoch) {
            last_claim = mail.claim(DocFact{state_.path, state_.epoch});
            if (last_claim.accepted) {
                claimed_path = state_.path;
                claimed_epoch = state_.epoch;
            }
        }
    }
    JointResult last{};
    SenseClaimResult last_claim{};
    std::string claimed_path = "A";
    std::int64_t claimed_epoch = 1;
    int published_seen = 0;
    const MirrorState& state() const { return state_; }
};

struct ViewState {
    std::string shown = "hidden";
    bool focus = false;
    std::int64_t published_seen = 0;
    std::int64_t deliveries = 0;
    ZEN_SHAPE(ViewState, 1, ZEN_FIELD(shown), ZEN_FIELD(focus), ZEN_FIELD(published_seen),
              ZEN_FIELD(deliveries));
};

/// The presentation-side owner: the same three verbs over a ViewFact.
class View : public WeaveBase<View, ViewState, Accept<Cmd>, Emit<>, Claims<ViewFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        ++state_.deliveries;
        if (c.verb == "claim") {
            state_.shown = c.path;
            state_.focus = c.epoch != 0;
            last_claim = mail.claim(ViewFact{state_.shown, state_.focus});
        } else if (c.verb == "offer") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), ViewFact{c.path, c.epoch != 0});
        }
    }
    void on_claim_published(const ViewFact& published) {
        state_.shown = published.shown;
        state_.focus = published.focus;
        ++state_.published_seen;
    }
    JointResult last{};
    SenseClaimResult last_claim{};
    const ViewState& state() const { return state_; }
};

struct OpState {
    std::int64_t op = 0;
    ZEN_SHAPE(OpState, 1, ZEN_FIELD(op));
};

/// The operator: holds the host-minted authority and the two keys it coordinates -- and
/// accepts the bus's word that an operation of its ended, re-reading the record for it;
/// the bus's word that an application of one SETTLED, re-read the same way; and the
/// dispatch refusal of its own sends, so a
/// held participant is met by exact attempt.
class Operator
    : public WeaveBase<Operator, OpState, Accept<Cmd, JointEnded, JointApplied, DispatchRefused>,
                       Emit<Cmd>> {
public:
    void on(const JointEnded& said, Mail& mail) {
        ended.push_back(said);
        ended_status.push_back(mail.joint_status(authority, said.ended_op()));
        ended_attested.push_back(mail.dispatch_refused()); // ordinary speech, never provenance
    }
    void on(const JointApplied& said, Mail& mail) {
        applied.push_back(said);
        applied_status.push_back(mail.joint_status(authority, said.applied_op()));
    }
    void on(const DispatchRefused& said, Mail& mail) {
        if (mail.dispatch_refused()) {
            refused.push_back(said);
        }
    }
    void on(const Cmd& c, Mail& mail) {
        if (c.verb == "nudge") {
            // An ordinary send of this operator's to the weave whose id is `op`.
            last_send = mail.send(WeaveId{static_cast<std::uint64_t>(c.op)}, Cmd{"note"});
        } else if (c.verb == "begin") {
            begun = mail.begin_joint(authority, keys);
            state_.op = static_cast<std::int64_t>(begun.op);
        } else if (c.verb == "commit") {
            committed = mail.commit_joint(authority, static_cast<std::uint64_t>(c.op));
        } else if (c.verb == "cancel") {
            cancelled = mail.cancel_joint(authority, static_cast<std::uint64_t>(c.op));
        } else if (c.verb == "status") {
            status = mail.joint_status(authority, static_cast<std::uint64_t>(c.op));
        } else if (c.verb == "release") {
            released = mail.release_joint(authority, static_cast<std::uint64_t>(c.op));
        }
    }
    JointAuthority authority;
    std::vector<ClaimKey> keys;
    JointBegin begun{};
    JointResult committed{};
    JointResult cancelled{};
    JointResult released{};
    JointStatus status{};
    std::vector<JointEnded> ended;
    std::vector<JointStatus> ended_status;
    std::vector<bool> ended_attested;
    std::vector<JointApplied> applied;
    std::vector<JointStatus> applied_status;
    std::vector<DispatchRefused> refused;
    Ticket last_send{};
    std::uint64_t op() const { return static_cast<std::uint64_t>(state_.op); }
};

/// A reader that observes BOTH claims from inside one delivery, so what it saw is what
/// one instant of the world said.
class Pair : public WeaveBase<Pair, OpState, Accept<Cmd>> {
public:
    void on(const Cmd&, Mail& mail) {
        const SenseReading d = mail.latest<DocFact>(doc);
        const SenseReading v = mail.latest<ViewFact>(view);
        std::string a = d ? from_value<DocFact>(*d.value).path : std::string("?");
        std::string b = v ? from_value<ViewFact>(*v.value).shown : std::string("?");
        seen.emplace_back(std::move(a), std::move(b));
    }
    WeaveId doc{};
    WeaveId view{};
    std::vector<std::pair<std::string, std::string>> seen;
};

template <class W, class... Args>
std::pair<WeaveId, W*> put(Switchboard& bus, Grant grant, std::string role, Args&&... args) {
    auto weave = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = weave.get();
    const WeaveId id = role.empty() ? bus.register_weave(std::move(weave), std::move(grant))
                                    : bus.register_weave(std::move(weave), std::move(grant),
                                                         std::move(role));
    raw->zen_set_self(id);
    return {id, raw};
}

Grant reads_both() {
    return Grant{}
        .allow_observe(DocFact::zen_name, DocFact::zen_version)
        .allow_observe(ViewFact::zen_name, ViewFact::zen_version);
}

void tell(Switchboard& bus, WeaveId who, Cmd cmd) {
    (void)bus.send(who, Message(to_value(cmd)));
}

/// The whole rig: a document owner in one office, a view owner in another, an operator
/// whose ceiling is exactly those two offices, and a reader authorized for both claims.
struct Rig {
    Switchboard bus;
    WeaveId doc{};
    Doc* d = nullptr;
    WeaveId view{};
    View* v = nullptr;
    WeaveId op{};
    Operator* o = nullptr;
    WeaveId reader{};
    Pair* r = nullptr;

    Rig() {
        std::tie(doc, d) = put<Doc>(bus, Grant{}, "joint.doc");
        std::tie(view, v) = put<View>(bus, Grant{}, "joint.view");
        std::tie(op, o) = put<Operator>(bus, Grant{}.allow_to_any(Cmd::zen_name, Cmd::zen_version),
                                       "joint.operator");
        std::tie(reader, r) = put<Pair>(bus, reads_both(), "");
        r->doc = doc;
        r->view = view;
        o->authority = bus.mint_joint_authority(op, {"joint.doc", "joint.view"});
        o->keys = {claim_key<DocFact>(doc), claim_key<ViewFact>(view)};
        // The initial claims: A is current, the view shows it hidden.
        tell(bus, doc, Cmd{"claim", 0, "A", 1});
        tell(bus, view, Cmd{"claim", 0, "hidden", 0});
        bus.drain_until_idle();
        REQUIRE(d->last_claim.accepted);
        REQUIRE(v->last_claim.accepted);
    }

    std::string doc_path() {
        const SenseReading s = bus.observe(doc, DocFact::zen_name, DocFact::zen_version);
        return s ? from_value<DocFact>(*s.value).path : std::string("?");
    }
    std::string view_shown() {
        const SenseReading s = bus.observe(view, ViewFact::zen_name, ViewFact::zen_version);
        return s ? from_value<ViewFact>(*s.value).shown : std::string("?");
    }
    std::uint64_t doc_revision() {
        return bus.observe(doc, DocFact::zen_name, DocFact::zen_version).by.revision;
    }

    /// Begin, and have both owners offer — the prepared state, nothing published.
    std::uint64_t prepare(const std::string& path = "B") {
        tell(bus, op, Cmd{"begin"});
        bus.drain_until_idle();
        REQUIRE_MESSAGE(o->begun.ok, name_of(o->begun.why));
        const std::uint64_t id = o->op();
        tell(bus, doc, Cmd{"offer", static_cast<std::int64_t>(id), path, 2});
        tell(bus, view, Cmd{"offer", static_cast<std::int64_t>(id), path, 1});
        bus.drain_until_idle();
        REQUIRE_MESSAGE(d->last.ok, name_of(d->last.why));
        REQUIRE_MESSAGE(v->last.ok, name_of(v->last.why));
        return id;
    }
    void read() { tell(bus, reader, Cmd{"read"}); }
    void commit(std::uint64_t id) { tell(bus, op, Cmd{"commit", static_cast<std::int64_t>(id)}); }
};

} // namespace

TEST_SUITE("joint") {

TEST_CASE("J1: one commit changes both claims, and a reader sees both-old or both-new") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    // PREPARED IS NOT PUBLISHED: the offers exist, the claims are what they were.
    CHECK(rig.doc_path() == "A");
    CHECK(rig.view_shown() == "hidden");
    CHECK(rig.bus.joint_pending() == 1);
    CHECK(rig.bus.joint_retained_bytes() > 0);
    // ONE POLL: a read, the commit, a read. The commit is one delivery to the operator.
    rig.read();
    rig.commit(id);
    rig.read();
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    REQUIRE(rig.r->seen.size() == 2);
    CHECK(rig.r->seen[0] == std::make_pair(std::string("A"), std::string("hidden")));
    CHECK(rig.r->seen[1] == std::make_pair(std::string("B"), std::string("B")));
    CHECK(rig.doc_path() == "B");
    CHECK(rig.view_shown() == "B");
    CHECK(rig.bus.joint_status(id).state == JointState::Committed);
    CHECK(rig.bus.joint_pending() == 0);
    CHECK(rig.bus.joint_retained_bytes() == 0);
    // THE AUTHORSHIP IS THE CLAIMANT'S, not the operator's: the published claim is still
    // the document owner's own, at its exact incarnation.
    const SenseReading s = rig.bus.observe(rig.doc, DocFact::zen_name, DocFact::zen_version);
    CHECK(s.by.author == rig.doc);
    CHECK(s.by.author_incarnation_is_current);
}

TEST_CASE("J2: a claimant is shown its published claim before its next handler and before a snapshot") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    // PUBLISHED, NOT YET OBSERVED: the owners have not run since the commit.
    CHECK(rig.bus.has_unobserved_publication(rig.doc));
    CHECK(rig.bus.has_unobserved_publication(rig.view));
    CHECK(rig.d->state().path == "A"); // native memory still says A ...
    SUBCASE("the next delivery finds the published value already folded in") {
        tell(rig.bus, rig.doc, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.d->state().seen_at_delivery == "B"); // ...but the handler never saw A
        CHECK(rig.d->state().published_seen == 1);
        CHECK_FALSE(rig.bus.has_unobserved_publication(rig.doc));
        CHECK(rig.bus.has_unobserved_publication(rig.view)); // the view has not run yet
    }
    SUBCASE("a snapshot carries the published value, so a reload cannot revive the old one") {
        const std::string bytes = rig.bus.snapshot_bytes(rig.doc);
        const Admission a = admit(parse(bytes), schema_of<ProbeState>());
        REQUIRE(a.ok());
        CHECK(from_value<ProbeState>(a.value()).path == "B");
        CHECK(rig.d->state().published_seen == 1);
        CHECK_FALSE(rig.bus.has_unobserved_publication(rig.doc));
    }
}

TEST_CASE("J3: the claimant's own claim over a bound key aborts the operation, and the other owner is untouched") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    // A legitimate edit of A while B is prepared: the document owner claims ordinarily.
    tell(rig.bus, rig.doc, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
    CHECK(rig.bus.joint_status(id).reason == JointRefusal::StaleRevision);
    CHECK(rig.bus.joint_retained_bytes() == 0);
    rig.commit(id);
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->committed.ok);
    CHECK(rig.o->committed.why == JointRefusal::WrongState);
    CHECK(rig.doc_path() == "A");
    CHECK(rig.view_shown() == "hidden");
    CHECK_FALSE(rig.bus.has_unobserved_publication(rig.view));
    CHECK(rig.v->state().published_seen == 0);
    // ...and a fresh operation over the same keys is possible: the key is no longer busy.
    const std::uint64_t again = rig.prepare();
    rig.commit(again);
    rig.bus.drain_until_idle();
    CHECK(rig.o->committed.ok);
    CHECK(rig.doc_path() == "B");
}

TEST_CASE("J4: new code behind a bound claimant aborts the operation; nothing is exchanged") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    const std::string bytes = rig.bus.snapshot_bytes(rig.doc);
    const ReviveOutcome swapped = rig.bus.swap_state(rig.doc, bytes);
    REQUIRE(swapped.revived);
    CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
    CHECK(rig.bus.joint_status(id).reason == JointRefusal::ParticipantChanged);
    rig.commit(id);
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->committed.ok);
    CHECK(rig.doc_path() == "A");
    CHECK(rig.view_shown() == "hidden");
    // THE CLAIM SURVIVED THE SWAP (a reload is not a death), stamped as the predecessor's.
    const SenseReading s = rig.bus.observe(rig.doc, DocFact::zen_name, DocFact::zen_version);
    CHECK(s);
    CHECK_FALSE(s.by.author_incarnation_is_current);
    // A stale offer from the old operation, delivered now, is refused for the reason.
    tell(rig.bus, rig.doc, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.d->last.ok);
    CHECK(rig.d->last.why == JointRefusal::WrongState);
}

TEST_CASE("J5: a removed claimant aborts the operation and its offers are released") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    REQUIRE(rig.bus.joint_retained_bytes() > 0);
    std::unique_ptr<Weave> gone = rig.bus.unregister_weave(rig.view);
    REQUIRE(gone != nullptr);
    rig.v = nullptr;
    CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
    CHECK(rig.bus.joint_status(id).reason == JointRefusal::ParticipantChanged);
    CHECK(rig.bus.joint_pending() == 0);
    CHECK(rig.bus.joint_retained_bytes() == 0);
    CHECK(rig.doc_path() == "A");
    // The document owner still works: it claims and is read, as before.
    tell(rig.bus, rig.doc, Cmd{"claim", 0, "A2", 1});
    rig.bus.drain_until_idle();
    CHECK(rig.doc_path() == "A2");
}

TEST_CASE("J6: every unauthorized or mistaken act is refused by name and changes nothing") {
    Rig rig;
    SUBCASE("a second weave presenting the operator's authority is not the operator") {
        // THE COPY IS REFUSED AT THE AUTHORITY, before any operation is looked at. The
        // other case -- a second operator's OWN valid authority named with this
        // operator's operation id, which does reach the record -- is J23.
        auto [other_id, other] = put<Operator>(rig.bus, Grant{}, "joint.other");
        other->authority = rig.o->authority; // copied, as a capability can be
        other->keys = rig.o->keys;
        tell(rig.bus, other_id, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(other->begun.ok);
        CHECK(other->begun.why == JointRefusal::NotOperator);
    }
    SUBCASE("another board's authority is foreign here") {
        Switchboard decoy;
        rig.o->authority = decoy.mint_joint_authority(rig.op, {"joint.doc", "joint.view"});
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK(rig.o->begun.why == JointRefusal::ForeignAuthority);
    }
    SUBCASE("a claimant outside the ceiling cannot be bound") {
        rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.doc"});
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK(rig.o->begun.why == JointRefusal::OutsideCeiling);
    }
    SUBCASE("a declared, never-claimed key has nothing to bind") {
        auto [fresh_id, fresh] = put<Doc>(rig.bus, Grant{}, "joint.fresh");
        (void)fresh;
        rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.fresh", "joint.view"});
        rig.o->keys = {claim_key<DocFact>(fresh_id), claim_key<ViewFact>(rig.view)};
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK(rig.o->begun.why == JointRefusal::NoClaim);
    }
    SUBCASE("a forged operation id, an unbound key, a stranger's offer, an oversized offer and an undeclared shape") {
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"offer", static_cast<std::int64_t>(id + 1000), "B", 2});
        rig.bus.drain_until_idle();
        CHECK(rig.d->last.why == JointRefusal::NoSuchOperation);
        auto [stranger_id, stranger] = put<Doc>(rig.bus, Grant{}, "joint.stranger");
        tell(rig.bus, stranger_id, Cmd{"claim", 0, "S", 1});
        tell(rig.bus, stranger_id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
        rig.bus.drain_until_idle();
        CHECK(stranger->last.why == JointRefusal::NotClaimant);
        // A shape the operation does not bind is refused as unbound BEFORE its declaration
        // is consulted: a bound key always has a claim, and a claim always has a
        // declaration, so `Undeclared` is unreachable through this door and `NotBound` is
        // the honest answer.
        tell(rig.bus, rig.doc, Cmd{"offer-stray", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK(rig.d->last.why == JointRefusal::NotBound);
        tell(rig.bus, rig.doc, Cmd{"offer-huge", static_cast<std::int64_t>(id), "",
                                   static_cast<std::int64_t>(Switchboard::kMaxJointOfferBytes)});
        rig.bus.drain_until_idle();
        CHECK(rig.d->last.why == JointRefusal::TooLarge);
        // None of that ended the operation, and the earlier good offers still stand.
        CHECK(rig.bus.joint_status(id).state == JointState::Preparing);
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok);
        CHECK(rig.doc_path() == "B");
    }
    SUBCASE("a commit with an offer missing aborts, and a cancelled operation cannot commit") {
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        const std::uint64_t id = rig.o->op();
        tell(rig.bus, rig.doc, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->committed.ok);
        CHECK(rig.o->committed.why == JointRefusal::OfferMissing);
        CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
        CHECK(rig.doc_path() == "A");
        const std::uint64_t again = rig.prepare();
        tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(again)});
        rig.commit(again);
        rig.bus.drain_until_idle();
        CHECK(rig.o->cancelled.ok);
        CHECK_FALSE(rig.o->committed.ok);
        CHECK(rig.o->committed.why == JointRefusal::WrongState);
        CHECK(rig.bus.joint_status(again).reason == JointRefusal::Cancelled);
        CHECK(rig.bus.joint_retained_bytes() == 0);
        CHECK(rig.doc_path() == "A");
    }
    SUBCASE("the operator's verbs are refused outside a live delivery") {
        // A host holding a bus is root; the gated door needs a live delivery of the
        // caller, and here nobody is being delivered to.
        const JointBegin b = rig.bus.begin_joint_as(rig.op, rig.o->authority, rig.o->keys);
        CHECK_FALSE(b.ok);
        CHECK(b.why == JointRefusal::NoLiveDelivery);
        const JointResult o = rig.bus.offer_claim_as(rig.doc, 1, to_value(DocFact{"B", 2}));
        CHECK(o.why == JointRefusal::NoLiveDelivery);
    }
}

TEST_CASE("J7: a loaded claimant offers across the seam and is shown its published claim back across it") {
    Rig rig;
    Kernel kernel(rig.bus);
    // The loaded image replaces the native document owner in the ceiling.
    const LoadResult loaded = kernel.load("joint-doc", ZEN_SO_JOINT, "joint.loaded");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.loaded", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(loaded.id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    const auto probe = [&] {
        const Admission a =
            admit(parse(rig.bus.snapshot_bytes(loaded.id)), schema_of<ProbeState>());
        REQUIRE(a.ok());
        return from_value<ProbeState>(a.value());
    };
    REQUIRE(probe().last_offer_ok == 1);
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t id = rig.o->op();
    tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
    tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), "B", 1});
    rig.bus.drain_until_idle();
    REQUIRE(probe().last_offer_ok == 1);
    REQUIRE(rig.v->last.ok);
    SUBCASE("the next delivery to the image finds the published value folded in") {
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        CHECK(rig.bus.has_unobserved_publication(loaded.id));
        tell(rig.bus, loaded.id, Cmd{"note"});
        rig.bus.drain_until_idle();
        const ProbeState s = probe();
        CHECK(s.seen_at_delivery == "B");
        CHECK(s.published_seen == 1);
        CHECK(s.path == "B");
        CHECK_FALSE(rig.bus.has_unobserved_publication(loaded.id));
        const SenseReading d =
            rig.bus.observe(loaded.id, DocFact::zen_name, DocFact::zen_version);
        CHECK(from_value<DocFact>(*d.value).path == "B");
        // THE APPLICATION SETTLES: once the view
        // has been shown as well, the operation says Applied and the operator is told once.
        CHECK(rig.bus.application_of(loaded.id) == JointApplication::Applied);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Pending); // the view
        tell(rig.bus, rig.view, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
        REQUIRE(rig.o->applied.size() == 1);
        CHECK(rig.o->applied[0].applied);
        CHECK(rig.o->applied[0].applied_op() == id);
        CHECK(rig.o->applied_status[0].application == JointApplication::Applied);
    }
    SUBCASE("a reload right after the commit carries the published value, and the old operation is over") {
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        // Another operation, bound to THIS incarnation, is in flight when the reload lands.
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->begun.ok);
        const std::uint64_t pending = rig.o->op();
        const std::filesystem::path copy =
            std::filesystem::temp_directory_path() / "zen-joint-reload.so";
        std::filesystem::copy_file(ZEN_SO_JOINT, copy,
                                   std::filesystem::copy_options::overwrite_existing);
        const ReloadResult r = kernel.reload_from("joint-doc", copy.string());
        REQUIRE_MESSAGE(r.reloaded, r.error);
        CHECK(rig.bus.joint_status(pending).state == JointState::Aborted);
        CHECK(rig.bus.joint_status(pending).reason == JointRefusal::ParticipantChanged);
        const ProbeState s = probe();
        CHECK(s.path == "B");            // the snapshot the swap revived from carried it
        CHECK(s.published_seen == 1);    // shown once, to the predecessor, at the snapshot
        CHECK_FALSE(rig.bus.has_unobserved_publication(loaded.id));
        // The successor is a new participant: it offers into a fresh operation and that one commits.
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->begun.ok);
        const std::uint64_t fresh = rig.o->op();
        tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(fresh), "C", 3});
        tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(fresh), "C", 1});
        rig.commit(fresh);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok);
        CHECK(rig.view_shown() == "C");
        tell(rig.bus, loaded.id, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(probe().path == "C");
        // UNLOAD BEFORE REMOVING THE COPY: a mapped image cannot be deleted on Windows
        // (access denied), and a leftover temp file is not this case's verdict.
        REQUIRE(kernel.unload("joint-doc"));
        std::error_code ec;
        std::filesystem::remove(copy, ec);
    }
    if (kernel.is_loaded("joint-doc")) {
        REQUIRE(kernel.unload("joint-doc"));
    }
}

TEST_CASE("J8: one live operation per key, bounded slots and keys, a terminal record kept until its operator releases it, and genuine exhaustion in words") {
    Rig rig;
    const std::uint64_t first = rig.prepare();
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->begun.ok);
    CHECK(rig.o->begun.why == JointRefusal::KeyBusy);
    // A LIVE RECORD CANNOT BE RELEASED: cancel it, do not lose it.
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->released.ok);
    CHECK(rig.o->released.why == JointRefusal::WrongState);
    tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->cancelled.ok);
    // More keys than one operation binds.
    rig.o->keys.assign(Switchboard::kMaxJointKeys + 1, claim_key<DocFact>(rig.doc));
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    CHECK(rig.o->begun.why == JointRefusal::Exhausted);
    rig.o->keys = {claim_key<DocFact>(rig.doc), claim_key<ViewFact>(rig.view)};
    // A TERMINAL RECORD IS KEPT, readable by its
    // operator and by the host, until its operator releases it: its outcome is owed to the
    // operator that began it, and no begin -- its own or anybody's -- takes it.
    tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK(rig.o->status.state == JointState::Aborted);
    CHECK(rig.o->status.reason == JointRefusal::Cancelled);
    CHECK(rig.bus.joint_status(first).state == JointState::Aborted);
    CHECK(rig.bus.joint_records() == 1);
    // A STRANGER PRESENTING THE OPERATOR'S AUTHORITY RELEASES NOTHING.
    auto [other_id, other] = put<Operator>(rig.bus, Grant{}, "joint.other");
    other->authority = rig.o->authority;
    tell(rig.bus, other_id, Cmd{"release", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK_FALSE(other->released.ok);
    CHECK(other->released.why == JointRefusal::NotOperator);
    CHECK(rig.bus.joint_status(first).state == JointState::Aborted);
    // THE OPERATOR'S RELEASE: Missing to it and to the host, the slot free, the id never
    // handed out again, and a second release naming nothing.
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK(rig.o->released.ok);
    CHECK(rig.bus.joint_status(first).state == JointState::Missing);
    CHECK(rig.bus.joint_records() == 0);
    tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK(rig.o->status.state == JointState::Missing);
    CHECK(rig.o->status.reason == JointRefusal::NoSuchOperation);
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(first)});
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->released.ok);
    CHECK(rig.o->released.why == JointRefusal::NoSuchOperation);
    // GENUINE EXHAUSTION: as many terminal records as there are slots, released by nobody;
    // the next begin is refused in words and takes none of them.
    std::vector<std::uint64_t> kept;
    for (std::size_t i = 0; i < Switchboard::kMaxJointOperations; ++i) {
        const std::uint64_t id = rig.prepare("B" + std::to_string(i));
        tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->cancelled.ok);
        kept.push_back(id);
    }
    CHECK(rig.bus.joint_records() == Switchboard::kMaxJointOperations);
    CHECK(rig.bus.joint_pending() == 0);
    CHECK(rig.bus.joint_retained_bytes() == 0);
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    CHECK_FALSE(rig.o->begun.ok);
    CHECK(rig.o->begun.why == JointRefusal::Exhausted);
    for (const std::uint64_t id : kept) {
        CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
        CHECK(rig.bus.joint_status(id).reason == JointRefusal::Cancelled);
    }
    // RELEASE ONE, AND THE NEXT BEGIN TAKES THAT SLOT with a fresh id.
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(kept[0])});
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->released.ok);
    const std::uint64_t fresh = rig.prepare("C");
    CHECK(std::find(kept.begin(), kept.end(), fresh) == kept.end());
    CHECK(rig.bus.joint_status(fresh).state == JointState::Preparing);
    CHECK(rig.bus.joint_status(kept[0]).state == JointState::Missing);
    CHECK(rig.bus.joint_records() == Switchboard::kMaxJointOperations);
    // ...and what an operator keeps, it keeps: released here, so nothing is left owed.
    tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(fresh)});
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(fresh)});
    for (std::size_t i = 1; i < kept.size(); ++i) {
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(kept[i])});
    }
    rig.bus.drain_until_idle();
    CHECK(rig.bus.joint_records() == 0);
}

TEST_CASE("J9: the commit's exchange runs no participant code") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    const std::int64_t doc_before = rig.d->state().deliveries;
    const std::int64_t view_before = rig.v->state().deliveries;
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    // The owners were never entered: no delivery, no hook (the hook runs at THEIR next
    // delivery or snapshot, not inside the operator's).
    CHECK(rig.d->state().deliveries == doc_before);
    CHECK(rig.v->state().deliveries == view_before);
    CHECK(rig.d->state().published_seen == 0);
    CHECK(rig.v->state().published_seen == 0);
    // ...yet the world already says B on both keys.
    CHECK(rig.doc_path() == "B");
    CHECK(rig.view_shown() == "B");
}

TEST_CASE("J10: the operator is told when the bus ends its operation, and a stranger is not") {
    SUBCASE("a bound participant removed: one notice, naming the operation and the reason") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        CHECK(rig.o->ended.empty());
        (void)rig.bus.unregister_weave(rig.doc);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->ended.size() == 1);
        CHECK(rig.o->ended[0].ended_op() == id);
        CHECK(rig.o->ended[0].reason == std::string(name_of(JointRefusal::ParticipantChanged)));
        CHECK_FALSE(rig.o->ended_attested[0]);
        // THE RECORD IS THE FACT, re-read from inside the delivery.
        CHECK(rig.o->ended_status[0].state == JointState::Aborted);
        CHECK(rig.o->ended_status[0].reason == JointRefusal::ParticipantChanged);
        CHECK(rig.bus.joint_pending() == 0);
        CHECK(rig.bus.joint_retained_bytes() == 0);
    }
    SUBCASE("a bound claim moved under an ordinary claim: no notice -- the owner answers; then its removal tells the operator once, with the record's reason") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"claim", 0, "A", 3}); // the document owner's own edit
        rig.bus.drain_until_idle();
        CHECK(rig.o->ended.empty());
        CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
        // The operator meets the abort at its next verb, as the record says.
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->committed.ok);
        CHECK(rig.o->committed.why == JointRefusal::WrongState);
        // ...AND IF THAT OWNER IS THEN REMOVED, the operator is told -- the reply it might
        // have been waiting on cannot come -- once, with the reason the record holds.
        (void)rig.bus.unregister_weave(rig.doc);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->ended.size() == 1);
        CHECK(rig.o->ended[0].ended_op() == id);
        CHECK(rig.o->ended[0].reason == std::string(name_of(JointRefusal::StaleRevision)));
        (void)rig.bus.unregister_weave(rig.view);
        rig.bus.drain_until_idle();
        CHECK(rig.o->ended.size() == 1); // once
    }
    SUBCASE("an operator's own cancellation and a commitment are its own acts: no notice") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK(rig.o->cancelled.ok);
        CHECK(rig.o->ended.empty());
        const std::uint64_t again = rig.prepare("C");
        rig.commit(again);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        CHECK(rig.o->ended.empty());
    }
    SUBCASE("a forged notice makes the operator consult a record that says Preparing") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        JointEnded forged;
        forged.op = std::to_string(id);
        forged.reason = name_of(JointRefusal::ParticipantChanged);
        (void)rig.bus.send(rig.op, Message(to_value(forged)));
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->ended.size() == 1);
        CHECK(rig.o->ended_status[0].state == JointState::Preparing); // the record, not the words
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok); // ...and the operation was never ended by it
    }
}

// ---- Publication is not application (SENSE-06) -------------------------------------------
//
// Two counterexamples shaped these cases: an application that failed inside the showing
// hook vanished -- the loaded status discarded, the mark cleared before the hook ran -- and
// a substrate mutation door bypassed the participant's end-of-delivery hook, so a written
// state kept an old claim and a stale preparation committed over it. What each case pins:
//
//   J11 loaded failure     a real image whose hook fails through the ABI is HELD: the
//                          ordinary snapshot refuses, a diagnostic read shows it as it
//                          is, the hook is not retried, deliveries are refused
//                          ApplicationFailed by exact attempt, the record names the
//                          participant and the operation, the operator is told once,
//                          unrelated work goes on, and a reload is the repair
//   J12 native parity      the same contract for a native owner: the first observer
//                          gets the exception after the record says Failed, the bus is
//                          not poisoned, and a native swap repairs it
//   J13 several at once    two publications under one weave: the first failure holds it,
//                          the second stays Pending unattempted, the successor is shown
//                          both
//   J14 removal            a claimant removed unshown is Lost, named, told once
//   J15 mutation doors     a PokeWrite or PokeResetState that changed exposed state
//                          reaches the claim through after_delivery; reads, describes
//                          and refused writes/resets change nothing and invalidate nothing
//   J16 loaded parity      J15's write and its controls through the ABI

/// A weave standing behind TWO claims, so two publications can be pending under one
/// weave at once; armed, its DocFact showing counts and fails before applying.
struct BothState {
    std::string path = "A";
    std::string shown = "hidden";
    std::int64_t doc_seen = 0;
    std::int64_t view_seen = 0;
    ZEN_SHAPE(BothState, 1, ZEN_FIELD(path), ZEN_FIELD(shown), ZEN_FIELD(doc_seen),
              ZEN_FIELD(view_seen));
};

class Both
    : public WeaveBase<Both, BothState, Accept<Cmd>, Emit<>, Claims<DocFact, ViewFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        ++deliveries;
        if (c.verb == "claim") {
            doc_claim = mail.claim(DocFact{state_.path, 1});
            view_claim = mail.claim(ViewFact{state_.shown, false});
        } else if (c.verb == "offer-doc") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), DocFact{c.path, 2});
        } else if (c.verb == "offer-view") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), ViewFact{c.path, true});
        } else if (c.verb == "arm-fail") {
            fail_doc = true;
        }
    }
    void on_claim_published(const DocFact& published) {
        ++state_.doc_seen; // a side effect BEFORE the failure: what a partial application is
        if (fail_doc) {
            fail_doc = false;
            throw std::runtime_error("the two-claim owner could not apply its document");
        }
        state_.path = published.path;
    }
    void on_claim_published(const ViewFact& published) {
        ++state_.view_seen;
        state_.shown = published.shown;
    }
    SenseClaimResult doc_claim{};
    SenseClaimResult view_claim{};
    JointResult last{};
    bool fail_doc = false;
    int deliveries = 0;
    const BothState& state() const { return state_; }
};

/// A Mirror whose state is only PARTLY exposed: `path` is writable, `hook_runs` is not,
/// so a reset of the whole state is refused while a write of the path is performed.
struct MirrorPartialState {
    std::string path = "A";
    std::int64_t epoch = 1;
    std::int64_t hook_runs = 0;
    ZEN_SHAPE(MirrorPartialState, 1, ZEN_EXPOSE(path), ZEN_EXPOSE(epoch), ZEN_FIELD(hook_runs));
};

class MirrorPartial
    : public WeaveBase<MirrorPartial, MirrorPartialState, Accept<Cmd>, Emit<>, Claims<DocFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        if (c.verb == "claim") {
            state_.path = c.path;
            state_.epoch = c.epoch;
            last_claim = mail.claim(DocFact{state_.path, state_.epoch});
            if (last_claim.accepted) {
                claimed_path = state_.path;
                claimed_epoch = state_.epoch;
            }
        } else if (c.verb == "offer") {
            last = mail.offer(static_cast<std::uint64_t>(c.op), DocFact{c.path, c.epoch});
        }
    }
    void on_claim_published(const DocFact& published) {
        state_.path = published.path;
        state_.epoch = published.epoch;
        claimed_path = state_.path;
        claimed_epoch = state_.epoch;
    }
    void after_delivery(Mail& mail) {
        ++state_.hook_runs;
        if (state_.path != claimed_path || state_.epoch != claimed_epoch) {
            last_claim = mail.claim(DocFact{state_.path, state_.epoch});
            if (last_claim.accepted) {
                claimed_path = state_.path;
                claimed_epoch = state_.epoch;
            }
        }
    }
    JointResult last{};
    SenseClaimResult last_claim{};
    std::string claimed_path = "A";
    std::int64_t claimed_epoch = 1;
    const MirrorPartialState& state() const { return state_; }
};

/// The path a claim says, read off the bus for any claimant.
std::string claimed_path_of(Switchboard& bus, WeaveId who) {
    const SenseReading d = bus.observe(who, DocFact::zen_name, DocFact::zen_version);
    return d ? from_value<DocFact>(*d.value).path : std::string("?");
}

/// The loaded probe's state through the ORDINARY snapshot -- which shows it a pending
/// publication first and refuses a held weave -- and through the DIAGNOSTIC read.
ProbeState probe_state(Switchboard& bus, WeaveId who,
                       Switchboard::SnapshotAccess access = Switchboard::SnapshotAccess::Ordinary) {
    const Admission a = admit(parse(bus.snapshot_bytes(who, access)), schema_of<ProbeState>());
    REQUIRE(a.ok());
    return from_value<ProbeState>(a.value());
}

TEST_CASE("J11: a loaded participant whose showing fails through the ABI is held, attributable, told once, and repaired by a reload") {
    Rig rig;
    Kernel kernel(rig.bus);
    const LoadResult loaded = kernel.load("joint-doc", ZEN_SO_JOINT, "joint.loaded");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.loaded", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(loaded.id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    REQUIRE(claimed_path_of(rig.bus, loaded.id) == "A");
    // PREPARE AND COMMIT B, NORMALLY -- the image armed to fail its next showing.
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t id = rig.o->op();
    tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
    tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), "B", 1});
    tell(rig.bus, loaded.id, Cmd{"arm-fail"});
    rig.bus.drain_until_idle();
    REQUIRE(rig.v->last.ok);
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    // PUBLISHED, NOT APPLIED: the claims say B; the application is owed.
    CHECK(claimed_path_of(rig.bus, loaded.id) == "B");
    CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
    CHECK(rig.bus.has_unobserved_publication(loaded.id));
    CHECK(rig.o->applied.empty());
    // THE SHOWING, through the real snapshot path: the hook counts its attempt across the
    // seam and fails; the status crosses back; the snapshot is REFUSED, not served.
    CHECK_THROWS_AS((void)rig.bus.snapshot_bytes(loaded.id), ApplicationFailedError);
    // THE RECORD: this participant, this operation, held.
    CHECK(rig.bus.has_failed_application(loaded.id));
    CHECK_FALSE(rig.bus.has_unobserved_publication(loaded.id)); // Failed is not Pending
    CHECK(rig.bus.application_of(loaded.id) == JointApplication::Failed);
    {
        const JointStatus s = rig.bus.joint_status(id);
        CHECK(s.state == JointState::Committed);
        CHECK(s.application == JointApplication::Failed);
        CHECK(s.failed == loaded.id);
        CHECK(s.failed_role == "joint.loaded");
    }
    CHECK(claimed_path_of(rig.bus, loaded.id) == "B"); // the commitment stands
    {
        const SenseReading d = rig.bus.observe(loaded.id, DocFact::zen_name, DocFact::zen_version);
        CHECK(d.by.author == loaded.id);
        CHECK(d.by.author_incarnation_is_current); // the incarnation that failed is this one
    }
    // THE OPERATOR IS TOLD, ONCE, naming the participant and the office it was bound by;
    // it re-reads the record, which is the fact.
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->applied.size() == 1);
    CHECK_FALSE(rig.o->applied[0].applied);
    CHECK(rig.o->applied[0].applied_op() == id);
    CHECK(rig.o->applied[0].failed_claimant() == loaded.id);
    CHECK(rig.o->applied[0].role == "joint.loaded");
    CHECK(rig.o->applied[0].reason == std::string(name_of(JointApplication::Failed)));
    CHECK(rig.o->applied_status[0].application == JointApplication::Failed);
    // REPEATED OBSERVATION: refused again, and the hook is NOT retried -- the diagnostic
    // read, which runs nothing, shows one attempt and the state the image is holding.
    CHECK_THROWS_AS((void)rig.bus.snapshot_bytes(loaded.id), ApplicationFailedError);
    {
        const ProbeState as_is =
            probe_state(rig.bus, loaded.id, Switchboard::SnapshotAccess::Diagnostic);
        CHECK(as_is.path == "A");           // never applied
        CHECK(as_is.published_seen == 1);   // one attempt, no retry
        CHECK(rig.bus.has_failed_application(loaded.id)); // the read changed nothing
    }
    // A DELIVERY TO THE HELD WEAVE IS REFUSED, journaled, and returned to a sender that
    // accepts dispatch refusals by EXACT ATTEMPT -- the handler never runs.
    const std::int64_t deliveries_before =
        probe_state(rig.bus, loaded.id, Switchboard::SnapshotAccess::Diagnostic).deliveries;
    tell(rig.bus, rig.op, Cmd{"nudge", static_cast<std::int64_t>(loaded.id.value)});
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->last_send.valid());
    {
        const DeliveryOutcome fate = rig.bus.outcome(rig.o->last_send);
        CHECK(fate.disposition == Disposition::Refused);
        CHECK(fate.refusal.reason == RefusalReason::ApplicationFailed);
    }
    REQUIRE(rig.o->refused.size() == 1);
    CHECK(rig.o->refused[0].refused_attempt().seq == rig.o->last_send.seq);
    CHECK(rig.o->refused[0].reason == std::string(name_of(RefusalReason::ApplicationFailed)));
    CHECK(probe_state(rig.bus, loaded.id, Switchboard::SnapshotAccess::Diagnostic).deliveries ==
          deliveries_before);
    CHECK(rig.o->applied.size() == 1); // said once
    // UNRELATED WORK IS NOT HELD: the view claims and is read; the operator begins
    // another operation over other keys.
    tell(rig.bus, rig.view, Cmd{"claim", 0, "V", 0});
    rig.bus.drain_until_idle();
    CHECK(rig.view_shown() == "V");
    // THE REPAIR: a real reload through the Kernel. The reload's read is not refused;
    // the successor is owed the showing (Pending again), and its own attempt applies.
    const std::filesystem::path copy = std::filesystem::temp_directory_path() / "zen-joint-held.so";
    std::filesystem::copy_file(ZEN_SO_JOINT, copy, std::filesystem::copy_options::overwrite_existing);
    const ReloadResult r = kernel.reload_from("joint-doc", copy.string());
    REQUIRE_MESSAGE(r.reloaded, r.error);
    CHECK_FALSE(rig.bus.has_failed_application(loaded.id));
    CHECK(rig.bus.has_unobserved_publication(loaded.id));
    CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
    tell(rig.bus, loaded.id, Cmd{"note"});
    rig.bus.drain_until_idle();
    {
        const ProbeState s = probe_state(rig.bus, loaded.id);
        CHECK(s.path == "B");
        CHECK(s.seen_at_delivery == "B");
        CHECK(s.published_seen == 2); // the predecessor's failed attempt rode; the successor's applied
    }
    CHECK(rig.bus.application_of(loaded.id) == JointApplication::Applied);
    // ...and once the view has been shown as well, the operation is Applied and the
    // operator is told again: a repair that re-settles is news.
    tell(rig.bus, rig.view, Cmd{"note"});
    rig.bus.drain_until_idle();
    CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
    REQUIRE(rig.o->applied.size() == 2);
    CHECK(rig.o->applied[1].applied);
    CHECK(rig.o->applied_status[1].application == JointApplication::Applied);
    REQUIRE(kernel.unload("joint-doc")); // unload first: a mapped image cannot be deleted on Windows
    std::error_code ec;
    std::filesystem::remove(copy, ec);
}

TEST_CASE("J12: a native participant whose showing throws is recorded, then re-raised; held the same way; repaired by a swap") {
    SUBCASE("first observed by a snapshot") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"arm-fail"});
        rig.bus.drain_until_idle();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        REQUIRE(rig.bus.joint_status(id).application == JointApplication::Pending);
        // THE FIRST OBSERVER HEARS THE WEAVE'S OWN EXCEPTION -- re-raised after the record
        // says what happened, MSG-10's discipline: recorded, never swallowed.
        bool heard = false;
        try {
            (void)rig.bus.snapshot_bytes(rig.doc);
        } catch (const ApplicationFailedError&) {
            FAIL("the bus's own refusal replaced the weave's exception at the showing that failed");
        } catch (const std::runtime_error& e) {
            heard = std::string(e.what()).find("document owner") != std::string::npos;
        }
        CHECK(heard);
        CHECK(rig.d->state().published_seen == 1);
        CHECK(rig.d->state().path == "A");
        CHECK(rig.bus.has_failed_application(rig.doc));
        {
            const JointStatus s = rig.bus.joint_status(id);
            CHECK(s.application == JointApplication::Failed);
            CHECK(s.failed == rig.doc);
            CHECK(s.failed_role == "joint.doc");
        }
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->applied.size() == 1);
        CHECK_FALSE(rig.o->applied[0].applied);
        CHECK(rig.o->applied[0].failed_claimant() == rig.doc);
        // HELD: the second observation is the bus's refusal, the hook is not retried, and
        // a delivery is refused without any exception reaching the host.
        CHECK_THROWS_AS((void)rig.bus.snapshot_bytes(rig.doc), ApplicationFailedError);
        CHECK(rig.d->state().published_seen == 1);
        const std::int64_t deliveries = rig.d->state().deliveries;
        const Ticket t = rig.bus.send(rig.doc, Message(to_value(Cmd{"note"})));
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.bus.outcome(t).disposition == Disposition::Refused);
        CHECK(rig.bus.outcome(t).refusal.reason == RefusalReason::ApplicationFailed);
        CHECK(rig.d->state().deliveries == deliveries);
        CHECK(rig.doc_path() == "B"); // the commitment stands
        // THE DIAGNOSTIC READ shows what it holds and changes nothing.
        {
            const Admission a = admit(parse(rig.bus.snapshot_bytes(
                                          rig.doc, Switchboard::SnapshotAccess::Diagnostic)),
                                      schema_of<ProbeState>());
            REQUIRE(a.ok());
            CHECK(from_value<ProbeState>(a.value()).path == "A");
            CHECK(rig.bus.has_failed_application(rig.doc));
        }
        // THE REPAIR, natively: the reload's read is not refused, the swap returns the
        // key to Pending, and the successor (the same object, new incarnation) applies.
        std::string bytes;
        CHECK_NOTHROW(bytes = rig.bus.snapshot_bytes(rig.doc, Switchboard::SnapshotAccess::Reload));
        REQUIRE(rig.bus.swap_state(rig.doc, bytes).revived);
        CHECK_FALSE(rig.bus.has_failed_application(rig.doc));
        CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
        tell(rig.bus, rig.doc, Cmd{"note"});
        tell(rig.bus, rig.view, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.d->state().path == "B");
        CHECK(rig.d->state().published_seen == 2);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
        REQUIRE(rig.o->applied.size() == 2);
        CHECK(rig.o->applied[1].applied);
    }
    SUBCASE("first observed by a delivery: the delivery is refused, the exception reaches the host, and the bus is not poisoned") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"arm-fail"});
        rig.bus.drain_until_idle();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        const std::int64_t deliveries = rig.d->state().deliveries;
        const Ticket t = rig.bus.send(rig.doc, Message(to_value(Cmd{"note"})));
        CHECK_THROWS_AS(rig.bus.drain_until_idle(), std::runtime_error);
        CHECK(rig.bus.outcome(t).disposition == Disposition::Refused);
        CHECK(rig.bus.outcome(t).refusal.reason == RefusalReason::ApplicationFailed);
        CHECK(rig.d->state().deliveries == deliveries); // the handler never ran
        CHECK(rig.bus.has_failed_application(rig.doc));
        CHECK(rig.bus.joint_status(id).application == JointApplication::Failed);
        // NOT POISONED: the next turn delivers, and the operator's notice arrives in it.
        tell(rig.bus, rig.view, Cmd{"claim", 0, "V", 0});
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.view_shown() == "V");
        REQUIRE(rig.o->applied.size() == 1);
        CHECK_FALSE(rig.o->applied[0].applied);
    }
}

TEST_CASE("J13: several publications under one weave -- the first failure holds it, the rest stay pending unattempted, and the successor is shown all of them") {
    Rig rig;
    auto [both_id, both] = put<Both>(rig.bus, Grant{}, "joint.both");
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.both"});
    rig.o->keys = {claim_key<DocFact>(both_id), claim_key<ViewFact>(both_id)};
    tell(rig.bus, both_id, Cmd{"claim"});
    rig.bus.drain_until_idle();
    REQUIRE(both->doc_claim.accepted);
    REQUIRE(both->view_claim.accepted);
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t id = rig.o->op();
    tell(rig.bus, both_id, Cmd{"offer-doc", static_cast<std::int64_t>(id), "B", 0});
    tell(rig.bus, both_id, Cmd{"offer-view", static_cast<std::int64_t>(id), "B", 0});
    tell(rig.bus, both_id, Cmd{"arm-fail"});
    rig.bus.drain_until_idle();
    REQUIRE(both->last.ok);
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
    // THE SHOWING: the document (first by key) fails after its side effect; the view is
    // never attempted -- Pending still, nothing of it partial; the weave is held.
    const Ticket t = rig.bus.send(both_id, Message(to_value(Cmd{"note"})));
    CHECK_THROWS_AS(rig.bus.drain_until_idle(), std::runtime_error);
    CHECK(rig.bus.outcome(t).refusal.reason == RefusalReason::ApplicationFailed);
    CHECK(both->state().doc_seen == 1);
    CHECK(both->state().view_seen == 0);
    CHECK(both->state().path == "A");
    CHECK(both->state().shown == "hidden");
    CHECK(rig.bus.has_failed_application(both_id));
    CHECK(rig.bus.has_unobserved_publication(both_id)); // the second is still owed
    CHECK(rig.bus.application_of(both_id) == JointApplication::Failed);
    {
        const JointStatus s = rig.bus.joint_status(id);
        CHECK(s.application == JointApplication::Failed);
        CHECK(s.failed == both_id);
    }
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->applied.size() == 1);
    CHECK_FALSE(rig.o->applied[0].applied);
    // NOTHING IS RETRIED while held: a second delivery is refused and the counters stand.
    const Ticket again = rig.bus.send(both_id, Message(to_value(Cmd{"note"})));
    CHECK_NOTHROW(rig.bus.drain_until_idle());
    CHECK(rig.bus.outcome(again).refusal.reason == RefusalReason::ApplicationFailed);
    CHECK(both->state().doc_seen == 1);
    CHECK(both->state().view_seen == 0);
    // THE SUCCESSOR IS SHOWN BOTH: the failed key returns to Pending, the pending one
    // was never touched, and one delivery applies the two.
    const std::string bytes = rig.bus.snapshot_bytes(both_id, Switchboard::SnapshotAccess::Reload);
    REQUIRE(rig.bus.swap_state(both_id, bytes).revived);
    CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
    tell(rig.bus, both_id, Cmd{"note"});
    rig.bus.drain_until_idle();
    CHECK(both->state().path == "B");
    CHECK(both->state().shown == "B");
    CHECK(both->state().doc_seen == 2);
    CHECK(both->state().view_seen == 1);
    CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
    REQUIRE(rig.o->applied.size() == 2);
    CHECK(rig.o->applied[1].applied);
}

TEST_CASE("J14: a claimant removed before it was shown is Lost -- named, told once, and the rest of the operation still settles") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    const std::size_t claims_before = rig.bus.retained_claim_count();
    std::unique_ptr<Weave> gone = rig.bus.unregister_weave(rig.doc);
    REQUIRE(gone != nullptr);
    rig.d = nullptr;
    CHECK(rig.bus.retained_claim_count() == claims_before - 1);
    {
        const JointStatus s = rig.bus.joint_status(id);
        CHECK(s.state == JointState::Committed);
        CHECK(s.application == JointApplication::Lost);
        CHECK(s.failed == rig.doc);
        CHECK(s.failed_role == "joint.doc");
    }
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->applied.size() == 1);
    CHECK_FALSE(rig.o->applied[0].applied);
    CHECK(rig.o->applied[0].reason == std::string(name_of(JointApplication::Lost)));
    CHECK(rig.o->applied[0].failed_claimant() == rig.doc);
    CHECK(rig.o->applied[0].role == "joint.doc");
    // THE VIEW IS STILL OWED ITS SHOWING, and applies at its next delivery; the
    // operation's word stays Lost (the worst part decides), said once.
    CHECK(rig.bus.has_unobserved_publication(rig.view));
    tell(rig.bus, rig.view, Cmd{"note"});
    rig.bus.drain_until_idle();
    CHECK(rig.v->state().published_seen == 1);
    CHECK(rig.v->state().shown == "B");
    CHECK(rig.bus.joint_status(id).application == JointApplication::Lost);
    CHECK(rig.o->applied.size() == 1);
    // ...AND A FRESH OPERATION over a fresh document owner works as ever.
    auto [fresh_id, fresh] = put<Doc>(rig.bus, Grant{}, "joint.doc");
    tell(rig.bus, fresh_id, Cmd{"claim", 0, "F", 1});
    rig.bus.drain_until_idle();
    REQUIRE(fresh->last_claim.accepted);
    rig.o->keys = {claim_key<DocFact>(fresh_id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t next = rig.o->op();
    tell(rig.bus, fresh_id, Cmd{"offer", static_cast<std::int64_t>(next), "G", 2});
    tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(next), "G", 1});
    rig.commit(next);
    rig.bus.drain_until_idle();
    CHECK(rig.o->committed.ok);
    CHECK(claimed_path_of(rig.bus, fresh_id) == "G");
}

TEST_CASE("J15: a substrate mutation door that changed exposed state reaches the claim, and the doors that changed nothing invalidate nothing (native)") {
    Rig rig;
    auto [mirror_id, mirror] = put<Mirror>(rig.bus, Grant{}, "joint.mirror");
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.mirror", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(mirror_id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, mirror_id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    REQUIRE(mirror->last_claim.accepted);
    const auto prepare_mirror = [&](const std::string& next) {
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
        const std::uint64_t id = rig.o->op();
        tell(rig.bus, mirror_id, Cmd{"offer", static_cast<std::int64_t>(id), next, 2});
        tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), next, 1});
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(mirror->last.ok, name_of(mirror->last.why));
        REQUIRE(rig.v->last.ok);
        return id;
    };
    const auto poke = [&](const Value& what) {
        (void)rig.bus.send(mirror_id, Message(what));
        rig.bus.drain_until_idle();
    };
    SUBCASE("a write that is performed: the claim follows, the stale preparation is over, the state stands") {
        const std::uint64_t id = prepare_mirror("B");
        const std::int64_t hooks = mirror->state().hook_runs;
        poke(to_value(PokeWrite{"path", "C"}));
        CHECK(mirror->state().path == "C");
        CHECK(mirror->state().hook_runs == hooks + 1);
        CHECK(claimed_path_of(rig.bus, mirror_id) == "C");
        CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
        CHECK(rig.bus.joint_status(id).reason == JointRefusal::StaleRevision);
        CHECK(rig.bus.joint_retained_bytes() == 0);
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->committed.ok);
        CHECK(rig.o->committed.why == JointRefusal::WrongState);
        CHECK(claimed_path_of(rig.bus, mirror_id) == "C");
        CHECK(rig.view_shown() == "hidden");
        const Admission a = admit(parse(rig.bus.snapshot_bytes(mirror_id)), schema_of<MirrorState>());
        REQUIRE(a.ok());
        CHECK(from_value<MirrorState>(a.value()).path == "C");
        CHECK(mirror->state().path == "C");
    }
    SUBCASE("a read, a describe, a refused write and a bad literal change nothing and invalidate nothing") {
        const std::uint64_t id = prepare_mirror("B");
        const std::int64_t hooks = mirror->state().hook_runs;
        const std::uint64_t revision = rig.doc_revision(); // the view's, untouched control
        poke(to_value(PokeRead{"path"}));
        poke(to_value(PokeDescribe{}));
        poke(to_value(PokeWrite{"epoch", "not-a-number"}));
        poke(to_value(PokeWrite{"nowhere", "C"}));
        CHECK(mirror->state().path == "A");
        CHECK(mirror->state().epoch == 1);
        CHECK(mirror->state().hook_runs == hooks);
        CHECK(claimed_path_of(rig.bus, mirror_id) == "A");
        CHECK(rig.bus.joint_status(id).state == JointState::Preparing);
        CHECK(rig.doc_revision() == revision);
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok);
        CHECK(claimed_path_of(rig.bus, mirror_id) == "B");
        tell(rig.bus, mirror_id, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(mirror->state().path == "B");
    }
    SUBCASE("a reset that is performed reaches the claim; a reset refused by a partly exposed state does not") {
        // The fully exposed Mirror: after B is published and shown, a reset restores the
        // defaults, and the claim follows them.
        const std::uint64_t id = prepare_mirror("B");
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        tell(rig.bus, mirror_id, Cmd{"note"});
        rig.bus.drain_until_idle();
        REQUIRE(mirror->state().path == "B");
        REQUIRE(claimed_path_of(rig.bus, mirror_id) == "B");
        const std::int64_t hooks = mirror->state().hook_runs;
        poke(to_value(PokeResetState{}));
        CHECK(mirror->state().path == "A");
        CHECK(mirror->state().hook_runs == 1); // reset to the default, then this run counted
        CHECK(hooks > 0);
        CHECK(claimed_path_of(rig.bus, mirror_id) == "A");
        // The partly exposed one: the reset is refused, so nothing moves and a preparation
        // bound to it commits.
        auto [partial_id, partial] = put<MirrorPartial>(rig.bus, Grant{}, "joint.partial");
        rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.partial", "joint.view"});
        rig.o->keys = {claim_key<DocFact>(partial_id), claim_key<ViewFact>(rig.view)};
        tell(rig.bus, partial_id, Cmd{"claim", 0, "A", 1});
        rig.bus.drain_until_idle();
        REQUIRE(partial->last_claim.accepted);
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
        const std::uint64_t again = rig.o->op();
        tell(rig.bus, partial_id, Cmd{"offer", static_cast<std::int64_t>(again), "B", 2});
        tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(again), "B", 1});
        rig.bus.drain_until_idle();
        REQUIRE(partial->last.ok);
        const std::int64_t partial_hooks = partial->state().hook_runs;
        (void)rig.bus.send(partial_id, Message(to_value(PokeResetState{})));
        rig.bus.drain_until_idle();
        CHECK(partial->state().path == "A");
        CHECK(partial->state().hook_runs == partial_hooks);
        CHECK(claimed_path_of(rig.bus, partial_id) == "A");
        CHECK(rig.bus.joint_status(again).state == JointState::Preparing);
        rig.commit(again);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok);
        CHECK(claimed_path_of(rig.bus, partial_id) == "B");
    }
}

TEST_CASE("J16: the same doors through the ABI -- a loaded participant's performed write reaches its claim, and its reads and refused writes do not") {
    Rig rig;
    Kernel kernel(rig.bus);
    const LoadResult loaded = kernel.load("joint-doc", ZEN_SO_JOINT, "joint.loaded");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.loaded", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(loaded.id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    REQUIRE(probe_state(rig.bus, loaded.id).last_offer_ok == 1);
    const auto prepare_loaded = [&] {
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
        const std::uint64_t id = rig.o->op();
        tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
        tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), "B", 1});
        rig.bus.drain_until_idle();
        REQUIRE(probe_state(rig.bus, loaded.id).last_offer_ok == 1);
        REQUIRE(rig.v->last.ok);
        return id;
    };
    const auto poke = [&](const Value& what) {
        (void)rig.bus.send(loaded.id, Message(what));
        rig.bus.drain_until_idle();
    };
    SUBCASE("a performed write") {
        const std::uint64_t id = prepare_loaded();
        const std::int64_t hooks = probe_state(rig.bus, loaded.id).hook_runs;
        poke(to_value(PokeWrite{"path", "C"}));
        CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
        CHECK(rig.bus.joint_status(id).reason == JointRefusal::StaleRevision);
        CHECK(claimed_path_of(rig.bus, loaded.id) == "C");
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->committed.ok);
        const ProbeState s = probe_state(rig.bus, loaded.id);
        CHECK(s.path == "C");
        CHECK(s.hook_runs == hooks + 1);
        CHECK(claimed_path_of(rig.bus, loaded.id) == "C");
    }
    SUBCASE("a read, a describe and a refused write") {
        const std::uint64_t id = prepare_loaded();
        const std::int64_t hooks = probe_state(rig.bus, loaded.id).hook_runs;
        poke(to_value(PokeRead{"path"}));
        poke(to_value(PokeDescribe{}));
        poke(to_value(PokeWrite{"epoch", "not-a-number"}));
        CHECK(rig.bus.joint_status(id).state == JointState::Preparing);
        CHECK(claimed_path_of(rig.bus, loaded.id) == "A");
        CHECK(probe_state(rig.bus, loaded.id).hook_runs == hooks);
        rig.commit(id);
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.ok);
        tell(rig.bus, loaded.id, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(probe_state(rig.bus, loaded.id).path == "B");
        CHECK(claimed_path_of(rig.bus, loaded.id) == "B");
    }
    REQUIRE(kernel.unload("joint-doc"));
}

// ---- Outcome retention ------------------------
//
// A committed operation's record is what its operator re-reads when the bus tells it the
// application settled (`zen.JointApplied`); an aborted operation's record is what it
// re-reads when told the operation ended (`zen.JointEnded`). An earlier form of this
// mechanism let `begin_joint_as` reuse any slot that was not Preparing, so one unrelated begin --
// under another operator, over other keys -- took the record out from under a legitimate,
// still-unconsumed outcome: the application settled against a record that had become Missing
// (schedule 1), or the notice arrived and re-read Missing (schedule 2). Publication is not
// the end of an outcome's lifetime, and a queued notification is not consumption.

/// A second, UNRELATED coordination on the same bus: its own document and view owners in
/// offices of their own, and its own operator with an authority over exactly those.
struct Unrelated {
    WeaveId doc{};
    Doc* d = nullptr;
    WeaveId view{};
    View* v = nullptr;
    WeaveId op{};
    Operator* o = nullptr;

    explicit Unrelated(Switchboard& bus) {
        std::tie(doc, d) = put<Doc>(bus, Grant{}, "joint.doc2");
        std::tie(view, v) = put<View>(bus, Grant{}, "joint.view2");
        std::tie(op, o) = put<Operator>(bus, Grant{}.allow_to_any(Cmd::zen_name, Cmd::zen_version),
                                       "joint.operator2");
        o->authority = bus.mint_joint_authority(op, {"joint.doc2", "joint.view2"});
        o->keys = {claim_key<DocFact>(doc), claim_key<ViewFact>(view)};
        tell(bus, doc, Cmd{"claim", 0, "A2", 1});
        tell(bus, view, Cmd{"claim", 0, "hidden2", 0});
        bus.drain_until_idle();
        REQUIRE(d->last_claim.accepted);
        REQUIRE(v->last_claim.accepted);
    }
};

TEST_CASE("J17: a committed publication's record outlives an unrelated coordination begun before its application was consumed") {
    SUBCASE("schedule 1: the unrelated operation begins before the owners are shown") {
        Rig rig;
        Unrelated other(rig.bus);
        const std::uint64_t id = rig.prepare();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        REQUIRE(rig.bus.joint_status(id).application == JointApplication::Pending);
        // ONE UNRELATED BEGIN, delivered: another operator, other keys, its own record.
        tell(rig.bus, other.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(other.o->begun.ok, name_of(other.o->begun.why));
        CHECK(other.o->op() != id);
        // THE LEGITIMATE RECORD STILL STANDS: committed, its application still owed.
        CHECK(rig.bus.joint_status(id).state == JointState::Committed);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
        // THE OWNERS ARE SHOWN, through their snapshot paths, and apply.
        (void)rig.bus.snapshot_bytes(rig.doc);
        (void)rig.bus.snapshot_bytes(rig.view);
        CHECK(rig.d->state().path == "B");
        CHECK(rig.v->state().shown == "B");
        // ...AND THE OPERATION SAYS SO, and its operator is told once and re-reads it.
        CHECK(rig.bus.joint_status(id).state == JointState::Committed);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->applied.size() == 1);
        CHECK(rig.o->applied[0].applied);
        CHECK(rig.o->applied[0].applied_op() == id);
        CHECK(rig.o->applied_status[0].state == JointState::Committed);
        CHECK(rig.o->applied_status[0].application == JointApplication::Applied);
        // THE UNRELATED OPERATION IS UNTOUCHED BY ANY OF IT.
        CHECK(rig.bus.joint_status(other.o->op()).state == JointState::Preparing);
        CHECK(other.o->applied.empty());
        CHECK(rig.bus.joint_records() == 2);
        // ...AND THE OUTCOME, CONSUMED, IS RELEASED BY ITS OPERATOR: Missing afterwards,
        // the slot free, the unrelated operation still exactly where it was.
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK(rig.o->released.ok);
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
        CHECK(rig.bus.joint_records() == 1);
        CHECK(rig.bus.joint_status(other.o->op()).state == JointState::Preparing);
    }
    SUBCASE("schedule 2: the unrelated operation begins while the application notice is queued") {
        Rig rig;
        Unrelated other(rig.bus);
        const std::uint64_t id = rig.prepare();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        // THE UNRELATED BEGIN IS QUEUED, NOT DELIVERED.
        tell(rig.bus, other.op, Cmd{"begin"});
        // THE OWNERS ARE SHOWN NOW: the application settles and the notice to operator 1 is
        // queued BEHIND the unrelated begin.
        (void)rig.bus.snapshot_bytes(rig.doc);
        (void)rig.bus.snapshot_bytes(rig.view);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
        // THE DRAIN: the begin runs first, then the notice is delivered.
        rig.bus.drain_until_idle();
        REQUIRE_MESSAGE(other.o->begun.ok, name_of(other.o->begun.why));
        REQUIRE(rig.o->applied.size() == 1);
        CHECK(rig.o->applied[0].applied_op() == id);
        // WHAT THE OPERATOR RE-READ INSIDE THAT DELIVERY IS THE FACT, and it is there.
        CHECK(rig.o->applied_status[0].state == JointState::Committed);
        CHECK(rig.o->applied_status[0].application == JointApplication::Applied);
        CHECK(rig.bus.joint_status(id).state == JointState::Committed);
        CHECK(rig.bus.joint_status(other.o->op()).state == JointState::Preparing);
    }
}

TEST_CASE("J18: an operation the bus ended keeps its record until its operator has consumed the notice, past an unrelated begin") {
    Rig rig;
    Unrelated other(rig.bus);
    const std::uint64_t id = rig.prepare();
    // THE UNRELATED BEGIN IS QUEUED; then a bound participant is removed, which ends the
    // operation and queues `zen.JointEnded` to its operator BEHIND that begin.
    tell(rig.bus, other.op, Cmd{"begin"});
    (void)rig.bus.unregister_weave(rig.doc);
    rig.d = nullptr;
    CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(other.o->begun.ok, name_of(other.o->begun.why));
    REQUIRE(rig.o->ended.size() == 1);
    CHECK(rig.o->ended[0].ended_op() == id);
    // THE RECORD THE NOTICE POINTS AT IS STILL THE BUS'S OWN WORD.
    CHECK(rig.o->ended_status[0].state == JointState::Aborted);
    CHECK(rig.o->ended_status[0].reason == JointRefusal::ParticipantChanged);
    CHECK(rig.bus.joint_status(id).state == JointState::Aborted);
    CHECK(rig.bus.joint_status(other.o->op()).state == JointState::Preparing);
    CHECK(rig.bus.joint_records() == 2);
    // CONSUMED, THEN RELEASED: what the operator learned is its to keep or retire.
    tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
    rig.bus.drain_until_idle();
    CHECK(rig.o->released.ok);
    CHECK(rig.bus.joint_status(id).state == JointState::Missing);
    CHECK(rig.bus.joint_records() == 1);
}

TEST_CASE("J19: an operator replaced, removed or dead leaves nobody to consume its records -- they are released, and the claimants keep every fact of their own") {
    SUBCASE("removed while its committed operation awaits application") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        REQUIRE(rig.bus.joint_records() == 1);
        std::unique_ptr<Weave> gone = rig.bus.unregister_weave(rig.op);
        REQUIRE(gone != nullptr);
        rig.o = nullptr;
        // RELEASED WITH ITS OPERATOR: Missing to the host, the slot free.
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
        CHECK(rig.bus.joint_records() == 0);
        // THE CLAIMANTS ARE STILL SHOWN, from their own claim records, and apply; nothing
        // is owed to anybody, and nothing throws.
        CHECK(rig.bus.has_unobserved_publication(rig.doc));
        tell(rig.bus, rig.doc, Cmd{"note"});
        tell(rig.bus, rig.view, Cmd{"note"});
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.d->state().path == "B");
        CHECK(rig.v->state().shown == "B");
        CHECK(rig.bus.application_of(rig.doc) == JointApplication::Applied);
        CHECK(rig.bus.application_of(rig.view) == JointApplication::Applied);
        CHECK(rig.doc_path() == "B");
    }
    SUBCASE("replaced by new code at the same address: the successor inherits nothing, a stale notice decides nothing, and a held claimant under the released record is still held and still repaired") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"arm-fail"});
        rig.bus.drain_until_idle();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        // The document owner fails its showing: held; the operator's notice is queued.
        CHECK_THROWS_AS((void)rig.bus.snapshot_bytes(rig.doc), std::runtime_error);
        REQUIRE(rig.bus.has_failed_application(rig.doc));
        REQUIRE(rig.bus.joint_status(id).application == JointApplication::Failed);
        // THE OPERATOR IS SWAPPED before that notice is delivered: same id, new incarnation.
        const JointAuthority retained = rig.o->authority; // the predecessor's, in the same member
        const std::string bytes = rig.bus.snapshot_bytes(rig.op);
        REQUIRE(rig.bus.swap_state(rig.op, bytes).revived);
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
        CHECK(rig.bus.joint_records() == 0);
        // THE HOST AUTHORIZES THE SUCCESSOR, explicitly: the predecessor's capability
        // names an incarnation that is gone (J24 pins every verb refusing it), and the
        // host, not the bus, decides that the new code may coordinate. This case's
        // subject is the RECORD's lifetime, so the successor holds current authority.
        rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.doc", "joint.view"});
        // THE STALE NOTICE reaches the successor and decides nothing: the record it
        // consults says Missing.
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->applied.size() == 1);
        CHECK(rig.o->applied_status[0].state == JointState::Missing);
        CHECK(rig.o->applied_status[0].reason == JointRefusal::NoSuchOperation);
        // THE SUCCESSOR'S OWN VERBS on the old id name nothing -- and with the retained
        // predecessor's capability they name nobody: the ungated host view is what says
        // the record is Missing, never a stale capability regaining a door for it.
        tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(id)});
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK(rig.o->status.state == JointState::Missing);
        CHECK(rig.o->released.why == JointRefusal::NoSuchOperation);
        {
            const JointAuthority current = rig.o->authority;
            rig.o->authority = retained;
            tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(id)});
            tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
            rig.bus.drain_until_idle();
            CHECK(rig.o->status.state == JointState::Missing);
            CHECK(rig.o->status.reason == JointRefusal::NotOperator);
            CHECK(rig.o->released.why == JointRefusal::NotOperator);
            CHECK(rig.bus.joint_status(id).state == JointState::Missing);
            rig.o->authority = current;
        }
        // THE HELD CLAIMANT never depended on the slot: still held, still refused, and
        // still repaired by its own swap -- shown again, applying, with no notice owed.
        CHECK(rig.bus.has_failed_application(rig.doc));
        const Ticket t = rig.bus.send(rig.doc, Message(to_value(Cmd{"note"})));
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.bus.outcome(t).refusal.reason == RefusalReason::ApplicationFailed);
        std::string doc_bytes;
        CHECK_NOTHROW(doc_bytes =
                          rig.bus.snapshot_bytes(rig.doc, Switchboard::SnapshotAccess::Reload));
        REQUIRE(rig.bus.swap_state(rig.doc, doc_bytes).revived);
        CHECK_FALSE(rig.bus.has_failed_application(rig.doc));
        CHECK(rig.bus.has_unobserved_publication(rig.doc));
        tell(rig.bus, rig.doc, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.d->state().path == "B");
        CHECK(rig.bus.application_of(rig.doc) == JointApplication::Applied);
        CHECK(rig.o->applied.size() == 1); // nothing more was owed: the record was released
        // ...and the successor operator begins a fresh operation as ever.
        const std::uint64_t fresh = rig.prepare("C");
        CHECK(rig.bus.joint_status(fresh).state == JointState::Preparing);
    }
    SUBCASE("dead: a life that ended owns no record") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->cancelled.ok);
        REQUIRE(rig.bus.joint_status(id).state == JointState::Aborted);
        rig.bus.kill(rig.op);
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
        CHECK(rig.bus.joint_records() == 0);
    }
}

TEST_CASE("J20: a native owner that DECLINES a published value is recorded Declined and not held -- told once, naming it -- and its own next claim replaces the value") {
    SUBCASE("declined by answer: the owner keeps its state, works on, and re-claims") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"arm-decline"});
        rig.bus.drain_until_idle();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        // THE SHOWING, at the next delivery: declined, and the handler RUNS in that same
        // delivery -- nothing is held, nothing is refused, nothing reaches the host.
        const Ticket t = rig.bus.send(rig.doc, Message(to_value(Cmd{"note"})));
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.bus.outcome(t).disposition == Disposition::Delivered);
        CHECK(rig.d->state().published_seen == 1);
        CHECK(rig.d->state().path == "A"); // kept its own
        CHECK(rig.d->state().seen_at_delivery == "A");
        CHECK_FALSE(rig.bus.has_failed_application(rig.doc));
        CHECK_FALSE(rig.bus.has_unobserved_publication(rig.doc));
        CHECK(rig.bus.application_of(rig.doc) == JointApplication::Declined);
        CHECK(rig.doc_path() == "B"); // the publication stands until the owner speaks
        // THE ORDINARY SNAPSHOT IS SERVED, shows the owner's own state, and re-shows nothing.
        {
            const Admission a =
                admit(parse(rig.bus.snapshot_bytes(rig.doc)), schema_of<ProbeState>());
            REQUIRE(a.ok());
            CHECK(from_value<ProbeState>(a.value()).path == "A");
        }
        CHECK(rig.d->state().published_seen == 1);
        // THE OPERATION SAYS DECLINED, NAMING THE OWNER -- the worst part decides, so it
        // settles now, before the view has even been shown -- and the operator is told once.
        {
            const JointStatus s = rig.bus.joint_status(id);
            CHECK(s.state == JointState::Committed);
            CHECK(s.application == JointApplication::Declined);
            CHECK(s.failed == rig.doc);
            CHECK(s.failed_role == "joint.doc");
        }
        REQUIRE(rig.o->applied.size() == 1);
        CHECK_FALSE(rig.o->applied[0].applied);
        CHECK(rig.o->applied[0].reason == std::string(name_of(JointApplication::Declined)));
        CHECK(rig.o->applied[0].failed_claimant() == rig.doc);
        CHECK(rig.o->applied[0].role == "joint.doc");
        CHECK(rig.o->applied_status[0].application == JointApplication::Declined);
        // THE VIEW APPLIES ITS HALF; the operation's word stays Declined, said once.
        tell(rig.bus, rig.view, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.v->state().shown == "B");
        CHECK(rig.bus.joint_status(id).application == JointApplication::Declined);
        CHECK(rig.o->applied.size() == 1);
        // THE OWNER SPEAKS: its ordinary claim replaces the published value; the claim
        // record owes nothing; the operation keeps its historical word.
        tell(rig.bus, rig.doc, Cmd{"claim", 0, "A", 3});
        rig.bus.drain_until_idle();
        CHECK(rig.doc_path() == "A");
        CHECK(rig.bus.application_of(rig.doc) == JointApplication::None);
        CHECK(rig.bus.joint_status(id).application == JointApplication::Declined);
        CHECK(rig.o->applied.size() == 1);
        // A RELOAD RE-SHOWS NOTHING: declined was an answer, not a failure to repair.
        const std::string bytes = rig.bus.snapshot_bytes(rig.doc);
        REQUIRE(rig.bus.swap_state(rig.doc, bytes).revived);
        CHECK_FALSE(rig.bus.has_unobserved_publication(rig.doc));
        tell(rig.bus, rig.doc, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.d->state().published_seen == 1);
        // ...AND THE OPERATOR RELEASES THE RECORD IT HAS CONSUMED.
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK(rig.o->released.ok);
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
    }
    SUBCASE("failed by answer, without an exception: held exactly as a throw holds, nothing reaches the host, repaired by a swap") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        tell(rig.bus, rig.doc, Cmd{"arm-fail-quietly"});
        rig.bus.drain_until_idle();
        rig.commit(id);
        rig.bus.drain_until_idle();
        REQUIRE(rig.o->committed.ok);
        const std::int64_t deliveries = rig.d->state().deliveries;
        const Ticket t = rig.bus.send(rig.doc, Message(to_value(Cmd{"note"})));
        CHECK_NOTHROW(rig.bus.drain_until_idle());
        CHECK(rig.bus.outcome(t).disposition == Disposition::Refused);
        CHECK(rig.bus.outcome(t).refusal.reason == RefusalReason::ApplicationFailed);
        CHECK(rig.d->state().deliveries == deliveries);
        CHECK(rig.d->state().published_seen == 1);
        CHECK(rig.bus.has_failed_application(rig.doc));
        CHECK(rig.bus.joint_status(id).application == JointApplication::Failed);
        CHECK_THROWS_AS((void)rig.bus.snapshot_bytes(rig.doc), ApplicationFailedError);
        REQUIRE(rig.o->applied.size() == 1);
        CHECK(rig.o->applied[0].reason == std::string(name_of(JointApplication::Failed)));
        // THE REPAIR: the reload's read is served, the swap returns the key to Pending,
        // and the successor applies -- told again.
        std::string bytes;
        CHECK_NOTHROW(bytes = rig.bus.snapshot_bytes(rig.doc, Switchboard::SnapshotAccess::Reload));
        REQUIRE(rig.bus.swap_state(rig.doc, bytes).revived);
        tell(rig.bus, rig.doc, Cmd{"note"});
        tell(rig.bus, rig.view, Cmd{"note"});
        rig.bus.drain_until_idle();
        CHECK(rig.d->state().path == "B");
        CHECK(rig.bus.joint_status(id).application == JointApplication::Applied);
        REQUIRE(rig.o->applied.size() == 2);
        CHECK(rig.o->applied[1].applied);
    }
}

TEST_CASE("J21: a loaded owner's declined showing crosses the seam as its own status -- recorded Declined, not held, its next claim replacing the value") {
    Rig rig;
    Kernel kernel(rig.bus);
    const LoadResult loaded = kernel.load("joint-doc", ZEN_SO_JOINT, "joint.loaded");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.loaded", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(loaded.id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    REQUIRE(claimed_path_of(rig.bus, loaded.id) == "A");
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t id = rig.o->op();
    tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
    tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), "B", 1});
    tell(rig.bus, loaded.id, Cmd{"arm-decline"});
    rig.bus.drain_until_idle();
    REQUIRE(rig.v->last.ok);
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    CHECK(claimed_path_of(rig.bus, loaded.id) == "B");
    // THE SHOWING, through the ordinary snapshot: the `bool` hook answers false across the
    // seam as ZEN_CLAIM_DECLINED; the snapshot is SERVED and shows the image's own state.
    const ProbeState s = probe_state(rig.bus, loaded.id);
    CHECK(s.path == "A");
    CHECK(s.published_seen == 1);
    CHECK_FALSE(rig.bus.has_failed_application(loaded.id));
    CHECK_FALSE(rig.bus.has_unobserved_publication(loaded.id));
    CHECK(rig.bus.application_of(loaded.id) == JointApplication::Declined);
    {
        const JointStatus st = rig.bus.joint_status(id);
        CHECK(st.state == JointState::Committed);
        CHECK(st.application == JointApplication::Declined);
        CHECK(st.failed == loaded.id);
        CHECK(st.failed_role == "joint.loaded");
    }
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->applied.size() == 1);
    CHECK_FALSE(rig.o->applied[0].applied);
    CHECK(rig.o->applied[0].reason == std::string(name_of(JointApplication::Declined)));
    CHECK(rig.o->applied[0].failed_claimant() == loaded.id);
    CHECK(rig.o->applied[0].role == "joint.loaded");
    // A DELIVERY PROCEEDS, and the hook is not re-run.
    const std::int64_t deliveries = s.deliveries;
    tell(rig.bus, loaded.id, Cmd{"note"});
    rig.bus.drain_until_idle();
    {
        const ProbeState after = probe_state(rig.bus, loaded.id);
        CHECK(after.deliveries == deliveries + 1);
        CHECK(after.published_seen == 1);
        CHECK(after.path == "A");
    }
    // THE OWNER SPEAKS: its ordinary claim replaces the published value.
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 3});
    rig.bus.drain_until_idle();
    CHECK(claimed_path_of(rig.bus, loaded.id) == "A");
    CHECK(rig.bus.application_of(loaded.id) == JointApplication::None);
    CHECK(rig.bus.joint_status(id).application == JointApplication::Declined);
    CHECK(rig.o->applied.size() == 1);
    REQUIRE(kernel.unload("joint-doc"));
}

TEST_CASE("J22: a refused replacement runs nothing of the incumbent's -- a v7 candidate is judged before the incumbent's snapshot, so a pending showing stays pending and the real reload still repairs") {
    // The candidate is judged (ABI version, descriptor, manifest) BEFORE the reload's
    // snapshot of the incumbent is taken (docs/reference/joint-publication.md#repair).
    // The observable is the showing: a snapshot SHOWS the weave its pending publication,
    // so a refused candidate that had been judged after it would leave a showing behind.
    Rig rig;
    Kernel kernel(rig.bus);
    const LoadResult loaded = kernel.load("joint-doc", ZEN_SO_JOINT, "joint.loaded");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.loaded", "joint.view"});
    rig.o->keys = {claim_key<DocFact>(loaded.id), claim_key<ViewFact>(rig.view)};
    tell(rig.bus, loaded.id, Cmd{"claim", 0, "A", 1});
    rig.bus.drain_until_idle();
    REQUIRE(claimed_path_of(rig.bus, loaded.id) == "A");
    tell(rig.bus, rig.op, Cmd{"begin"});
    rig.bus.drain_until_idle();
    REQUIRE_MESSAGE(rig.o->begun.ok, name_of(rig.o->begun.why));
    const std::uint64_t id = rig.o->op();
    tell(rig.bus, loaded.id, Cmd{"offer", static_cast<std::int64_t>(id), "B", 2});
    tell(rig.bus, rig.view, Cmd{"offer", static_cast<std::int64_t>(id), "B", 1});
    rig.bus.drain_until_idle();
    rig.commit(id);
    rig.bus.drain_until_idle();
    REQUIRE(rig.o->committed.ok);
    REQUIRE(rig.bus.has_unobserved_publication(loaded.id));
    // THE REFUSED REPLACEMENT: a current-layout image claiming the previous ABI version.
    const ReloadResult rr = kernel.reload_from("joint-doc", ZEN_SO_STALEABI);
    CHECK_FALSE(rr.ok);
    CHECK(rr.error.find("abi_version") != std::string::npos);
    CHECK(rr.error.find(std::to_string(ZEN_ABI_VERSION - 1u)) != std::string::npos);
    // NOTHING OF THE INCUMBENT'S RAN: still Pending, no attempt counted, still bound to
    // the same incarnation (the operation is intact, not aborted by a swap that never was).
    CHECK(rig.bus.has_unobserved_publication(loaded.id));
    CHECK(probe_state(rig.bus, loaded.id, Switchboard::SnapshotAccess::Diagnostic).published_seen == 0);
    CHECK(rig.bus.joint_status(id).state == JointState::Committed);
    CHECK(rig.bus.joint_status(id).application == JointApplication::Pending);
    CHECK(kernel.weave_id("joint-doc") == loaded.id);
    // ...AND A REAL RELOAD STILL DOES ITS WORK: the incumbent is shown at the reload's
    // read, applies, and the successor revives carrying B.
    const std::filesystem::path copy = std::filesystem::temp_directory_path() / "zen-joint-j22.so";
    std::filesystem::copy_file(ZEN_SO_JOINT, copy, std::filesystem::copy_options::overwrite_existing);
    const ReloadResult ok = kernel.reload_from("joint-doc", copy.string());
    REQUIRE_MESSAGE(ok.reloaded, ok.error);
    CHECK_FALSE(rig.bus.has_unobserved_publication(loaded.id));
    CHECK(probe_state(rig.bus, loaded.id).path == "B");
    CHECK(probe_state(rig.bus, loaded.id).published_seen == 1);
    REQUIRE(kernel.unload("joint-doc")); // unload first: a mapped image cannot be deleted on Windows
    std::error_code ec;
    std::filesystem::remove(copy, ec);
}

// ---- J23-J24: authority is exact, and reaches only its holder's own records ----------
//
// Two defects a review reproduced against the landed candidate, both present since the
// experiment: a foreign operator's refused commit ABORTED the record it named (the
// refusal was checked by its return value, not by what it changed), and a capability
// that named only the operator's address survived the operator's swap. The lessons live
// in the reference page's authority section and on `JointAuthority`; these two cases are
// the proof. J6's copied capability (refused at the authority, never reaching a record)
// and J19's operator lifetime (the record retired) are neighbouring predicates, not these.

TEST_CASE("J23: a foreign operator's valid authority reaches none of another operator's operation -- refused NotOperator before the record is touched, and the owner still commits") {
    Rig rig;
    const std::uint64_t id = rig.prepare();
    const std::size_t retained = rig.bus.joint_retained_bytes();
    REQUIRE(retained > 0);
    // X: a second operator with its OWN host-minted authority over an unrelated ceiling. It
    // holds neither A's capability (J6's copy) nor authority over A's participants; what it
    // presents is valid, issued here, and its own -- the request is wrong only in the
    // operation it names.
    auto [stranger_id, stranger] = put<Doc>(rig.bus, Grant{}, "joint.stranger");
    tell(rig.bus, stranger_id, Cmd{"claim", 0, "S", 1});
    rig.bus.drain_until_idle();
    REQUIRE(stranger->last_claim.accepted);
    auto [x_id, x] = put<Operator>(rig.bus, Grant{}, "joint.x");
    x->authority = rig.bus.mint_joint_authority(x_id, {"joint.stranger"});
    x->keys = {claim_key<DocFact>(stranger_id)};
    REQUIRE(x->authority.valid());
    // EVERY OPERATOR VERB, NAMED WITH A'S OPERATION: refused by name, and A's record is
    // exactly what it was -- Preparing, no reason, its offers retained, nothing owed.
    tell(rig.bus, x_id, Cmd{"commit", static_cast<std::int64_t>(id)});
    tell(rig.bus, x_id, Cmd{"cancel", static_cast<std::int64_t>(id)});
    tell(rig.bus, x_id, Cmd{"status", static_cast<std::int64_t>(id)});
    tell(rig.bus, x_id, Cmd{"release", static_cast<std::int64_t>(id)});
    rig.bus.drain_until_idle();
    CHECK_FALSE(x->committed.ok);
    CHECK_MESSAGE(x->committed.why == JointRefusal::NotOperator, std::string(name_of(x->committed.why)));
    CHECK(x->cancelled.why == JointRefusal::NotOperator);
    CHECK(x->status.state == JointState::Missing);
    CHECK(x->status.reason == JointRefusal::NotOperator);
    CHECK(x->released.why == JointRefusal::NotOperator);
    CHECK_MESSAGE(rig.bus.joint_status(id).state == JointState::Preparing,
                  std::string(name_of(rig.bus.joint_status(id).state)));
    CHECK(rig.bus.joint_status(id).reason == JointRefusal::None);
    CHECK(rig.bus.joint_retained_bytes() == retained);
    CHECK(rig.bus.joint_pending() == 1);
    CHECK(rig.doc_path() == "A");
    CHECK(rig.view_shown() == "hidden");
    CHECK(rig.o->ended.empty()); // nothing ended, so no notice of an ending was queued
    // X IS A GENUINE OPERATOR over its own ceiling: its own begin works. That is what
    // separates this case from J6's copy, which never reaches a record at all.
    tell(rig.bus, x_id, Cmd{"begin"});
    rig.bus.drain_until_idle();
    CHECK_MESSAGE(x->begun.ok, std::string(name_of(x->begun.why)));
    CHECK(rig.bus.joint_pending() == 2);
    // A'S LEGITIMATE COMMIT AFTERWARDS publishes as if X had never spoken.
    rig.commit(id);
    rig.bus.drain_until_idle();
    CHECK_MESSAGE(rig.o->committed.ok, std::string(name_of(rig.o->committed.why)));
    CHECK(rig.doc_path() == "B");
    CHECK(rig.view_shown() == "B");
    CHECK(rig.bus.joint_status(id).state == JointState::Committed);
    // ...and A's committed record is no more X's to release than its live one was.
    tell(rig.bus, x_id, Cmd{"release", static_cast<std::int64_t>(id)});
    rig.bus.drain_until_idle();
    CHECK(x->released.why == JointRefusal::NotOperator);
    CHECK(rig.bus.joint_status(id).state == JointState::Committed);
    CHECK(rig.bus.joint_records() == 2);
}

TEST_CASE("J24: a JointAuthority names the operator's exact life and incarnation -- retiring its records and refusing its retained capability are two obligations, and the host authorizes a successor by minting again") {
    SUBCASE("swapped: new code behind the operator's id") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        // THE PREDECESSOR'S CAPABILITY, retained in the same object's member across the
        // swap -- what a native operator that is never re-authorized holds.
        const JointAuthority retained = rig.o->authority;
        CHECK(retained.operator_id() == rig.op);
        CHECK(retained.operator_life() == 1);
        CHECK(retained.operator_incarnation() == 1);
        const std::string bytes = rig.bus.snapshot_bytes(rig.op);
        REQUIRE(rig.bus.swap_state(rig.op, bytes).revived);
        // OBLIGATION ONE: the records the predecessor began are retired at the transition.
        CHECK(rig.bus.joint_status(id).state == JointState::Missing);
        CHECK(rig.bus.joint_records() == 0);
        // OBLIGATION TWO: the retained capability names nobody now. Every operator verb
        // presented with it is refused NotOperator, and a begin creates no record.
        tell(rig.bus, rig.op, Cmd{"begin"});
        tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(id)});
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(id)});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK_MESSAGE(rig.o->begun.why == JointRefusal::NotOperator, std::string(name_of(rig.o->begun.why)));
        CHECK(rig.o->status.state == JointState::Missing);
        CHECK(rig.o->status.reason == JointRefusal::NotOperator);
        CHECK(rig.o->released.why == JointRefusal::NotOperator);
        CHECK(rig.bus.joint_records() == 0);
        CHECK(rig.bus.joint_pending() == 0);
        // THE HOST AUTHORIZES THE SUCCESSOR, explicitly, for the incarnation that exists;
        // nothing in the bus or the SDK did it for the host.
        const JointAuthority current =
            rig.bus.mint_joint_authority(rig.op, {"joint.doc", "joint.view"});
        REQUIRE(current.valid());
        CHECK(current.operator_life() == retained.operator_life());
        CHECK(current.operator_incarnation() == retained.operator_incarnation() + 1);
        rig.o->authority = current;
        const std::uint64_t fresh = rig.prepare("C");
        REQUIRE(rig.bus.joint_status(fresh).state == JointState::Preparing);
        const std::size_t offered = rig.bus.joint_retained_bytes();
        REQUIRE(offered > 0);
        // ...AND THE STALE CAPABILITY CANNOT REACH THE NEW RECORD: the same weave, the
        // predecessor's capability, the successor's own operation -- every verb refused,
        // and the record exactly as it was.
        rig.o->authority = retained;
        tell(rig.bus, rig.op, Cmd{"commit", static_cast<std::int64_t>(fresh)});
        tell(rig.bus, rig.op, Cmd{"cancel", static_cast<std::int64_t>(fresh)});
        tell(rig.bus, rig.op, Cmd{"status", static_cast<std::int64_t>(fresh)});
        tell(rig.bus, rig.op, Cmd{"release", static_cast<std::int64_t>(fresh)});
        rig.bus.drain_until_idle();
        CHECK(rig.o->committed.why == JointRefusal::NotOperator);
        CHECK(rig.o->cancelled.why == JointRefusal::NotOperator);
        CHECK(rig.o->status.reason == JointRefusal::NotOperator);
        CHECK(rig.o->released.why == JointRefusal::NotOperator);
        CHECK(rig.bus.joint_status(fresh).state == JointState::Preparing);
        CHECK(rig.bus.joint_status(fresh).reason == JointRefusal::None);
        CHECK(rig.bus.joint_retained_bytes() == offered);
        CHECK(rig.doc_path() == "A");
        // THE POSITIVE PATH, with the current authority: the successor coordinates.
        rig.o->authority = current;
        rig.commit(fresh);
        rig.bus.drain_until_idle();
        CHECK_MESSAGE(rig.o->committed.ok, std::string(name_of(rig.o->committed.why)));
        CHECK(rig.doc_path() == "C");
        CHECK(rig.view_shown() == "C");
        CHECK(rig.bus.joint_status(fresh).state == JointState::Committed);
    }
    SUBCASE("dead and revived: a new life behind the operator's id, and nothing minted for the dead") {
        Rig rig;
        const std::uint64_t id = rig.prepare();
        const JointAuthority retained = rig.o->authority;
        const std::string bytes = rig.bus.snapshot_bytes(rig.op);
        rig.bus.kill(rig.op);
        CHECK(rig.bus.joint_status(id).state == JointState::Missing); // retired with the life
        CHECK(rig.bus.joint_records() == 0);
        // MINTED FOR THE DEAD, OR FOR NOBODY: there is no live participant to bind, so the
        // result is not valid and can never become the revived life's by accident.
        const JointAuthority for_the_dead =
            rig.bus.mint_joint_authority(rig.op, {"joint.doc", "joint.view"});
        CHECK_FALSE(for_the_dead.valid());
        CHECK_FALSE(rig.bus.mint_joint_authority(WeaveId{987654}, {"joint.doc"}).valid());
        REQUIRE(rig.bus.reload(rig.op, bytes).revived);
        // THE ENDED LIFE'S CAPABILITY is refused by the revived one; so is the one minted
        // while it was dead. Neither creates a record.
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK_MESSAGE(rig.o->begun.why == JointRefusal::NotOperator, std::string(name_of(rig.o->begun.why)));
        rig.o->authority = for_the_dead;
        tell(rig.bus, rig.op, Cmd{"begin"});
        rig.bus.drain_until_idle();
        CHECK_FALSE(rig.o->begun.ok);
        CHECK(rig.o->begun.why == JointRefusal::ForeignAuthority);
        CHECK(rig.bus.joint_records() == 0);
        // MINTED FOR THE LIFE THAT EXISTS, it coordinates.
        rig.o->authority = rig.bus.mint_joint_authority(rig.op, {"joint.doc", "joint.view"});
        REQUIRE(rig.o->authority.valid());
        CHECK(rig.o->authority.operator_life() == retained.operator_life() + 1);
        CHECK(rig.o->authority.operator_incarnation() == retained.operator_incarnation());
        const std::uint64_t fresh = rig.prepare("C");
        rig.commit(fresh);
        rig.bus.drain_until_idle();
        CHECK_MESSAGE(rig.o->committed.ok, std::string(name_of(rig.o->committed.why)));
        CHECK(rig.doc_path() == "C");
        CHECK(rig.view_shown() == "C");
    }
}

} // TEST_SUITE("joint")
