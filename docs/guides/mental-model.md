# The mental model

Zen composes other people's native code safely. The Loom is the shared
substrate everyone stands on; a **weave** is the thing you make — a
participant on one in-process bus, whole and part at once.

A weave answers four questions, and the framework holds you to them:

```text
What am I?               a struct of state, with a shape
What may I hear?         Accept<...> — my declared doors
What may I say?          Emit<...> + my grant — declared and authorized
What do I do?            on(const Shape&, Mail&) handlers
```

Three different questions decide whether a message means anything, and they
never collapse into each other:

- **Shape** — is it a well-formed instance of what it claims?
  One gate answers, at every boundary
  ([GATE-01](../laws/admission-laws.md)).
- **Sender** — which weave said it? The bus stamps it; payloads cannot lie
  about it ([MSG-02](../laws/messaging-laws.md)).
- **Provenance** — does Loom itself vouch for its standing (an answer to my
  ask; a lifecycle fact)? A delivery fact, never a payload field
  ([ANS-01](../laws/answer-authority-laws.md)).

Dispatch is single-threaded FIFO: nothing runs until the host `pump()`s, a
handler's sends become later deliveries, and there are no threads to fear
([MSG-01](../laws/messaging-laws.md)). Everything observable is observable —
refusals are named events, not silence.

Authority is minimal by default: a fresh weave may say nothing until the host
grants it reach. Holding a `Mail` is being *a weave*; holding the
`Switchboard` itself is being *the host* — the two surfaces are the trust
boundary ([capabilities](../reference/capabilities.md)).

Where next: [write your first weave](writing-a-weave.md) · then
[messaging](messaging.md). Exact semantics live in
[reference](../reference/), invariants in [laws](../laws/README.md).
