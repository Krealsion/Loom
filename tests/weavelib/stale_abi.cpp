// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A library built against the PREVIOUS ABI — a deliberately stale artifact.
//
// ZEN_ABI_VERSION went 1 -> 2 so that a delivery's provenance can cross
// the library seam beside its sender. That break is paid deliberately, and this
// fixture is the proof that it is paid HONESTLY: an artifact compiled before the
// change is refused at load, with a reason that names the version, before any of
// its function pointers is called.
//
// The alternative shape of the change — an appended host callback that older
// libraries simply never ask about — would have loaded this artifact happily and
// left it permanently unable to accept an attested activation: loaded, running,
// and silently inert. A refusal that says why is the better failure, and this
// case is what keeps that claim true.
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
