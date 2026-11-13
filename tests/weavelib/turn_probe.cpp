// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The turn probe: a weave that keeps, for each knock, the turn's time and date its delivery was
// handed, or that it was handed none; built native and boxed alike.

#include "turn_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

using namespace loom;
using namespace turning;

namespace {

class TurnProbe final : public WeaveBase<TurnProbe, TurnHeard, Accept<TurnKnock>> {
public:
    void on(const TurnKnock&, Mail& mail) {
        ++state_.knocks;
        const std::optional<std::int64_t> ms = mail.turn_ms();
        const std::optional<std::int64_t> date = mail.turn_date_ms();
        state_.told = ms && date ? 1 : 0;
        state_.turn_ms = ms.value_or(0);
        state_.turn_date_ms = date.value_or(0);
    }
};

} // namespace

ZEN_EXPORT_WEAVE(TurnProbe)
