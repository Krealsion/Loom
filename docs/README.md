# Loom documentation

**I am new.** Start with the [mental model](guides/mental-model.md), then
[write your first weave](guides/writing-a-weave.md) — working code in two
pages, no history required.

**I am implementing something.**
[Messaging](guides/messaging.md) ·
[dynamic weaves](guides/dynamic-weaves.md) ·
[replacing a service](guides/replacing-a-service.md) ·
timers live in [Zengine's guides](../../Zengine/docs/README.md).

**I need exact semantics.** [reference/](reference/) — one page per subsystem:
[values & admission](reference/values-and-admission.md),
[messaging](reference/messaging.md), [lifecycle](reference/lifecycle.md),
[capabilities](reference/capabilities.md), [kernel](reference/kernel.md),
[prepared replacement](reference/prepared-replacement.md),
[dynamic ABI](reference/dynamic-abi.md), [bounds](reference/bounds.md),
[known seams](reference/known-seams.md). Words are defined once, in the
[terminology index](terminology.md).

**I am debugging behavior.** [guides/diagnostics.md](guides/diagnostics.md)
first; the invariants themselves are the [laws](laws/README.md) — small, named
(GATE/MSG/ANS/LIFE/PR/KERN), each stating what it does *not* mean.

**I need to know why.** [decisions/](decisions/README.md) for the choices that
would otherwise be re-litigated; [history/](history/README.md) for the frozen
manuscripts and the phase chronology.

**I want experimental evidence.** [evidence/](evidence/README.md) — Night Lab
and the audits: what real applications discovered, distinct from what the API
promises.

Machine collaborators: start at [CONTEXT.md](CONTEXT.md).
