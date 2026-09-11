// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A current-header fixture that CLAIMS the previous ABI version.
//
// It pins rejection before any descriptor callback, with a diagnostic naming
// both versions. Its claim follows ZEN_ABI_VERSION - 1 after every bump.
// A separately retained image compiled against the actual pre-change header
// supplies mixed-artifact evidence; this fixture supplies the version-gate
// ordering control. See docs/reference/dynamic-abi.md#compatibility-discipline.
//
// Its function pointers are deliberately null: if the host ever called one, the
// crash would be the test failing loudly rather than a silent pass.

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

// By name, in declaration order (KERN-04) — and note what it replaces: a
// `/*abi_version=*/` comment on the one field anybody reading this fixture cares
// about, and seven anonymous nulls after it. The comment was right, and nothing
// was checking that it stayed right.
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION - 1u,
                                    .create      = nullptr,
                                    .destroy     = nullptr,
                                    .describe    = nullptr,
                                    .snapshot    = nullptr,
                                    .policy      = nullptr,
                                    .revive      = nullptr,
                                    .handle      = nullptr};
    return &abi;
}
