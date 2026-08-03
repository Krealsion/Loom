// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The `versioned.service` fixture pair (R2B-3b-3): two REAL loadable artifacts,
// v1 and v2, that implement one production role and one domain query.
//
//   v1  the incumbent. It answers "v1" and it answers it throughout — during the
//       whole preparation, after a refusal, after an abort, and right up to the
//       breath before commit. Every failure case in the suite asks it again.
//   v2  the candidate (ZEN_VERSIONED_CANDIDATE). The same production service,
//       answering "v2", PLUS the preparation conversation: it is asked to
//       prepare, it prepares, and it answers for itself.
//
// ONE SOURCE, because the artifact that prepares MUST be the artifact that goes
// live. A candidate assembled specially for preparation and swapped for a freshly
// loaded object at commit would prove nothing about either.
//
// The candidate is also the phase's hostile witness. Before it answers anything
// it REACHES FOR THE WORLD — publishes, addresses the production role, and
// addresses a stranger by id — and records how many times it tried. The suite
// reads the refusals off the tap. That is deliberate: "a sealed candidate cannot
// speak" is only a proof if something actually speaks.

#include "prepared_replacement_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/weave/shape.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace loom;

namespace {

#if defined(ZEN_VERSIONED_CANDIDATE)
constexpr const char* kVersion = "v2";
#else
constexpr const char* kVersion = "v1";
#endif

/// What this service remembers about itself — its only window on the world, and
/// deliberately the ORDINARY one. A sealed candidate cannot be asked anything by
/// anyone but its coordinator, so everything the suite needs to see about it is
/// read through `snapshot_bytes`, the same host call any weave answers. No back
/// channel was invented for the test.
std::shared_ptr<const Schema> state_schema() {
    static const auto s = SchemaBuilder("VersionedState", 1)
                              .field("served", Kind::Int)
                              .field("prepares", Kind::Int)
                              .field("continues", Kind::Int)
                              .field("deferred", Kind::Int)
                              .field("answered", Kind::Int)
                              .field("activations", Kind::Int)
                              .field("last_activation", Kind::Int)
                              .field("escapes", Kind::Int)
                              .field("retired", Kind::Int)
                              .field("token", Kind::Int)
                              // R2B-3d-1: what the activation handler tried, and
                              // what it got. Recorded rather than asserted here —
                              // the fixture attempts, the suite judges.
                              .field("act_answer", Kind::Int)
                              .field("act_defer", Kind::Int)
                              .field("act_send", Kind::Int)
                              .field("act_late_spend", Kind::Int)
                              .field("plan", Kind::Text)
                              .build();
    return s;
}

class VersionedService final : public Weave {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
#if defined(ZEN_VERSIONED_CANDIDATE)
        // THE CANDIDATE'S REAL CONTRACT — production plus preparation, declared
        // once and used both while sealed and after commit. There is no second,
        // wider contract for the preparation phase: what it accepts to be prepared
        // is what it accepts as the live service.
        return {schema_of<versioned::QueryVersion>(), schema_of<versioned::PrepareReplacement>(),
                schema_of<versioned::ContinuePreparation>(), schema_of<versioned::RetireNow>(),
                schema_of<loom::Activated>()};
#else
        return {schema_of<versioned::QueryVersion>(), schema_of<versioned::RetireNow>(),
                schema_of<loom::Activated>()};
#endif
    }

    void handle(const Message& in, Bus& bus) override {
        const std::string_view shape = in.payload.schema().name();

        if (shape == std::string_view(loom::Activated::zen_name)) {
            // R2A-1's law, unchanged and re-proven here across the seam: the fact
            // is trusted because Loom attests it, not because the shape arrived.
            // An ordinary `zen.Activated` — including one from the coordinator
            // during preparation — is a costume, and is ignored entirely.
            const std::int64_t claimed = in.payload.get("sequence")->as_int();
            if (!in.provenance.lifecycle_activation() ||
                in.provenance.attested_sequence() != claimed) {
                return;
            }
            ++activations_;
            last_activation_ = claimed;

            // ---- R2B-3d-1: FIRST BREATH IS NOT A QUESTION --------------------
            //
            // The fixture is the hostile witness again, and this is the whole
            // attack surface in three lines. It TRIES, from inside an accepted
            // activation, everything a delivery that had been an ask would
            // grant — and then does the one thing that is genuinely still its
            // right. Whether any of them worked is recorded, never judged here:
            // a fixture that declined to try would pass this proof by not
            // testing it.
            //
            // 1. answer the question nobody asked
            versioned::VersionReply forged;
            forged.version = kVersion;
            act_answer_ = bus.answer(Message(to_value(forged))).valid();
            // 2. keep an answer right for later
            activation_pending_ = bus.make_deferred_answer();
            act_defer_ = activation_pending_.valid();
            // 3. ...and ORDINARY DOMAIN SPEECH, to the very weave whose
            //    imaginary question it was just refused. This must work: not
            //    being answerable is not being mute.
            //
            // RECORDED AS "ATTEMPTED", NOT "SUCCEEDED", and the distinction is
            // the seam's rather than this phase's: the library side of an
            // ordinary `send` returns `Ticket{}` ALWAYS — the C ABI carries no
            // bus seq back — so `.valid()` here would be false even on a perfect
            // delivery. (`answer` and `defer_answer` are different: R2B-3b-1a
            // gave them real success/failure across the seam, which is exactly
            // why the two fields above ARE verdicts.) Whether this arrived is
            // the SUITE's to judge, from what the coordinator was handed.
            versioned::ActivationObserved note;
            note.sequence = claimed;
            note.version = kVersion;
            (void)bus.send(in.sender, Message(to_value(note)));
            act_send_ = true; // the handler got this far, having been refused twice
            return;
        }

        if (shape == std::string_view(versioned::RetireNow::zen_name)) {
            // A retired incumbent says nothing back and does nothing more. It is
            // sealed by now; the point of hearing this at all is that a coordinator
            // CAN still reach what it retired.
            retired_ = true;
            return;
        }

        if (shape == std::string_view(versioned::QueryVersion::zen_name)) {
            // THE PRODUCTION ANSWER, and it is a real authenticated answer across
            // the dynamic seam (ABI v4). A sealed candidate never reaches this
            // line, because no `QueryVersion` can reach a sealed candidate.
            //
            // R2B-3d-1: before answering the REAL question, try once more to
            // spend whatever the activation handed back. Nothing it kept may
            // become valid later — an invalid capability is invalid forever, not
            // merely at the moment it was refused.
            if (!activation_spend_tried_) {
                activation_spend_tried_ = true;
                versioned::VersionReply late;
                late.version = kVersion;
                act_late_spend_ =
                    bus.spend_deferred(activation_pending_, Message(to_value(late))).valid();
            }
            ++served_;
            versioned::VersionReply reply;
            reply.version = kVersion;
            (void)bus.answer(Message(to_value(reply)));
            return;
        }

#if defined(ZEN_VERSIONED_CANDIDATE)
        if (shape == std::string_view(versioned::PrepareReplacement::zen_name)) {
            ++prepares_;
            const versioned::PrepareReplacement ask =
                from_value<versioned::PrepareReplacement>(in.payload);
            transaction_ = ask.transaction;
            plan_ = ask.plan;
            // The one address it will ever know: whoever is preparing it, read
            // off the ask's bus-stamped sender. It has no way to look anything up.
            coordinator_ = in.sender;

            // ---- reach for the world, before saying anything useful ----------
            reach_for_the_world(bus, ask.escape_to);

            // ---- then validate what was actually asked -----------------------
            //
            // The candidate's own business, and refusing here is a REAL refusal:
            // an unknown plan is answered authentically, spending the same one
            // right a readiness would have spent.
            if (plan_ == "ready") {
                answer_ready(bus);
                return;
            }
            if (plan_ == "refuse") {
                answer_refused(bus, "declined");
                return;
            }
            if (plan_ == "defer" || plan_ == "defer-refuse") {
                // TAKE THE ANSWER RIGHT AWAY WITH IT and return without
                // answering. Nothing is retained but the opaque capability — no
                // Bus, no Message, no authority that could be re-derived.
                pending_ = bus.make_deferred_answer();
                deferred_ = pending_.valid();
                return;
            }
            answer_refused(bus, "unknown plan: " + plan_);
            return;
        }

        if (shape == std::string_view(versioned::ContinuePreparation::zen_name)) {
            ++continues_;
            // Preparation completes across deliveries, and the answer it produces
            // is spent from THIS handler using only what the earlier one kept.
            if (plan_ == "defer-refuse") {
                versioned::CandidateRefused no;
                no.transaction = transaction_;
                no.reason = "changed my mind";
                answered_ = bus.spend_deferred(pending_, Message(to_value(no))).valid();
                return;
            }
            versioned::CandidateReady yes;
            yes.transaction = transaction_;
            answered_ = bus.spend_deferred(pending_, Message(to_value(yes))).valid();
            return;
        }
#endif
    }

    Value snapshot() const override {
        Value v(state_schema());
        v.set("served", Cell::integer(served_));
        v.set("prepares", Cell::integer(prepares_));
        v.set("continues", Cell::integer(continues_));
        v.set("deferred", Cell::integer(deferred_ ? 1 : 0));
        v.set("answered", Cell::integer(answered_ ? 1 : 0));
        v.set("activations", Cell::integer(activations_));
        v.set("last_activation", Cell::integer(last_activation_));
        v.set("escapes", Cell::integer(escapes_));
        v.set("retired", Cell::integer(retired_ ? 1 : 0));
        v.set("token", Cell::integer(static_cast<std::int64_t>(pending_.opaque_token())));
        v.set("act_answer", Cell::integer(act_answer_ ? 1 : 0));
        v.set("act_defer", Cell::integer(act_defer_ ? 1 : 0));
        v.set("act_send", Cell::integer(act_send_ ? 1 : 0));
        v.set("act_late_spend", Cell::integer(act_late_spend_ ? 1 : 0));
        v.set("plan", Cell::text(plan_));
        return v;
    }

    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(8));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }

    void revive(const Value& state) override {
        served_ = state.get("served")->as_int();
        prepares_ = state.get("prepares")->as_int();
        continues_ = state.get("continues")->as_int();
        deferred_ = state.get("deferred")->as_int() != 0;
        answered_ = state.get("answered")->as_int() != 0;
        activations_ = state.get("activations")->as_int();
        last_activation_ = state.get("last_activation")->as_int();
        escapes_ = state.get("escapes")->as_int();
        retired_ = state.get("retired")->as_int() != 0;
        act_answer_ = state.get("act_answer")->as_int() != 0;
        act_defer_ = state.get("act_defer")->as_int() != 0;
        act_send_ = state.get("act_send")->as_int() != 0;
        act_late_spend_ = state.get("act_late_spend")->as_int() != 0;
        plan_ = std::string(state.get("plan")->as_text());
        // THE SUCCESSOR INHERITS THE NUMBER AND BELIEVES IT HOLDS A CAPABILITY.
        // It is entitled to nothing, and the board — never this fixture — is what
        // must say so. Without this, a reloaded candidate would simply have an
        // empty member and fail locally, proving nothing about the bus.
        pending_ = loom::DeferredAnswer::from_host_token(
            static_cast<std::uint64_t>(state.get("token")->as_int()));
    }

private:
#if defined(ZEN_VERSIONED_CANDIDATE)
    void answer_ready(Bus& bus) {
        versioned::CandidateReady yes;
        yes.transaction = transaction_;
        answered_ = bus.answer(Message(to_value(yes))).valid();
    }

    void answer_refused(Bus& bus, const std::string& why) {
        versioned::CandidateRefused no;
        no.transaction = transaction_;
        no.reason = why;
        answered_ = bus.answer(Message(to_value(no))).valid();
    }

    /// Every way out of the seal this weave can express, tried in one breath.
    /// None of them may work. The count is what the suite compares against the
    /// refusals it saw on the tap — a candidate that quietly did nothing would
    /// otherwise pass an isolation proof by not testing it.
    void reach_for_the_world(Bus& bus, std::int64_t stranger) {
        versioned::VersionReply leak;
        leak.version = kVersion;

        // 1. a publication — speech into the world by definition
        (void)bus.publish(Message(to_value(leak)));
        ++escapes_;
        // 2. the production role — it may not even learn whether the slot is held
        (void)bus.send_to_role("versioned.service", Message(to_value(leak)));
        ++escapes_;
        // 3. a stranger, by id
        if (stranger != 0) {
            (void)bus.send(WeaveId{static_cast<std::uint64_t>(stranger)},
                           Message(to_value(leak)));
            ++escapes_;
        }
        // 4. a domain message to the coordinator itself. This one is DELIVERED —
        //    the seal is a conversation, not a quarantine — and it is still not
        //    readiness. It is the sharpest forgery in the suite, because the
        //    speaker is the genuine candidate, the recipient is the genuine
        //    coordinator, and the only thing missing is that nobody asked.
        (void)bus.send(coordinator_, Message(to_value(leak)));
        ++escapes_;
    }
#endif

    std::int64_t served_ = 0;
    std::int64_t prepares_ = 0;
    std::int64_t continues_ = 0;
    std::int64_t activations_ = 0;
    std::int64_t last_activation_ = 0;
    std::int64_t escapes_ = 0;
    std::int64_t transaction_ = 0;
    bool deferred_ = false;
    bool answered_ = false;
    bool retired_ = false;
    /// R2B-3d-1: what the activation handler's three attempts returned.
    bool act_answer_ = false;
    bool act_defer_ = false;
    bool act_send_ = false;
    bool act_late_spend_ = false;
    bool activation_spend_tried_ = false;
    std::string plan_;
    WeaveId coordinator_{};
    loom::DeferredAnswer pending_{};
    /// Whatever the activation's `defer_answer()` handed back. It must never
    /// become spendable — kept precisely so the suite can watch it fail.
    loom::DeferredAnswer activation_pending_{};
};

} // namespace

ZEN_EXPORT_WEAVE(VersionedService)
