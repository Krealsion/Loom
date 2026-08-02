# Prepared readiness is authenticated conversation, not host assertion

**Context.** A replacement transaction must learn that its candidate is ready.
The scaffolding version was a host call — `mark_candidate_ready` — that simply
declared it.

**Decision.** Readiness is the *consumption of the candidate's own
authenticated answer* to the transaction's one preparation ask. The transition
into `Ready` is private; its only caller is the acceptance of a delivery the
bus proves is: an authentic answer, from the exact sealed candidate, to this
transaction's exact ask, heard by the exact coordinator. The deciding fact is
the ask's own bus-private envelope identity — never the correlation.

**Alternatives considered.**
- *Keep the host assertion* — rejected: "trust me, it's ready" is precisely
  the claim a verified-successor ceremony exists to remove.
- *Match on correlation* — rejected: a correlation is a number a sender
  chooses; believing it means believing the candidate answered *some* question
  numbered N, and the gap is reachable through the honest API (a coordinator
  can hand-address an ordinary ask with the same number).
- *A second readiness door for deferred answers* — rejected: immediate and
  deferred readiness ride the same rails and are one definition, or they
  drift.

**Consequences.** The bus authenticates **that**; the coordinator maps
**what** (the domain payload) onto Ready/Refused — trusted for semantics, not
for authenticity. Payloads need no transaction id. Hostile offers refuse the
command and can never terminalize someone else's promise.

**Laws supported.** [PR-04](../laws/replacement-laws.md),
[ANS-05](../laws/answer-authority-laws.md).

**Evidence / history.** R2B-3b-3 in [history](../history/README.md); the
forged-readiness cases in `tests/test_kernel.cpp`; six Night Lab coordinators
used the mapping surface unchanged ([evidence](../evidence/night-lab.md)).
