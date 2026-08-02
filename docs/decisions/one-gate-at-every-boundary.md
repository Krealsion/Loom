# One gate at every boundary

**Context.** A value crosses many thresholds: bus delivery, persistence,
dynamic-library seams, IPC, the operator bridge. Each could validate its own
way — or one authority could judge them all.

**Decision.** `loom::admit()` is the sole conformance authority. Every
boundary funnels through it; unvalidated bytes (`Unverified`) have no
accessors; authorization (grants) is a distinct step *around* the gate, never
folded in.

**Alternatives considered.**
- *Per-boundary validators* — rejected: N subtly-different definitions of
  "well-formed" drift apart, and the security story becomes the weakest one.
- *A content-id fast path* ("we already validated this shape once") —
  rejected, deliberately untaken, so "one gate, every delivery" stays
  literally true and countable.
- *Folding grant checks into the gate* — rejected: "may you say this" and "is
  this well-formed" send an operator to different fixes, and conflating them
  is how authorization bugs hide inside schema errors.

**Consequences.** A counter (`gate_invocations()`) can prove coverage in
tests; hostile bytes are inert everywhere by the same argument; the gate's
cost is paid at every boundary (accepted — the boundary is where Zen pays).

**Laws supported.** [GATE-01..04](../laws/admission-laws.md),
[KERN-01](../laws/kernel-laws.md).

**Evidence / history.** The spine section of the frozen
[DESIGN.md](../history/pre-r2c/DESIGN.md); every suite's gating cases.
