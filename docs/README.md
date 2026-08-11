# Loom documentation

**I am new.** Start with the [mental model](guides/mental-model.md), then
[write your first weave](guides/writing-a-weave.md) — working code in two
pages, no history required.

**I am implementing something.**
[Messaging](guides/messaging.md) ·
[dynamic weaves](guides/dynamic-weaves.md) ·
[replacing a service](guides/replacing-a-service.md).
Timers are not Loom's: they are the Timer *package*, owned by the separate
**Zengine** repository, at `Zengine/docs/`. Zengine-owned pages are named that
way throughout and never linked — no page under `docs/` reaches into a sibling
checkout, so this tree is readable from a Loom clone alone.

**I need exact semantics.** [reference/](reference/) — one page per subsystem:
[values & admission](reference/values-and-admission.md),
[messaging](reference/messaging.md), [lifecycle](reference/lifecycle.md),
[capabilities](reference/capabilities.md), [kernel](reference/kernel.md),
[prepared replacement](reference/prepared-replacement.md),
[dynamic ABI](reference/dynamic-abi.md), [bridge](reference/bridge.md),
[weaver](reference/weaver.md), [terminal](reference/terminal.md),
[bounds](reference/bounds.md),
[known seams](reference/known-seams.md). Words are defined once, in the
[terminology index](terminology.md).

**I am about to expose something.** Two pages carry the trust boundaries, and
both say plainly what they do *not* claim:
[capabilities](reference/capabilities.md) (in-process trust, the OS sandbox,
the exec boundary) and [bridge](reference/bridge.md) (the remote-operator
socket — **it does not authenticate**).

**I am debugging behavior.** [guides/diagnostics.md](guides/diagnostics.md)
first; the invariants themselves are the [laws](laws/README.md) — small, named
(GATE/MSG/ANS/LIFE/PR/SENSE/HANDOFF/KERN, and POP for what a green *test* run
means), each stating what it does *not* mean.

**I need to know why.** [decisions/](decisions/README.md) for the choices that
would otherwise be re-litigated; [history/](history/README.md) for the frozen
manuscripts and the phase chronology.

**I want experimental evidence.** [evidence/](evidence/README.md) — Night Lab
and the audits: what real applications discovered, distinct from what the API
promises.

Machine collaborators: start at [CONTEXT.md](CONTEXT.md).
