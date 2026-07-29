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
// removes it from the authoring surface; making it need a `Switchboard&`
// removes it from a weave's reach even if the weave includes this file anyway.
// The second wall is the load-bearing one: a weave is handed a `Bus&`, never
// the Switchboard, so the argument this function requires is one no weave can
// produce. Include it all you like; you still have nothing to pass it.
//
// That is the same line this codebase has always drawn. `Switchboard::send` is
// root authority and `send_as` is a weave speaking as itself; holding the
// Switchboard IS being the host. Lifecycle minting now sits on the correct side
// of a boundary that already existed and is already understood.
//
// THE TIERS THIS KEEPS APART, none of which implies the next:
//
//   PUBLIC SHAPE        ordinary code may represent `zen.Activated`
//   EXACT GRANT         ordinary code may be permitted to EMIT it
//   LIFECYCLE AUTHORITY only Loom infrastructure may ATTEST it as a lifecycle fact
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
