// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A library whose describe() hands the host's byte sink more bytes than any string can hold. The
// sink is the host's code called from this library's frames, so what the host cannot hold must
// come back as the host's own refusal of the load, never as an exception through this library.

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

int instance = 0;

void* create() { return &instance; }
void destroy(void*) {}

ZenStatus describe(void*, ZenByteSink sink) {
    // The length alone is past what the host can hold, so no byte past the first is read.
    const std::uint8_t byte = 0;
    sink.write(sink.ctx, &byte, std::numeric_limits<std::size_t>::max());
    return ZEN_OK;
}

} // namespace

// By name, in declaration order (KERN-04). Only the doors a load reaches before reading the
// manifest are given; the load is refused at the manifest.
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION,
                                    .create      = &create,
                                    .destroy     = &destroy,
                                    .describe    = &describe,
                                    .snapshot    = nullptr,
                                    .policy      = nullptr,
                                    .revive      = nullptr,
                                    .handle      = nullptr,
                                    .claim_published = nullptr};
    return &abi;
}
