// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The addressed-role probe: a weave keeping in its state the role each knock was addressed to and
// the role it was authored as, read through `Mail`; built native, boxed and isolated alike.
// ADDRESSED_PROBE_SMALLER_TABLES gives the same weave a descriptor saying it was built against
// smaller tables: its size ends at `handle`, and the host table it names before `sense_offer`.
// The slot past its size ends the process, so a host reading past the size it was told fails
// loudly.

#include "addressed_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>

using namespace loom;
using namespace addressed;

namespace {

class AddressedProbe final
    : public WeaveBase<AddressedProbe, AddressedHeard, Accept<AddressedKnock>> {
public:
    void on(const AddressedKnock&, Mail& mail) {
        state_.addressed = std::string(mail.addressed_role());
        state_.authored = std::string(mail.authored_role());
        ++state_.knocks;
    }
};

} // namespace

#if defined(ADDRESSED_PROBE_SMALLER_TABLES)

extern "C" {
static void* probe_create(ZenByteSink r) { return detail::do_create<AddressedProbe>(r); }
static void probe_destroy(void* i) { detail::do_destroy<AddressedProbe>(i); }
static ZenStatus probe_describe(void* i, ZenByteSink s) {
    return detail::do_describe<AddressedProbe>(i, s);
}
static ZenStatus probe_snapshot(void* i, ZenByteSink s) {
    return detail::do_snapshot<AddressedProbe>(i, s);
}
static ZenStatus probe_policy(void* i, ZenByteSink s) {
    return detail::do_policy<AddressedProbe>(i, s);
}
static ZenStatus probe_revive(void* i, const uint8_t* st, size_t n) {
    return detail::do_revive<AddressedProbe>(i, st, n);
}
static ZenStatus probe_handle(void* i, const ZenDelivery* d, const uint8_t* p, size_t n,
                              const ZenHostApi* h) {
    return detail::do_handle<AddressedProbe>(i, d, p, n, h);
}
static ZenStatus probe_past_its_size(void*, const uint8_t*, size_t) { std::abort(); }

// By name, in declaration order (KERN-04).
ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {
        .abi_version = ZEN_ABI_VERSION,
        .size = static_cast<uint32_t>(offsetof(ZenWeaveAbi, handle) + sizeof(ZenWeaveAbi::handle)),
        .host_size = static_cast<uint32_t>(offsetof(ZenHostApi, sense_offer)),
        .manifest_version = kManifestVersion,
        .create = probe_create,
        .destroy = probe_destroy,
        .describe = probe_describe,
        .snapshot = probe_snapshot,
        .policy = probe_policy,
        .revive = probe_revive,
        .handle = probe_handle,
        .claim_published = probe_past_its_size};
    return &abi;
}
} // extern "C"

#else

ZEN_EXPORT_WEAVE(AddressedProbe)

#endif
