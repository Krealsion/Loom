#ifndef ZEN_HOST_LIFECYCLE_WIRING_HPP
#define ZEN_HOST_LIFECYCLE_WIRING_HPP

// HOST WIRING — not part of the weave-authoring surface.
//
// This header exists to hold exactly one expression: the one that yields a
// `loom::LifecycleAuthority`. It is deliberately the whole of its contents, so
// that "where can lifecycle authority come from?" has a one-file answer that a
// reviewer can read in a minute and an auditor can grep for in one line.
//
// WHY IT IS A SEPARATE HEADER, and why that is not merely a rename. R2B-1 put
// the mint on `Switchboard` as a PUBLIC STATIC, reachable from every weave that
// includes `zen/switchboard.hpp` — which is every native weave. Moving it here
// removes it from the authoring surface.
//
// BUT DO NOT OVERSTATE THE SECOND WALL (corrected in R2B-2). An earlier version
// of this comment said a weave "has nothing to pass it", as though a weave could
// not obtain a Switchboard at all. That is false, and the falsehood mattered:
//
//     An ordinary weave MAY construct another Switchboard and mint genuine
//     authority there. That authority has no standing in the running Loom.
//
// Constructing a board is ordinary — a Switchboard is an ordinary object. What a
// weave cannot do is make its own board be THIS one. The protection is not
// scarcity of boards; it is that an authority names its issuer and the issuer
// checks (R2B-1b, `Switchboard::issued_here`). Every Loom is its own authority
// domain, and an authority minted elsewhere is real — elsewhere.
//
// That is the same line this codebase has always drawn. `Switchboard::send` is
// root authority and `send_as` is a weave speaking as itself. Lifecycle minting
// now sits on the correct side of a boundary that already existed.
//
// BUT SAY IT PRECISELY (R2B-1b). The earlier wording here — "holding the
// Switchboard IS being the host" — was too broad, and the gap was real:
//
//     Holding a Switchboard grants host authority only within THAT
//     Switchboard's Loom.
//
// Constructing a separate board creates a separate authority domain, and an
// authority minted from it has no standing anywhere else. Anyone may own a
// Switchboard; nobody thereby owns yours. Every Loom is its own authority
// domain, and an authority carries which one issued it.
//
// THE TIERS THIS KEEPS APART, none of which implies the next:
//
//   POSSESSING A SWITCHBOARD  mints authority for THAT Switchboard only
//   POSSESSING AN AUTHORITY   attests through its ISSUING Switchboard only
//   POSSESSING AN EXACT GRANT permits the shape, never lifecycle provenance
//   POSSESSING A Bus / Mail   implies no authority over the host Loom at all
//
// WHO USES THIS, and it is a short list on purpose:
//   - `loom::mount_control` (kernel/control.hpp) — the one production caller,
//     handing the kernel's control door its authority at mount;
//   - host programs wiring an alternative lifecycle operator of their own;
//   - test harnesses, which ARE hosts: they hold the Switchboard, so they are
//     inside the boundary by construction rather than by exemption.
//
// A weave may freely INSPECT the provenance it receives (`Mail::answers_ask`,
// `Mail::lifecycle_attested`). It may not create it. Reading Loom's word and
// speaking as Loom are different acts, and only the second one is gated here.

#include <zen/switchboard/message.hpp>
#include <zen/switchboard/switchboard.hpp>

namespace loom {

/// Mint the lifecycle authority for a host that owns `bus`.
///
/// Requires the Switchboard by reference — which is the check, not a formality.
/// There is no other declaration of this name anywhere, and the Switchboard's
/// private mint has exactly one friend: this function.
inline LifecycleAuthority host_lifecycle_authority(Switchboard& bus) {
    return bus.lifecycle_authority();
}

} // namespace loom

#endif // ZEN_HOST_LIFECYCLE_WIRING_HPP
