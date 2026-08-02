# Diagnostics — where to look when something refuses

Zen never drops silently: every refused delivery is a structured, observable
event. This page is the "it didn't arrive" checklist.

## 1. Put a tap on the bus

```cpp
bus.add_observer([](const loom::BusEvent& e) {
    if (e.kind == loom::EventKind::Refused) {
        std::cout << e.schema_name << " -> " << e.refusal.message() << "\n";
    }
});
```

`BusEvent` carries the target, the stamped sender, the schema, and — on the
lifecycle-flavored refusals — the expected vs. current life/incarnation, so
you can *see* "the asker was replaced" instead of inferring it.

## 2. Read the reasons as directions

| Reason | It means | You probably |
|---|---|---|
| `NotAccepted` | the target never declared that shape | add it to `Accept<...>` |
| `CapabilityDenied` | the sender's grant doesn't permit shape→target | ask the host for reach |
| `GateRefused` | malformed instance (field path + expected/actual attached) | fix the payload/shape version |
| `NoSuchTarget` | unknown id, unheld role — or a sealed candidate you cannot know exists | check who holds the role |
| `TargetUnavailable` | the target is dead, awaiting revival | supervise / revive |
| `SenderLifeEnded` | the author's life ended before delivery | expected after kill/revive |
| `AnswerTargetChanged` | the asker was replaced; the answer refused rather than misdelivered | re-ask from the new life |
| `SealedSpeech` | a prepared candidate reached for the world | that's the operator's business to see |
| `ForeignAuthority` | an authority another Loom issued | you're on the wrong board |
| `Exhausted` | a published bound ([which ones](../reference/bounds.md)) | release/redesign, not retry |
| `AdmissionRevoked` | a scheduled admission met a drifted world — **nothing changed** | read the transaction outcome |

The full delivery-order ladder is in
[reference/messaging](../reference/messaging.md).

## 3. The journal, for host code

`Ticket t = bus.send(...)` → after pumping, `bus.outcome(t)` reads the
recorded disposition (last 1024 deliveries retained). Weave code cannot do
this — a sender does not observe its send's fate; observers do
([the seam](../reference/known-seams.md#sender-cannot-observe-send-fate)).

## 4. Replacement outcomes

For a prepared replacement, the truth is the transaction:
`upgrade.state()` and `upgrade.take_outcome()` — the `TxnReason` vocabulary is
exact on purpose (`CandidateRefused` means the coordinator mapped the
candidate's authentic domain answer to Refused; `IncumbentBusy` means someone
is already replacing that service; …). See
[replacing a service](replacing-a-service.md#when-something-refuses).

## 5. Live inspection

A weave that opts in (`ZEN_EXPOSE`) can be inspected and adjusted **by
message** from the console — see the Poke doors in
[reference/messaging](../reference/messaging.md#observation).
