# Committed activation is not answerable

**Context.** The committed activation's delivery context was first built in
the ordinary path's image — which fabricated a requester. The stamped sender
is the *operator that admitted*, not a weave that asked anything; a candidate
could therefore `answer()` its own first breath and queue a real,
provenance-carrying answer to a question nobody asked, or `defer_answer()` and
park a slot of the Loom-wide bounded registry for a conversation that did not
exist.

**Decision.** The activation handler holds **no** reply authority. The model
already had the category — `answer_as`/`defer_answer_as` refuse when there is
no valid requester ("the request came from a root") — and Loom's own act
belongs in it. The whole repair is `authority_ = ReplyAuthority{}` at the
admission dispatch.

**Alternatives considered.**
- *Keep it answerable* ("harmless — nobody listens") — rejected: the answer
  carries genuine `answers_ask()` provenance to a coordinator that never
  spoke, and a deferred one consumes shared bounded capacity.
- *A new refusal category / API for activation* — rejected: the no-requester
  category already existed; new machinery would be a second definition of an
  old truth.
- *Strip the attestation too* — rejected (and pinned red as a mutation):
  activation must stay authentic; only the fabricated *conversation* goes.

**Consequences.** Not-answerable is not mute: ordinary sends from inside the
handler work — a prepared Timer's first act is an ordinary claim send, and the
entire handoff depends on it. A later real ask is answerable normally;
authority is scoped per delivery, never removed from the weave. The
distinction is provenance, not payload shape: an *ordinary* `zen.Activated`
message remains answerable like any delivery.

**Laws supported.** [LIFE-05](../laws/lifecycle-laws.md),
[ANS-01](../laws/answer-authority-laws.md).

**Evidence / history.** R2B-3d-1 in [history](../history/README.md); the
"first breath is not a question" cases in `tests/test_kernel.cpp`, including
the deferred-capacity proof run with the registry held one slot from full.
