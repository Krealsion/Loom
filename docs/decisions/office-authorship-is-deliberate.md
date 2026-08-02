# Office authorship is deliberate, not inferred

**Context.** Applications repeatedly needed to trust *an office*, not merely an
occupant: Night Lab produced five independent sightings, escalating to a forged
`WorkerOpen` destroying healthy work and a player joining an attacker's server
on a forged `MatchCreated`. The missing fact has two load-bearing halves —
*authorization* (the author actually held R) and *intent* (this statement was
deliberately spoken as R) — because the same weave, holding the same office,
may speak personally or for it, and those are different statements.

**Decision.** Role authorship is an explicit per-statement act
(`mail.as_role(R).send/send_to_role/publish`), verified by Loom at the
**authorship moment** (`role_holder(R) == sender`, at enqueue), and carried as
immutable delivery provenance on a second axis beside answer/activation
standing. Delivery never recomputes it; refusal (`RoleAuthorshipDenied`)
queues nothing and is never a silent downgrade to personal speech
([MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit)).

**Alternatives considered.**
- *Infer from the current holder at receipt* — rejected: topology may move
  while a statement waits in the queue, so historical speech would change
  meaning; a recipient would also be reading current membership and calling it
  authorship.
- *Infer from the holder at send, for every message* — rejected: personal
  speech becomes office speech automatically, which authenticates exactly the
  statements the lobby evidence needed to reject. (The mandatory mutation for
  this cut reddens the same-holder case and the definition-of-done program.)
- *Put the role in the payload* — rejected: forgeable application data; a
  payload `role = R` buys zero provenance, pinned as such.
- *A registry announcing who owns the role* — rejected: the registry's own
  belief rests on an unauthenticated announcement — the problem restated, not
  solved (built and priced in Night Lab).
- *Invert every push into an authenticated pull* — rejected on measured cost:
  it spends the Loom-wide deferred-answer bound, strands strict receivers
  across honest replacement (they refuse the honest successor for exactly the
  reason they refuse a forger), and cannot cover observers of publications at
  all.

**Consequences.** A strict receiver writes `if
(!mail.authored_from_role("matchmaker")) return;` with no Switchboard access,
no role lookup, and no payload identity field. The office survives its
officeholder without pretending successor identity equals predecessor
identity. Nothing widens: the ordinary grant, sender-life, seal, and routing
laws all still refuse independently. The `as_role()` view is syntax, not
stored authority — every emission re-verifies, and the type resists being
kept (non-copyable, rvalue-qualified verbs).

**Laws supported.**
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit),
[MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-office).

**Evidence / history.** R2D-0 in [history](../history/README.md); the priced
workarounds and five sightings in
[night-lab evidence](../evidence/night-lab.md); the focused replay
`playground/night-lab/followups/role-authorship/`; suites `role_authorship`,
`kernel` (ABI v5 parity), `isolation` (fail-closed pipe).
