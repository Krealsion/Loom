# Laws — what must never become false

A **law** here is a small, named, locally complete architectural invariant of the
*current* system. Laws are the machine-friendliest surface Zen has: each one can
be retrieved alone and still make sense, each carries what it **means**, what it
**does not mean**, and where it is **proven**.

**"Proven" means a regression test asserts it.** A property that holds only by
reading the code is *"true by construction, not yet pinned"* — Zen's docs say it
that way, never "proven". (The project's own ethos applied to its prose: never
claim an enforcement, or a proof, you did not earn.)

## Namespaces

| Prefix | File | Owns |
|---|---|---|
| `GATE-xx` | [admission-laws.md](admission-laws.md) | the gate, schemas, untrusted-until-proven |
| `MSG-xx` | [messaging-laws.md](messaging-laws.md) | the Switchboard: dispatch, identity, roles, refusals |
| `ANS-xx` | [answer-authority-laws.md](answer-authority-laws.md) | answer authority, deferral, provenance |
| `LIFE-xx` | [lifecycle-laws.md](lifecycle-laws.md) | `zen.Activated`, lifecycle authority, and lifetime — of a weave through its callback, and of bytes in a transport buffer |
| `PR-xx` | [replacement-laws.md](replacement-laws.md) | the candidate seal, the transaction, admission |
| `SENSE-xx` | [sense-laws.md](sense-laws.md) | latest claims: settlement, provenance, office claims, authority, lifetime |
| `HANDOFF-xx` | [handoff-laws.md](handoff-laws.md) | authored continuity across an incompatible schema: migration, the FIFO boundary, protocol vs state |
| `KERN-xx` | [kernel-laws.md](kernel-laws.md) | dynamic artifacts, lifetimes, role truth |
| `POP-xx` | [population-laws.md](population-laws.md) | what a green result means: suite/case/enforcement populations, declared absence, the opt-out |
| `TIMER-xx` | **Zengine repo** — `docs/laws/timer-laws.md` (a separate repository; not in a Loom checkout) | Timer continuity (Zengine owns that truth) |

Identifiers are stable and deliberately carry **no phase coordinates** — phase
names belong to [history](../history/README.md), which each law may cite as
rationale.

## Reading a law

```text
LAW            the invariant, one or two sentences
MEANS          what it concretely guarantees
DOES NOT MEAN  the misreadings we paid real work to rule out
PROVEN BY      source location + the asserting test suite (+ evidence)
```

Negative guarantees ("does not mean") are first-class: most of them were
discovered the hard way, and their whole value is that a future reader does not
rediscover them.
