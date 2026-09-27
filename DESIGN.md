# Loom — design

This is the map of Loom's design. The earlier complete design manuscript — every subsystem's
rationale and the alternatives weighed, about 3,900 lines — is kept frozen at
[`docs/history/pre-r2c/DESIGN.md`](docs/history/pre-r2c/DESIGN.md); it describes the tree it was
written against, not this one.

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

Dispatch is single-threaded, first-in first-out and never re-entrant. Authority is minimal by
default and separate from conformance: passing the gate grants nothing. Provenance — that a
message answers an ask, or states a lifecycle fact — is a fact of delivery with no wire form a
sender could forge. A service is replaced by admitting a **verified, sealed** successor, whose
admission and first delivery are one queue event.

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

There is deliberately **one** normative current surface, the reference and the laws; this map
and the frozen manuscript are not it.
