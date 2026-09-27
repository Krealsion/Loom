// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_LIFECYCLE_WIRING_HPP
#define ZEN_HOST_LIFECYCLE_WIRING_HPP

// Host wiring, not part of the weave-authoring surface: the one expression that yields a
// `loom::LifecycleAuthority`, kept apart from zen/switchboard.hpp, which every native weave
// includes, so where lifecycle authority comes from has a one-file answer.
//
// This is not scarcity: any weave may construct a Switchboard of its own and mint real
// authority there. That authority names its issuer, and the running Loom's Switchboard refuses
// it (`Switchboard::issued_here`): holding a Switchboard grants host authority within that
// Switchboard's Loom only.
// LIFE-04; docs/laws/lifecycle-laws.md
//
//   a Switchboard     mints authority for that Switchboard only
//   an authority      attests through its issuing Switchboard only
//   an exact grant    permits the shape, never lifecycle provenance
//   a Bus or Mail     confers no authority over the host Loom
//
// Used by `loom::mount_control` (kernel/control.hpp), by hosts wiring a lifecycle operator of
// their own, and by test harnesses, which hold the Switchboard and so are hosts. A weave may
// inspect the provenance it receives (`Mail::answers_ask`, `Mail::lifecycle_attested`); it
// cannot create it.

#include <zen/switchboard/message.hpp>
#include <zen/switchboard/switchboard.hpp>

namespace loom {

/// Mint the lifecycle authority for a host that owns `bus`. Needing the Switchboard is the
/// check: this is the one function the Switchboard befriends to reach its private mint.
/// private mint has exactly one friend: this function.
inline LifecycleAuthority host_lifecycle_authority(Switchboard& bus) {
    return bus.lifecycle_authority();
}

} // namespace loom

#endif // ZEN_HOST_LIFECYCLE_WIRING_HPP
