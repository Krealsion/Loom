// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE HANDOFF GARDEN'S LEDGER: one source, two REAL artifacts whose
// state schemas are genuinely incompatible.
//
//   v1  (default)              LedgerV1{next_id, total, mode}
//   v2  (ZEN_HANDOFF_V2)       LedgerV2{ids{high_water}, totals{count,sum},
//                                       modes:List<ModeFlag>}
//
// One source, for the same reason the versioned.service pair is one source: the
// artifact that prepares must be the artifact that goes live.
//
// WHAT EACH KNOWS HOW TO DO, and what it deliberately does not:
//
//   v1  accepts the OLD protocol (AddV1). It also accepts Quiesce — the FIFO
//       handoff boundary — after which it refuses production by DOMAIN POLICY
//       and says so. Loom chose none of that; this weave did.
//   v2  accepts the NEW protocol (AddV2) and does NOT accept AddV1 at all. Old
//       traffic aimed at it after the role moves refuses as NotAccepted, which
//       is the honest outcome: Loom does not know whether an old command still
//       means anything, so it does not pretend to.
//
// Both declare Claims<LedgerStatus>, so the role-bound Sense view is continuous
// across the replacement — which is what makes "the predecessor's claim is never
// relabelled as the successor's" a real question here.

#include "handoff_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/zen.hpp>

#include <string>
#include <vector>

using namespace loom;
using namespace hg;

namespace {

#if defined(ZEN_HANDOFF_V2)
constexpr const char* kVersion = "v2";
#else
constexpr const char* kVersion = "v1";
#endif

#if defined(ZEN_HANDOFF_V2)

// ---- v2: the successor ------------------------------------------------------

class LedgerV2Weave
    : public WeaveBase<LedgerV2Weave, LedgerV2,
                       Accept<Issue, AddV2, AdoptMigrated, Describe, loom::Activated>, Emit<Issued>,
                       Claims<LedgerStatus>> {
public:
    /// THE PREPARATION CONVERSATION. The candidate is asked to adopt a migrated
    /// v2 value; it answers for itself, through the ordinary answer rail, so the
    /// coordinator's readiness verdict rests on an authenticated statement.
    ///
    /// NOTE WHAT IS NOT HAPPENING: no gate is being taught to transcode. The
    /// value arriving here is ALREADY a valid LedgerV2 — an explicit actor made
    /// it one. If it were not, this delivery would never have happened.
    void on(const AdoptMigrated& m, Mail& mail) {
        // A candidate may still refuse: adopting a namespace that has already
        // been overtaken would mint duplicate identities, and this service would
        // rather not go live than do that.
        if (m.state.ids.high_water < 0) {
            mail.answer(Adopted{false, "negative high-water"});
            return;
        }
        state_ = m.state;
        adopted_ = true;
        mail.answer(Adopted{true, ""});
    }

    void on(const loom::Activated&, Mail& mail) {
        // Legally live. The successor claims as the office for the FIRST time
        // here — before this, the office's claim is its predecessor's and says so.
        ++activations_;
        claim_status(mail);
    }

    void on(const Issue&, Mail& mail) {
        // The whole point of the namespace witness: the next identity continues
        // from the high-water mark the migration carried (or does not, in the
        // control case — and then it collides, visibly).
        const std::int64_t id = ++state_.ids.high_water;
        served_.push_back(id);
        mail.reply(Issued{id});
        claim_status(mail);
    }

    void on(const AddV2& a, Mail& mail) {
        ++state_.totals.count;
        state_.totals.sum += a.amount;
        state_.modes.push_back(ModeFlag{a.currency, true});
        claim_status(mail);
    }

    void on(const Describe&, Mail& mail) {
        // v2 answers in v1 shape only to keep the suite's reader simple; the
        // fields are its own.
        LedgerV1 as_v1;
        as_v1.next_id = state_.ids.high_water + 1;
        as_v1.total = state_.totals.sum;
        as_v1.mode = kVersion;
        mail.answer(LedgerV1Report{as_v1, false});
    }

    LifecyclePolicy policy_config() const { return LifecyclePolicy{4, true}; }

private:
    void claim_status(Mail& mail) {
        // Deliberately AS THE OFFICE: this is the ledger speaking, not this
        // artifact speaking about itself. Refused unless it holds the role right
        // now — which a sealed candidate does not.
        (void)mail.as_role(kLedgerRole)
            .claim(LedgerStatus{state_.ids.high_water, false, kVersion, 0});
    }

    bool adopted_ = false;
    std::int64_t activations_ = 0;
    std::vector<std::int64_t> served_;
};

#else

// ---- v1: the incumbent ------------------------------------------------------

class LedgerV1Weave : public WeaveBase<LedgerV1Weave, LedgerV1,
                                       Accept<Issue, AddV1, Quiesce, Describe>, Emit<Issued>,
                                       Claims<LedgerStatus>> {
public:
    void on(const Issue&, Mail& mail) {
        if (refuse_post_boundary(mail)) {
            return;
        }
        const std::int64_t id = state_.next_id++;
        mail.reply(Issued{id});
        claim_status(mail);
    }

    void on(const AddV1& a, Mail& mail) {
        if (refuse_post_boundary(mail)) {
            return;
        }
        state_.total += a.amount;
        claim_status(mail);
    }

    /// THE FIFO HANDOFF BOUNDARY. Everything delivered before this was handled
    /// under ordinary policy; everything after meets the policy chosen below.
    ///
    /// THE DOMAIN'S CHOICE, recorded here because Loom makes none: this ledger
    /// REFUSES post-boundary production. It could equally have deferred,
    /// buffered, redirected through an adapter, or degraded — those are all
    /// legitimate, and the substrate would support any of them identically. This
    /// one refuses because a ledger that keeps minting identities after handing
    /// its namespace away is the failure the namespace witness exists to catch.
    void on(const Quiesce&, Mail& mail) {
        quiesced_ = true;
        // The final authored value is simply the state as of this exact FIFO
        // position. Nothing further will change it, which is what makes the
        // Describe that follows EXACT rather than a snapshot of a moving thing.
        claim_status(mail);
    }

    /// Answer with this ledger's v1 meaning, LABELLED with whether it had already
    /// quiesced. A consumer that ignores the label is choosing to treat a
    /// snapshot as exact; it cannot do so by accident.
    void on(const Describe&, Mail& mail) {
        mail.answer(LedgerV1Report{state_, quiesced_});
    }

    LifecyclePolicy policy_config() const { return LifecyclePolicy{4, true}; }

private:
    /// The post-boundary policy, in one place so the suite can point at it.
    bool refuse_post_boundary(Mail& mail) {
        if (!quiesced_) {
            return false;
        }
        // Refusing is a DOMAIN act here, not a Loom refusal: the message really
        // was delivered (the incumbent still holds the role and still accepts the
        // shape), and this service declined to act on it. The count rides the
        // Sense, so the policy is observable without inventing a back channel.
        ++refused_after_boundary_;
        claim_status(mail);
        return true;
    }

    void claim_status(Mail& mail) {
        (void)mail.as_role(kLedgerRole)
            .claim(LedgerStatus{state_.next_id - 1, quiesced_, kVersion,
                                refused_after_boundary_});
    }

    bool quiesced_ = false;
    std::int64_t refused_after_boundary_ = 0;
};

#endif

} // namespace

#if defined(ZEN_HANDOFF_V2)
ZEN_EXPORT_WEAVE(LedgerV2Weave)
#else
ZEN_EXPORT_WEAVE(LedgerV1Weave)
#endif
