// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A library that exports a descriptor with an unsupported abi_version. The
// kernel must reject it cleanly, before calling any of its function pointers
// (which are therefore left null).

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

// Written out by hand rather than through ZEN_EXPORT_WEAVE, because the whole
// point is a descriptor the macro would never produce. By name, in declaration
// order (KERN-04): a hand-written descriptor is exactly where a positional drift
// would go unnoticed, and naming every door keeps this fixture's "the version is
// the ONLY thing wrong here" claim checkable instead of asserted.
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION + 1000u,
                                    .create      = nullptr,
                                    .destroy     = nullptr,
                                    .describe    = nullptr,
                                    .snapshot    = nullptr,
                                    .policy      = nullptr,
                                    .revive      = nullptr,
                                    .handle      = nullptr};
    return &abi;
}
