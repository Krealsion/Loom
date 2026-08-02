# Lifecycle authority is Loom-owned

**Context.** `zen.Activated` is only worth trusting if forging it is
impossible for ordinary code. The first shipped design minted authority via a
public static on the Switchboard; the second required a `Switchboard&` but the
resulting token was an empty marker every board accepted.

**Decision.** Attestation is a capability: minted privately and non-statically
by the Switchboard, reachable through exactly one friend function in a
host-wiring header no weave-authoring header includes — and the authority
**names its issuing board**, which checks (`issued_here`). Anyone may own a
Switchboard; nobody thereby owns yours.

**Alternatives considered.**
- *Grant-based* ("an `Emit<zen.Activated>` grant is authority") — rejected:
  three tiers (shape / grant / authority) must never imply each other; a
  consumer could not tell an operator from a well-granted impostor.
- *Public static mint* — shipped, then removed: reachable from every weave, no
  boundary at all.
- *Un-scoped authority object* — shipped, then repaired: a weave could stand
  up a decoy board, mint a *genuine* authority there, and spend it here. Real
  authority — real somewhere else.

**Consequences.** Test harnesses are hosts (they own the bus) — inside the
boundary by construction; a library forging provenance flags lies only to
itself; every Loom is its own authority domain.

**Laws supported.** [LIFE-04](../laws/lifecycle-laws.md),
[ANS-07](../laws/answer-authority-laws.md).

**Evidence / history.** The two shipped-broken iterations are preserved in the
frozen [DESIGN.md](../history/pre-r2c/DESIGN.md) (R2B-1/1a/1b) — kept
precisely because both tells were in our own code.
