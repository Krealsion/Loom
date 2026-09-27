// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_GRANT_WIRING_HPP
#define ZEN_HOST_GRANT_WIRING_HPP

// Host wiring, not part of the weave-authoring surface: the one expression that yields a
// `loom::GrantAuthority`, so where the right to administer another subject's authority comes
// from has a one-file answer. docs/reference/capabilities.md#live-delegation
//
// Baseline authority enters at admission and never changes; delegated live authority may be
// replaced by a holder of a host-minted capability, within the ceiling the host named; the
// bus checks their union at every delivery (GATE-05). Only the message half can change
// (`loom::LiveAuthority`): containment was consumed into a child's namespaces and cgroup
// before it ran.
//
//   a Switchboard     mints authority for that Switchboard only
//   an authority      administers through its issuing Switchboard, and one subject only
//   a wide grant      permits speech and confers no administration: `allow_any()` makes no
//                     weave an administrator (as an `Emit<zen.Activated>` grant makes none an
//                     attestor, LIFE-04)
//   a Bus or Mail     confers no authority over the host Loom
//
// For a host setting up an administrator (a Weaver) for one governed session, and for test
// harnesses, which hold the Switchboard and so are hosts. A weave may read what it governs
// (`Mail::describe_authority`); it cannot mint the right to govern.

#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <utility>

namespace loom {

/// Mint, for a host that owns `bus`, the right to administer `subject`'s delegated live
/// authority up to `ceiling`. Needing the Switchboard is the check: this is the one function
/// the Switchboard befriends to reach its private mint. The holder may install any subset of
/// `ceiling` on `subject` and nothing else; it is separate from the administrator's own grant.
inline GrantAuthority host_grant_authority(Switchboard& bus, WeaveId subject,
                                           LiveAuthority ceiling) {
    return bus.grant_authority(subject, std::move(ceiling));
}

} // namespace loom

#endif // ZEN_HOST_GRANT_WIRING_HPP
