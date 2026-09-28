// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A MALICIOUS storage-client mod for the confused-deputy proof. On DoForge it bypasses Mail
// (whose send_to_role hard-codes reply_to = WeaveId{}) and emits a raw role-send StorageGet whose
// WIRE reply_to points at a DIFFERENT Weave (the trigger's `victim`). The host must ignore that
// and reply to the stamped sender, so a StorageValue never crosses the scoping boundary. It
// holds only the floor's storage send rule, which it abuses, not exceeds.

#include "storage_protocol.hpp"

#include <zen/weave/weave.hpp>
#include <zen/kernel/export.hpp>

#include <cstdint>

using namespace loom;
using namespace storage;

namespace {

struct ForgeState {
    std::int64_t replies = 0;
    ZEN_SHAPE(ForgeState, 1, ZEN_FIELD(replies));
};

class ForgeClient
    : public WeaveBase<ForgeClient, ForgeState, Accept<DoForge, StorageValue>, Emit<StorageGet>> {
public:
    void on(const DoForge& m, Mail& mail) {
        // Bypass Mail (which would zero reply_to) and forge a raw role-send: reply_to = the
        // victim. mail.bus() is the only Bus a child sees (the host's API bus); send_to_role
        // ships the frame with this forged reply_to, and the host parses it and discards it.
        loom::Message forged(to_value(StorageGet{m.key}), loom::WeaveId{},
                                loom::WeaveId{static_cast<std::uint64_t>(m.victim)}, 0);
        mail.bus().send_to_role("storage", std::move(forged));
    }
    void on(const StorageValue&, Mail&) { ++state_.replies; } // the requester DOES get the reply
};

} // namespace

ZEN_EXPORT_WEAVE(ForgeClient)
