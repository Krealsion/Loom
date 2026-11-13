// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_WEAVE_ASK_ENDED_HPP
#define ZEN_WEAVE_ASK_ENDED_HPP

#include <zen/switchboard/bus.hpp>
#include <zen/weave/shape.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace loom {

// How far an ask got, and why no answer will come: one enumeration a stage, each a case of
// `AskEnded::Stage`. Registered by hand, so their names carry the "zen." prefix.
// docs/reference/messaging.md#asks-and-how-each-one-ends

/// The ask never reached a handler. The first six are `zen.DispatchRefused`'s words for the same
/// refusals (MSG-12); `AskerSealed` is the asker retired before its ask was delivered.
enum class AskNotDelivered : std::uint8_t {
    CapabilityDenied,
    NoSuchTarget,
    TargetUnavailable,
    NotAccepted,
    GateRefused,
    ApplicationFailed,
    AskerSealed,
};

/// A handler received the ask and returned with its answer right neither spent nor kept, or
/// did not finish.
enum class AskNotAnswered : std::uint8_t { Unanswered, HandlerFailed };

/// The respondent kept the answer right, and the right ended unspent.
enum class AskNotKept : std::uint8_t {
    Released,
    RespondentReplaced,
    RespondentRemoved,
    RespondentDied,
    RespondentRetired,
};

/// An answer was made, and Loom refused to deliver it.
enum class AnswerNotDelivered : std::uint8_t { AnswerRefused };

#define LOOM_ASK_ENUM(E, Name, ...)                                                                \
    [[maybe_unused]] inline auto zen_enum_info(E*) {                                                \
        using enum E;                                                                               \
        constexpr auto values = ::std::array{__VA_ARGS__};                                          \
        return ::loom::detail::EnumInfo<E, values.size()>{                                          \
            Name, 1, values, ::loom::detail::split_spellings<values.size()>(#__VA_ARGS__)};         \
    }                                                                                               \
    static_assert(true, "")
LOOM_ASK_ENUM(AskNotDelivered, "zen.AskEnded.Queued", CapabilityDenied, NoSuchTarget,
              TargetUnavailable, NotAccepted, GateRefused, ApplicationFailed, AskerSealed);
LOOM_ASK_ENUM(AskNotAnswered, "zen.AskEnded.Delivered", Unanswered, HandlerFailed);
LOOM_ASK_ENUM(AskNotKept, "zen.AskEnded.Deferred", Released, RespondentReplaced,
              RespondentRemoved, RespondentDied, RespondentRetired);
LOOM_ASK_ENUM(AnswerNotDelivered, "zen.AskEnded.Answered", AnswerRefused);
#undef LOOM_ASK_ENUM

/// Loom's notice that an ask this weave sent through the ask door has ended without an answer:
/// told once, to the exact life and incarnation that asked, granting nothing. The shape alone
/// is ordinary speech, which any weave granted it may send: check `Mail::ask_ended()` first.
/// Match `ended_attempt()` against the ticket the ask returned; a correlation may repeat.
/// `stage` says how far the ask got, which is what makes a retry safe or not: an ask that ended
/// `queued` reached no handler, and one that ended later may have been half done.
/// docs/reference/messaging.md#asks-and-how-each-one-ends
struct AskEnded {
    /// How far the ask got, and why no answer will come.
    struct Stage {
        std::variant<AskNotDelivered, AskNotAnswered, AskNotKept, AnswerNotDelivered> is;
        using ZenSelf = Stage;
        static constexpr const char* zen_name = "zen.AskEnded.Stage";
        static constexpr std::uint32_t zen_version = 1;
        static constexpr auto zen_union() { return &ZenSelf::is; }
        static constexpr auto zen_case_names =
            ::loom::detail::case_names("queued", "delivered", "deferred", "answered");
    };

    /// The ask's queued attempt: canonical unsigned decimal, as `zen.DispatchRefused` spells
    /// its attempt, since Loom's wire Int is signed. Use `ended_attempt()`.
    std::string attempt;
    std::string target; ///< the directed WeaveId, decimal; empty for a role address
    std::string role;   ///< the role as the ask addressed it, never its resolved holder
    std::string shape;  ///< the ask's own shape
    std::int64_t version = 0;
    Stage stage;

    using ZenSelf = AskEnded;
    static constexpr const char* zen_name = "zen.AskEnded";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(attempt), ZEN_FIELD(target), ZEN_FIELD(role),
                               ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(stage));
    }

    /// The ask this ends, as the ask door's Ticket named it.
    Ticket ended_attempt() const noexcept { return Ticket{unsigned_id(attempt)}; }
    /// The WeaveId the ask was addressed to; invalid for a role address.
    WeaveId addressed_weave() const noexcept { return WeaveId{unsigned_id(target)}; }
    /// The ask reached no handler, so sending it again cannot repeat work.
    bool never_delivered() const noexcept {
        return std::holds_alternative<AskNotDelivered>(stage.is);
    }

private:
    static std::uint64_t unsigned_id(const std::string& s) noexcept {
        if (s.empty() || (s.size() > 1 && s[0] == '0')) {
            return 0;
        }
        std::uint64_t n = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size() ? n : 0;
    }
};

/// The stage's name, as its case is named: `queued`, `delivered`, `deferred` or `answered`.
inline const char* stage_name(const AskEnded::Stage& stage) noexcept {
    return stage.is.index() < AskEnded::Stage::zen_case_names.size()
               ? AskEnded::Stage::zen_case_names[stage.is.index()]
               : "?";
}

/// The reason's name, as its enumerator is spelled.
inline std::string_view reason_name(const AskEnded::Stage& stage) noexcept {
    return std::visit(
        [](auto e) -> std::string_view {
            const auto info = zen_enum_info(static_cast<decltype(e)*>(nullptr));
            const std::size_t i = detail::enum_index(e);
            return i < info.spellings.size() ? info.spellings[i] : std::string_view("?");
        },
        stage.is);
}

} // namespace loom
#endif
