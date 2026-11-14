// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// A provider that forges: its descriptor is the export layer's but for two slots. `handle` keeps a
// copy of the host table each delivery is handed, past the delivery, and `provide` ignores the
// function the manifest names and speaks through that kept table instead: a send and a call. Its
// answer is the two statuses the host gave, CallSum{send's, call's}. The honest API cannot make a
// function reach a door; this is what a library that tried would meet.

#include "call_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <string>

using namespace loom;
using namespace calling;

namespace {

class Forger final : public WeaveBase<Forger, CallHeard, Accept<CallKnock>, Emit<>, Claims<>,
                                      Provides<CallAdd>> {
public:
    void on(const CallKnock&, Mail&) { ++state_.knocks; }
    static CallSum provide(const CallAdd& add) { return CallSum{add.a + add.b, 1}; }
};

/// The table of the last delivery, kept past it.
ZenHostApi g_kept{};
bool g_have = false;

} // namespace

extern "C" {
static void* forge_create(ZenByteSink r) { return detail::do_create<Forger>(r); }
static void forge_destroy(void* i) { detail::do_destroy<Forger>(i); }
static ZenStatus forge_describe(void* i, ZenByteSink s) { return detail::do_describe<Forger>(i, s); }
static ZenStatus forge_snapshot(void* i, ZenByteSink s) { return detail::do_snapshot<Forger>(i, s); }
static ZenStatus forge_policy(void* i, ZenByteSink s) { return detail::do_policy<Forger>(i, s); }
static ZenStatus forge_revive(void* i, const uint8_t* st, size_t n) {
    return detail::do_revive<Forger>(i, st, n);
}
static ZenStatus forge_handle(void* i, const ZenDelivery* d, const uint8_t* p, size_t n,
                              const ZenHostApi* h) {
    if (h != nullptr) {
        g_kept = *h; // the doors and the context, kept past the delivery that handed them
        g_have = true;
    }
    return detail::do_handle<Forger>(i, d, p, n, h);
}
static ZenStatus forge_claim_published(void* i, const uint8_t* v, size_t n) {
    return detail::do_claim_published<Forger>(i, v, n);
}
static ZenStatus forge_provide(const uint8_t*, size_t, ZenByteSink answer, ZenByteSink) {
    ZenStatus sent = ZEN_OK;
    ZenStatus called = ZEN_OK;
    if (g_have) {
        const std::string knock = serialize(to_value(CallKnock{"calls.forge", 0, 0}));
        const auto* k = reinterpret_cast<const uint8_t*>(knock.data());
        sent = g_kept.send(g_kept.ctx, 1, 0, 0, k, knock.size(), nullptr);
        const std::string add = serialize(to_value(CallAdd{1, 2}));
        called = g_kept.call(g_kept.ctx, "calls.forge", reinterpret_cast<const uint8_t*>(add.data()),
                             add.size(), "CallSum", 1, ZenByteSink{}, ZenByteSink{});
    }
    detail::sink_write(answer, serialize(to_value(CallSum{sent, called})));
    return ZEN_OK;
}

// By name, in declaration order (KERN-04).
ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION,
                                    .size = sizeof(ZenWeaveAbi),
                                    .host_size = sizeof(ZenHostApi),
                                    .manifest_version = ::loom::kManifestVersion,
                                    .create = forge_create,
                                    .destroy = forge_destroy,
                                    .describe = forge_describe,
                                    .snapshot = forge_snapshot,
                                    .policy = forge_policy,
                                    .revive = forge_revive,
                                    .handle = forge_handle,
                                    .claim_published = forge_claim_published,
                                    .describe_creation = nullptr,
                                    .create_with = nullptr,
                                    .provide = forge_provide};
    return &abi;
}
} // extern "C"
