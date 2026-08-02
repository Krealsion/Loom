# loom — design

> **This manuscript moved.** The complete pre-consolidation design document —
> every subsystem's rationale, alternatives, intermediate laws and phase-by-
> phase reasoning, ~3,900 lines — is preserved **unabridged** at
> [`docs/history/pre-r2c/DESIGN.md`](docs/history/pre-r2c/DESIGN.md)
> (frozen at commit `78d64ea`). This file is now the concise map.

## The architecture, in one screen

```text
            values carry their shape ──► one gate admits at every boundary
                                              │
   ┌──────────────┬───────────────┬───────────┴────────┬──────────────────┐
 loom (core)  zen-switchboard  zen-kernel          zen-isolation      console/bridge
 schema/value  gated FIFO bus  dynamic weaves     out-of-process      the operator's
 gate/registry grants/answers  across a C ABI     OS sandbox          seat, local and
 serialization lifecycle/      sealed candidates  (abuse-tier,        remote
               replacement     hot-reload         confirmed, honest)
                                              │
                    the weave layer: ZEN_SHAPE · WeaveBase · Mail · mount
                    the host layer:  lifecycle authority · PreparedReplacement
```

Dispatch is single-threaded FIFO and non-reentrant; authority is minimal by
default and distinct from conformance; provenance (answers, lifecycle facts)
is a delivery fact with no wire form; a service is replaced by admitting a
**verified, sealed** successor whose admission and first breath are one queue
event.

## Where the truth lives now

| Question | Go to |
|---|---|
| teach me | [docs/guides/](docs/guides/mental-model.md) |
| exact current behavior | [docs/reference/](docs/reference/) |
| what must never become false | [docs/laws/](docs/laws/README.md) |
| why it is this way | [docs/decisions/](docs/decisions/README.md) · [docs/history/](docs/history/README.md) |
| what applications found | [docs/evidence/](docs/evidence/README.md) |
| every term, defined once | [docs/terminology.md](docs/terminology.md) |
| machine routing | [docs/CONTEXT.md](docs/CONTEXT.md) |

There is deliberately **one** normative current surface (reference + laws);
this file and the frozen manuscript are not it.
