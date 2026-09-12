// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// A real loaded claimant for the joint-publication witnesses (suite `joint`). It
// claims a DocFact, offers the next one for an operation it is told about, and
// folds a joint-published DocFact into its state through `on_claim_published` --
// the route a loaded document owner would take. Nothing here is an application;
// the suite reads its state back through a snapshot.
//
// Three more things a real participant can do, each driven from the suite so the
// seam is exercised for each: FAIL to apply a published claim (`arm-fail`: the
// next hook increments its attempt counter and throws before applying anything --
// the failure the host must not swallow), DECLINE one (`arm-decline`: the `bool`
// form's `false`, crossing as ZEN_CLAIM_DECLINED), and MIRROR its native state
// into its claim at the end of every delivery (`after_delivery`), which is how a
// participant whose exposed state was written through a substrate door keeps its
// claim true.
#include "joint_protocol.hpp"

#include <zen/kernel/export.hpp>

#include <stdexcept>

using namespace loom;
using namespace joint_test;

namespace {

class JointProbe
    : public WeaveBase<JointProbe, ProbeState, Accept<Cmd>, Emit<>, Claims<DocFact>> {
public:
    void on(const Cmd& c, Mail& mail) {
        ++state_.deliveries;
        // WHAT THE HANDLER FOUND: if the hook ran before this delivery, this is
        // already the published path — which is the whole property under test.
        state_.seen_at_delivery = state_.path;
        if (c.verb == "claim") {
            state_.path = c.path;
            state_.epoch = c.epoch;
            const SenseClaimResult r = mail.claim(DocFact{state_.path, state_.epoch});
            state_.last_offer_ok = r.accepted ? 1 : 0;
            state_.last_refusal = r.accepted ? std::string() : name_of(r.why);
            if (r.accepted) {
                state_.claimed_path = state_.path;
                state_.claimed_epoch = state_.epoch;
            }
        } else if (c.verb == "offer") {
            const JointResult r =
                mail.offer(static_cast<std::uint64_t>(c.op), DocFact{c.path, c.epoch});
            state_.last_offer_ok = r.ok ? 1 : 0;
            state_.last_refusal = r.ok ? std::string() : name_of(r.why);
        } else if (c.verb == "arm-fail") {
            // NATIVE, NOT IN THE STATE SHAPE: a reload revives a successor that is not
            // armed, which is what lets a reload be the repair of a failed application.
            fail_next_ = true;
        } else if (c.verb == "arm-decline") {
            // ...and the third answer: the next showing is counted and DECLINED -- this
            // weave keeps its own state, is not broken, and says so in the `bool` form
            // of the hook, which crosses the seam as ZEN_CLAIM_DECLINED.
            decline_next_ = true;
        }
    }

    /// THE HOOK, ACROSS THE SEAM: the published fact becomes this weave's own -- or,
    /// when armed, the attempt is counted and fails before anything is applied -- or,
    /// armed the other way, the attempt is counted and declined (`false`).
    bool on_claim_published(const DocFact& published) {
        ++state_.published_seen;
        if (fail_next_) {
            fail_next_ = false;
            throw std::runtime_error("the probe could not apply its published claim");
        }
        if (decline_next_) {
            decline_next_ = false;
            return false;
        }
        state_.path = published.path;
        state_.epoch = published.epoch;
        state_.claimed_path = state_.path;
        state_.claimed_epoch = state_.epoch;
        return true;
    }

    /// THE END OF EVERY DELIVERY: the claim follows the state. A write through a
    /// substrate door that changed `path` is a change of this weave's fact, and the
    /// claim it stands behind must say so.
    void after_delivery(Mail& mail) {
        ++state_.hook_runs;
        if (state_.path != state_.claimed_path || state_.epoch != state_.claimed_epoch) {
            const SenseClaimResult r = mail.claim(DocFact{state_.path, state_.epoch});
            if (r.accepted) {
                state_.claimed_path = state_.path;
                state_.claimed_epoch = state_.epoch;
            }
        }
    }

private:
    bool fail_next_ = false;
    bool decline_next_ = false;
};

} // namespace

ZEN_EXPORT_WEAVE(JointProbe)
