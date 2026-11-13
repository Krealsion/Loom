// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// An image compiled against ABI 12's header, kept byte for byte with its hash (tests/abi_kept/),
// so its descriptor has that version's layout and number: the last image a host refuses for being
// from before the tables said their size. Built with the kept header first on its include path,
// and nothing else of Loom's. Every slot ends the process, so a host that called one before
// refusing the image fails loudly rather than passing.

#include <zen/kernel/abi.h>

#include <cstdlib>

static_assert(ZEN_ABI_VERSION == 12u, "this image is built against ABI 12's kept header");

namespace {

void* create(ZenByteSink) { std::abort(); }
void destroy(void*) { std::abort(); }
ZenStatus emit(void*, ZenByteSink) { std::abort(); }
ZenStatus revive(void*, const uint8_t*, size_t) { std::abort(); }
ZenStatus handle(void*, uint64_t, uint64_t, uint64_t, uint32_t, int64_t, const char*,
                 const uint8_t*, size_t, const ZenHostApi*) {
    std::abort();
}
ZenStatus claim_published(void*, const uint8_t*, size_t) { std::abort(); }

} // namespace

// By name, in declaration order (KERN-04), as ABI 12's tables are laid out.
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION,
                                    .create = create,
                                    .destroy = destroy,
                                    .describe = emit,
                                    .snapshot = emit,
                                    .policy = emit,
                                    .revive = revive,
                                    .handle = handle,
                                    .claim_published = claim_published};
    return &abi;
}
