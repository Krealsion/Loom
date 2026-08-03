// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A library built against the PREVIOUS ABI — the stale artifact R2B-1 creates.
//
// R2B-1 bumped ZEN_ABI_VERSION 1 -> 2 so that a delivery's provenance can cross
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

extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {/*abi_version=*/ZEN_ABI_VERSION - 1u, nullptr, nullptr,
                                    nullptr,
                                    nullptr,            nullptr, nullptr, nullptr};
    return &abi;
}
