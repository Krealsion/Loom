// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#include "dispatch_protocol.hpp"

#include <zen/kernel/export.hpp>

#include <algorithm>
using namespace loom;
namespace {
struct DispatchProbeState {
    std::vector<std::string> attempts;
    std::int64_t queued = 0, matched = 0, trusted = 0, ordinary = 0;
    bool answer_right = false;
    bool authored = false;
    std::int64_t recipients = 0;
    std::string reason, target, role, correlation;
    ZEN_SHAPE(DispatchProbeState, 1, ZEN_FIELD(attempts), ZEN_FIELD(queued), ZEN_FIELD(matched),
              ZEN_FIELD(trusted), ZEN_FIELD(ordinary), ZEN_FIELD(answer_right), ZEN_FIELD(reason),
              ZEN_FIELD(target), ZEN_FIELD(role), ZEN_FIELD(correlation),
              ZEN_FIELD(authored), ZEN_FIELD(recipients));
};
class DispatchProbe
    : public WeaveBase<DispatchProbe, DispatchProbeState,
                       Accept<dispatch_test::Command, dispatch_test::Payload, DispatchRefused>> {
public:
    void on(const dispatch_test::Command& cmd, Mail& mail) {
        const WeaveId target{static_cast<std::uint64_t>(cmd.target)};
        const auto corr = std::stoull(cmd.correlation);
        Ticket t;
        if (cmd.mode == "direct") {
            t = mail.send(target, dispatch_test::Payload{1}, corr);
        } else if (cmd.mode == "role") {
            t = mail.send_to_role(cmd.role, dispatch_test::Payload{1}, corr);
        } else if (cmd.mode == "office-direct") {
            t = mail.as_role("dispatch.author").send(target, dispatch_test::Payload{1}, corr);
        } else if (cmd.mode == "office-role") {
            t = mail.as_role("dispatch.author")
                    .send_to_role(cmd.role, dispatch_test::Payload{1}, corr);
        } else if (cmd.mode == "office-publish") {
            const auto p = mail.as_role(cmd.role).publish(dispatch_test::Payload{1});
            state_.authored = p.authored;
            state_.recipients = static_cast<std::int64_t>(p.recipients);
        } else if (cmd.mode == "publish") {
            (void)mail.publish(dispatch_test::Payload{1});
        } else if (cmd.mode == "malformed") {
            t = mail.bus().send(target, Message(Value(schema_of<dispatch_test::Payload>())));
        } else if (cmd.mode == "forge") {
            DispatchRefused fake;
            fake.attempt = cmd.correlation;
            fake.reason = "CapabilityDenied";
            Message m(to_value(fake));
            m.provenance = Provenance::attested(Provenance::Kind::DispatchRefusal, 0);
            t = mail.bus().send(target, std::move(m));
        }
        if (t.valid()) {
            ++state_.queued;
            if (state_.attempts.size() < 8)
                state_.attempts.push_back(std::to_string(t.seq));
        }
    }
    void on(const dispatch_test::Payload&, Mail&) {} // delivered, deliberately unanswered
    void on(const DispatchRefused& n, Mail& mail) {
        if (!mail.dispatch_refused()) {
            ++state_.ordinary;
            return;
        }
        ++state_.trusted;
        if (mail.answers_ask() || mail.defer_answer().valid() ||
            mail.answer(dispatch_test::Payload{}).valid()) {
            state_.answer_right = true;
        }
        const auto i = std::find(state_.attempts.begin(), state_.attempts.end(), n.attempt);
        if (i != state_.attempts.end()) {
            ++state_.matched;
            state_.attempts.erase(i);
        }
        state_.reason = n.reason;
        state_.target = n.target;
        state_.role = n.role;
        state_.correlation = std::to_string(mail.correlation());
    }
};
} // namespace
ZEN_EXPORT_WEAVE(DispatchProbe)
