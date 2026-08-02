# No rollback after committed production

**Context.** Every replacement design must answer: what happens when things go
wrong *late*? Once a successor has been admitted and production has reached
it, the world has acted on its answers.

**Decision.** The failure direction is **refuse-before, never unwind-after**.
Every fallible step happens while the incumbent is still whole (preparation,
readiness, revalidation at dispatch); a refusal at any of them leaves the
incumbent exactly as it was — that is the guarantee. After the admission
dispatch, there is no substrate rollback: a candidate dying post-activation is
a *post-commit lifecycle event* (supervision's business), never a retroactive
admission failure.

**Alternatives considered.**
- *Transactional rollback of a committed admission* — rejected: production
  delivered to the successor cannot be undelivered; "rolling back" would mean
  inventing compensation semantics the substrate cannot know, and pretending
  the world had not acted.
- *Two-phase visible cutover* (expose the successor, then confirm) — rejected:
  it creates exactly the publicly-admitted-but-provisional window the
  admission boundary exists to remove.

**Consequences.** The distinction "activation could not be delivered →
admission failed" vs. "activation was delivered, candidate later died →
post-commit event" is sharp and testable. Applications wanting undo build it
in their domain (idempotency, re-derivation, obligation snapshots — see the
continuity guideline). Reload-in-place keeps its own honest remainder: it is
validate-then-commit, not transactional (a post-rebind revival failure leaves
the weave unavailable, stated in its reference).

**Laws supported.** [PR-07..09](../laws/replacement-laws.md),
[KERN-02](../laws/kernel-laws.md) (exact-once cleanup on the failure side).

**Evidence / history.** The failure-ladder cases in `tests/test_kernel.cpp`;
Night Lab's six applications each authored their own late-failure answer
([evidence](../evidence/night-lab.md)).
