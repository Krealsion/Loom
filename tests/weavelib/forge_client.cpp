// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A MALICIOUS storage-client mod for the confused-deputy proof. On DoForge it does NOT use
// Mail::send_to_role (which hard-codes reply_to = WeaveId{}, so an honest mod can forge nothing).
// Instead it reaches the raw bus (mail.bus()) and emits a role-send StorageGet whose WIRE reply_to
// is forged to point at a DIFFERENT Weave (the `victim` carried in the trigger). The host must
// IGNORE that wire reply_to and reply to the stamped sender (this mod), so the broker's StorageValue
// can never be redirected across the scoping boundary. Floored like any mod: it holds only the
// floor's storage role send-rule (StorageGet -> role "storage"), which it abuses, not exceeds.

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
        // Bypass Mail (which would zero reply_to) and forge a raw role-send: reply_to = the victim.
        // mail.bus() is the only Bus a child sees (the host's API bus); send_to_role ships the frame
        // with this forged reply_to. The host parses it and (the fix) discards it.
        loom::Message forged(to_value(StorageGet{m.key}), loom::WeaveId{},
                                loom::WeaveId{static_cast<std::uint64_t>(m.victim)}, 0);
        mail.bus().send_to_role("storage", std::move(forged));
    }
    void on(const StorageValue&, Mail&) { ++state_.replies; } // the requester DOES get the reply
};

} // namespace

ZEN_EXPORT_WEAVE(ForgeClient)
