// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_WEAVE_ROLE_REQUEST_HPP
#define ZEN_WEAVE_ROLE_REQUEST_HPP

#include <zen/weave/dispatch_refusal.hpp>
#include <zen/weave/weave.hpp>
#include <cstdint>
#include <string>
#include <utility>

namespace loom {

// One role-addressed attempt, using the caller's correlation sequence and ordinary Mail
// (or Mail::Office) send. Matching never consumes the record: validate the reply's meaning,
// then forget it explicitly. No retry, timeout, cancellation or completion policy.
// See docs/reference/messaging.md#one-role-addressed-request.
class RoleRequest {
public:
    bool pending() const noexcept { return attempt_.valid(); }
    Ticket attempt() const noexcept { return attempt_; }
    std::uint64_t correlation() const noexcept { return correlation_; }

    // A second send while occupied refuses locally, preserving the original attempt.
    // The caller owns a unique correlation across ALL its outstanding operations.
    template<class Sender, class Request>
    bool send_to_role(Sender&& sender, const std::string& role, const Request& request,
                      std::uint64_t correlation) {
        if (pending()) return false;
        role_ = role;
        shape_ = Request::zen_name;
        version_ = Request::zen_version;
        correlation_ = correlation;
        attempt_ = std::forward<Sender>(sender).send_to_role(role_, request, correlation_);
        if (!pending()) forget();
        return pending();
    }

    template<class Request>
    bool is() const noexcept {
        return pending() && shape_ == Request::zen_name && version_ == Request::zen_version;
    }

    bool matches_answer(const Mail& mail) const noexcept {
        return pending() && mail.answers_ask() && mail.correlation() == correlation_;
    }

    bool matches_refusal(const DispatchRefused& refused, const Mail& mail) const noexcept {
        return pending() && mail.dispatch_refused() &&
               refused.refused_attempt().seq == attempt_.seq &&
               refused.role == role_ && refused.target.empty() &&
               refused.shape == shape_ && refused.version == version_;
    }

    // Local bookkeeping only. The recipient may still be doing the work.
    void forget() noexcept {
        attempt_ = {};
        correlation_ = 0;
        role_.clear();
        shape_.clear();
        version_ = 0;
    }

private:
    Ticket attempt_;
    std::uint64_t correlation_ = 0;
    std::string role_, shape_;
    std::uint32_t version_ = 0;
};
} // namespace loom

#endif
