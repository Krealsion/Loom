// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_GRANT_WIRING_HPP
#define ZEN_HOST_GRANT_WIRING_HPP

// HOST WIRING — not part of the weave-authoring surface.
//
// This header exists to hold exactly one expression: the one that yields a
// `loom::GrantAuthority`. It is the same discipline `lifecycle_wiring.hpp`
// keeps, for the same reason — "where can the right to administer another
// subject's authority come from?" gets a one-file answer a reviewer can read in
// a minute and an auditor can grep for in one line.
//
// WHAT CHANGED, SAID EXACTLY (GRANT-0). Before this, a mounted subject's
// authority was fixed for its whole life: the host attached a `Grant` at
// admission and nothing in the system could rewrite one. The honest replacement
// for that sentence is three sentences, and losing any of them loses the model:
//
//     BASELINE authority enters at admission and never changes.
//     DELEGATED live authority may later be replaced, by a holder of a
//         host-minted capability, within a ceiling the host named.
//     EFFECTIVE authority — baseline union delegated — is what the bus checks,
//         at every delivery, at the moment of delivery.
//
// It is deliberately NOT "grants are now mutable". A `Grant` also carries
// containment policy that an isolated child consumed into a namespace, a mount
// view and a cgroup leaf before it ever ran; no write in this process moves any
// of that. Only the message half can honestly change, so only the message half
// is reachable here — see `loom::LiveAuthority`, which has no vocabulary for the
// rest.
//
// THE TIERS THIS KEEPS APART, none of which implies the next:
//
//   POSSESSING A SWITCHBOARD  mints authority for THAT Switchboard only
//   POSSESSING AN AUTHORITY   administers through its ISSUING Switchboard only,
//                             and only the ONE subject it names
//   POSSESSING A WIDE GRANT   permits speech, and confers no administration at
//                             all — `allow_any()` does not make a weave an
//                             administrator of anything, exactly as an
//                             `Emit<zen.Activated>` grant never made one an
//                             attestor of lifecycle (LIFE-04)
//   POSSESSING A Bus / Mail   implies no authority over the host Loom at all
//
// WHO USES THIS, and it is a short list on purpose:
//   - a host bootstrapping an administrator (a Weaver) for one governed session;
//   - test harnesses, which ARE hosts: they hold the Switchboard, so they are
//     inside the boundary by construction rather than by exemption.
//
// A weave may freely READ what it governs (`Mail::describe_authority`, scoped to
// its own subject). It may not mint the right to govern. Reading an authority
// and issuing one are different acts, and only the second is gated here.

#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <utility>

namespace loom {

/// Mint, for a host that owns `bus`, the right to administer `subject`'s
/// delegated live authority up to `ceiling`.
///
/// Requires the Switchboard by reference — which is the check, not a formality.
/// There is no other declaration of this name anywhere, and the Switchboard's
/// private mint has exactly one friend: this function.
///
/// `ceiling` is the host's decision and the holder's hard limit; the holder may
/// install any semantic subset of it on `subject` and nothing else, ever. It is
/// named separately from whatever grant the administrator itself carries,
/// because "what a Weaver may say" and "what a Weaver may hand out" are two
/// different questions and only the host gets to answer the second.
inline GrantAuthority host_grant_authority(Switchboard& bus, WeaveId subject,
                                           LiveAuthority ceiling) {
    return bus.grant_authority(subject, std::move(ceiling));
}

} // namespace loom

#endif // ZEN_HOST_GRANT_WIRING_HPP
