// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A current-header fixture that CLAIMS the previous ABI version (ZEN_ABI_VERSION - 1, after
// every bump): it pins rejection before any descriptor callback, with a diagnostic naming both
// versions (docs/reference/dynamic-abi.md#compatibility-discipline). Its function pointers are
// null on purpose: a host that called one would crash, failing loudly rather than passing.

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

// By name, in declaration order (KERN-04), so the one field that matters here, abi_version, is
// named where the compiler checks it.
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION - 1u,
                                    .create      = nullptr,
                                    .destroy     = nullptr,
                                    .describe    = nullptr,
                                    .snapshot    = nullptr,
                                    .policy      = nullptr,
                                    .revive      = nullptr,
                                    .handle      = nullptr,
                                    .claim_published = nullptr};
    return &abi;
}
