# Admission and committed activation share one queue boundary

**Context.** Admission once moved topology inside the commit call and queued
the activation as an ordinary gated send stamped as the coordinator. The
message was then authorized *later* — against a grant, a sender life and a
seal the commit could no longer guarantee. Demonstrated result: a coordinator
without an `Emit<zen.Activated>` grant committed successfully and its
successor was publicly the service, never told it was alive.

**Decision.** One envelope IS the admission and IS the activation. The commit
request *schedules* it (`AdmitResult.scheduled`; the transaction becomes
`AdmissionPending`); its dispatch — one queue turn, nothing between any two
steps — revalidates every exact participant, admits the activation through the
candidate's own door and gate, **then** moves topology, terminalizes
`Committed`, and delivers. A candidate that cannot receive its activation is
not admissible. The activation is Loom's act: ungated, lifecycle-authorized,
no ordinary grant consulted.

**Alternatives considered.**
- *Prevalidate the grant, keep committing synchronously* — rejected: "valid
  when checked" is insufficient while the coordinator's life, the seal and the
  candidate remain mutable before delivery.
- *A committed-but-activating barrier state* — rejected: it must park real
  production somewhere and would exist only to preserve the old call shape.
- *Synchronous delivery inside the commit call* — rejected: commit is
  routinely called from inside a handler; that is reentrant delivery, which
  the dispatch model has no meaning for.

**Consequences.** "Publicly admitted but never told" is unrepresentable, not
avoided. The boundary became a *position in the queue*: traffic ahead of the
envelope belongs to the old world, behind it to the new — one rule instead of
two. The cost is paid out loud as `AdmissionPending`, abortable, bounded.

**Laws supported.** [PR-05, PR-07, PR-08](../laws/replacement-laws.md),
[LIFE-05](../laws/lifecycle-laws.md).

**Evidence / history.** R2B-3d in [history](../history/README.md); the
original missing-grant reproducer lives on as a regression case in
`tests/test_kernel.cpp`.
