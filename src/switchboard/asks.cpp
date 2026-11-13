// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#include <zen/switchboard/asks.hpp>
#include <zen/switchboard/switchboard.hpp> // name_of(RefusalReason)
#include <zen/weave/ask_ended.hpp>
#include <zen/weave/dispatch_refusal.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace loom {

AskFacts ask_facts(const Message& msg, std::uint64_t attempt, std::uint64_t life,
                   std::uint64_t incarnation, WeaveId target, const std::string& role) {
    AskFacts ask;
    ask.attempt = attempt;
    ask.asker = msg.sender;
    ask.asker_life = life;
    ask.asker_incarnation = incarnation;
    ask.target = role.empty() ? target : WeaveId{};
    ask.role = role;
    ask.shape = msg.payload.schema().name();
    ask.version = msg.payload.schema().version();
    ask.correlation = msg.correlation;
    return ask;
}

// ---- the notice ------------------------------------------------------------------------------

bool ask_ending_of(RefusalReason reason, AskEnding& ending) noexcept {
    switch (reason) {
    case RefusalReason::CapabilityDenied:
        ending = AskEnding::CapabilityDenied;
        return true;
    case RefusalReason::NoSuchTarget:
        ending = AskEnding::NoSuchTarget;
        return true;
    case RefusalReason::TargetUnavailable:
        ending = AskEnding::TargetUnavailable;
        return true;
    case RefusalReason::NotAccepted:
        ending = AskEnding::NotAccepted;
        return true;
    case RefusalReason::GateRefused:
        ending = AskEnding::GateRefused;
        return true;
    // A target held behind a published claim it could not apply is a pre-handler refusal like
    // the five above, and the one a weaver most needs by exact attempt.
    case RefusalReason::ApplicationFailed:
        ending = AskEnding::ApplicationFailed;
        return true;
    // The asker was retired between asking and delivery: a sealed weave's speech reaches only
    // its coordinator.
    case RefusalReason::SealedSpeech:
        ending = AskEnding::AskerSealed;
        return true;
    default:
        // `SenderLifeEnded` is the asker's own ending, with nobody left to tell; the rest are
        // not refusals of a queued send.
        return false;
    }
}

namespace {
AskEnded::Stage stage_of(AskEnding ending) {
    AskEnded::Stage s;
    switch (ending) {
    case AskEnding::CapabilityDenied:
        s.is = AskNotDelivered::CapabilityDenied;
        break;
    case AskEnding::NoSuchTarget:
        s.is = AskNotDelivered::NoSuchTarget;
        break;
    case AskEnding::TargetUnavailable:
        s.is = AskNotDelivered::TargetUnavailable;
        break;
    case AskEnding::NotAccepted:
        s.is = AskNotDelivered::NotAccepted;
        break;
    case AskEnding::GateRefused:
        s.is = AskNotDelivered::GateRefused;
        break;
    case AskEnding::ApplicationFailed:
        s.is = AskNotDelivered::ApplicationFailed;
        break;
    case AskEnding::AskerSealed:
        s.is = AskNotDelivered::AskerSealed;
        break;
    case AskEnding::Unanswered:
        s.is = AskNotAnswered::Unanswered;
        break;
    case AskEnding::HandlerFailed:
        s.is = AskNotAnswered::HandlerFailed;
        break;
    case AskEnding::Released:
        s.is = AskNotKept::Released;
        break;
    case AskEnding::RespondentReplaced:
        s.is = AskNotKept::RespondentReplaced;
        break;
    case AskEnding::RespondentRemoved:
        s.is = AskNotKept::RespondentRemoved;
        break;
    case AskEnding::RespondentDied:
        s.is = AskNotKept::RespondentDied;
        break;
    case AskEnding::RespondentRetired:
        s.is = AskNotKept::RespondentRetired;
        break;
    case AskEnding::AnswerRefused:
        s.is = AnswerNotDelivered::AnswerRefused;
        break;
    }
    return s;
}

} // namespace

Value ask_ended_value(const AskFacts& ask, AskEnding ending) {
    AskEnded notice;
    notice.attempt = std::to_string(ask.attempt);
    notice.target = ask.role.empty() ? std::to_string(ask.target.value) : std::string{};
    notice.role = ask.role;
    notice.shape = ask.shape;
    notice.version = ask.version;
    notice.stage = stage_of(ending);
    return to_value(notice);
}

std::optional<Value> dispatch_refused_value(std::uint64_t attempt, WeaveId target,
                                            const std::string& role, const std::string& shape,
                                            std::uint32_t version, RefusalReason told) {
    AskEnding said = AskEnding::AskerSealed;
    if (!ask_ending_of(told, said) || said == AskEnding::AskerSealed) {
        return std::nullopt; // not one of the six it says: a sealed sender is told nothing
    }
    DispatchRefused notice;
    notice.attempt = std::to_string(attempt);
    notice.target = role.empty() ? std::to_string(target.value) : std::string{};
    notice.role = role;
    notice.shape = shape; // the gate may already have consumed the payload
    notice.version = version;
    notice.reason = name_of(told);
    return to_value(notice);
}

// ---- the kept answers ------------------------------------------------------------------------

KeptAnswer* KeptAnswers::find(std::uint64_t token) noexcept {
    if (token == 0) {
        return nullptr;
    }
    for (KeptAnswer& r : slots_) {
        if (r.token == token) {
            return &r;
        }
    }
    return nullptr;
}

std::size_t KeptAnswers::held_by(WeaveId respondent) const noexcept {
    std::size_t n = 0;
    for (const KeptAnswer& r : slots_) {
        if (r.token != 0 && r.respondent == respondent) {
            ++n;
        }
    }
    return n;
}

std::uint64_t KeptAnswers::take(KeptAnswer right, Full& full) {
    // The respondent's own bound first: a weave that keeps too many answers exhausts itself and
    // nobody else, so the Loom-wide bound is only ever met by several weaves together.
    if (held_by(right.respondent) >= respondent_bound_) {
        full = Full::Respondent;
        return 0;
    }
    KeptAnswer* slot = nullptr;
    for (KeptAnswer& r : slots_) {
        if (r.token == 0) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        if (slots_.size() >= loom_bound_) {
            full = Full::Loom;
            return 0;
        }
        slot = &slots_.emplace_back();
    }
    full = Full::No;
    *slot = std::move(right);
    slot->token = next_token_++;
    return slot->token;
}

KeptAnswer KeptAnswers::release(KeptAnswer& rec) noexcept {
    KeptAnswer was = std::move(rec);
    rec = KeptAnswer{}; // the slot is free again
    return was;
}

template <class Ends>
std::vector<AskFacts> KeptAnswers::end_where(WeaveId id, Ends ends) {
    // Both directions, and only rights `id` is a party to. A right it kept tells its asker; a
    // right it asked for ends with the asker, and nobody is left to tell.
    std::vector<AskFacts> told;
    for (KeptAnswer& r : slots_) {
        if (r.token == 0) {
            continue;
        }
        const bool as_respondent = r.respondent == id && ends(r.respondent_incarnation);
        const bool as_requester = r.requester == id && ends(r.requester_incarnation);
        if (as_respondent && r.ask.present()) {
            told.push_back(r.ask);
        }
        if (as_respondent || as_requester) {
            r = KeptAnswer{};
        }
    }
    return told;
}

std::vector<AskFacts> KeptAnswers::end_life(WeaveId id) {
    return end_where(id, [](std::uint64_t) { return true; });
}

std::vector<AskFacts> KeptAnswers::end_incarnation(WeaveId id, std::uint64_t now) {
    return end_where(id, [now](std::uint64_t inc) { return inc != now; });
}

std::vector<AskFacts> KeptAnswers::end_respondent(WeaveId respondent, std::uint64_t incarnation) {
    std::vector<AskFacts> told;
    for (KeptAnswer& r : slots_) {
        if (r.token != 0 && r.respondent == respondent &&
            r.respondent_incarnation == incarnation) {
            if (r.ask.present()) {
                told.push_back(r.ask);
            }
            r = KeptAnswer{};
        }
    }
    return told;
}

} // namespace loom
