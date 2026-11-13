// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The asker probe: a weave that asks through `Mail`'s four ask forms when kicked, and keeps in its
// state the ticket each ask returned and what Loom told it of each one's end; built native, boxed
// and as an image compiled against an earlier header alike. ASK_PROBE_DEAF builds the same weave
// without `zen.AskEnded` in its accept-set, so each of its asks is refused at the call.

#include "ask_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <string>

using namespace loom;
using namespace asking;

namespace {

#if defined(ASK_PROBE_DEAF)
#define ASK_PROBE_ACCEPTS Accept<AskKick, AskReply>
#else
#define ASK_PROBE_ACCEPTS Accept<AskKick, AskReply, AskEnded>
#endif

class AskProbe final : public WeaveBase<AskProbe, AskHeard, ASK_PROBE_ACCEPTS> {
public:
    void on(const AskKick& k, Mail& mail) {
        const AskQuestion q{k.n};
        const auto c = static_cast<std::uint64_t>(k.correlation);
        const WeaveId to{static_cast<std::uint64_t>(k.target)};
        Ticket t;
        switch (k.form) {
        case 0:
            t = mail.ask(to, q, c);
            break;
        case 1:
            t = mail.ask_role(k.role, q, c);
            break;
        case 2:
            t = mail.as_role(k.as_role).ask(to, q, c);
            break;
        default:
            t = mail.as_role(k.as_role).ask_role(k.role, q, c);
            break;
        }
        state_.ticket = static_cast<std::int64_t>(t.seq);
        state_.refusal = t.valid() ? std::string{} : std::string(name_of(t.refusal));
    }

    void on(const AskReply&, Mail& mail) {
        if (mail.answers_ask()) {
            ++state_.answers;
        }
    }

#if !defined(ASK_PROBE_DEAF)
    void on(const AskEnded& ended, Mail& mail) {
        if (!mail.ask_ended()) {
            ++state_.forged; // the shape alone is ordinary speech
            return;
        }
        ++state_.endings;
        state_.stage = stage_name(ended.stage);
        state_.reason = std::string(reason_name(ended.stage));
        state_.ended_attempt = static_cast<std::int64_t>(ended.ended_attempt().seq);
        state_.correlation = static_cast<std::int64_t>(mail.correlation());
        state_.shape = ended.shape;
        state_.role = ended.role;
        state_.target = static_cast<std::int64_t>(ended.addressed_weave().value);
    }
#endif
};

} // namespace

ZEN_EXPORT_WEAVE(AskProbe)
