# Admission and committed activation share one queue boundary

**Context.** An admission that moves topology inside the commit call and
queues the activation as an ordinary gated send stamped as the coordinator has
that message authorized *later* — against a grant, a sender life and a seal the
commit can no longer guarantee. Demonstrated result: a coordinator without an
`Emit<zen.Activated>` grant commits successfully and its successor is publicly
the service, never told it is alive.

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
  production somewhere and would exist only to keep the commit call synchronous.
- *Synchronous delivery inside the commit call* — rejected: commit is
  routinely called from inside a handler; that is reentrant delivery, which
  the dispatch model has no meaning for.

**Consequences.** "Publicly admitted but never told" is unrepresentable, not
avoided. The boundary is a *position in the queue*: traffic ahead of the
envelope belongs to the old world, behind it to the new — one rule instead of
two. The cost is paid out loud as `AdmissionPending`, abortable, bounded.

**Laws supported.** [PR-05, PR-07, PR-08](../laws/replacement-laws.md),
[LIFE-05](../laws/lifecycle-laws.md).

**Evidence / history.** The missing-grant reproducer is a regression case in
`tests/test_kernel.cpp`; how it came to be, in [history](../history/README.md).
