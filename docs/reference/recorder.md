# Recorder — the host's memory of what this bus did (reference)

`loom::Recorder` is a bounded, structured, host-side consumer of the tap. It
exists because Zen remembered almost nothing about a message once the turn that
carried it was over: the Switchboard's journal keeps a *verdict* per delivery seq
and nothing else, and everything richer belonged to a participant — a
`TerminalSession`'s transcript, a `ConsoleEngine`'s tap window, an application
tool's picture of one operation. Nothing could answer a question about a message
the asker did not itself send or receive.

Header: `zen/recorder/recorder.hpp` (target `loom::recorder`). The dump witness
is deliberately somewhere else: `zen/recorder/dump.hpp`.

## What it is, and what it is not

| Is | Is not |
|---|---|
| a tap consumer, beside `ConsoleEngine` | inside the `Switchboard`, which is the component that runs for weeks |
| bounded, and it says what it forgot | a database, an event store, or an unbounded log |
| structured — records, never lines | a formatted terminal surface; nothing in `recorder.hpp` returns a rendered string |
| host-owned, exactly the console's authority | a weave. It is not addressable, accepts nothing, and widens no participant's observation |

Its authority argument is `ConsoleEngine`'s, verbatim: it takes a
`Switchboard&`, and holding one is already root authority
([terminal](terminal.md) has the side-by-side table naming which class is which
security role). No scoped-observation law is needed, and none is invented.

## One record

`HistoryRecord` carries only facts a `BusEvent` states — sequence, sender,
resolved target, addressed role, authored office, shape and version,
correlation, dispatch parent, handler duration, outcome, refusal reason and the
gate's detail, plus the recorder's own `record_seq`. Fields a log conventionally
holds and this bus cannot supply (a wall-clock stamp, a user, a severity) are
absent on purpose.

**`record_seq` is the record's identity, not the bus `seq`.** Not every retained
fact has a delivery number: a lifecycle transition carries none, and a policy
change is not a bus fact at all.

## Delivery truth and retention truth are different truths

```text
RecordedOutcome     what became of the DELIVERY   Delivered / Refused / HandlerFailed
PayloadDisposition  what the recorder DECIDED     None / Retained / TooLarge / NotRetained
PayloadState        what the recorder still HAS   Absent / Retained / Evicted / Declined
Horizon             what the reader can be told   Retained / Forgotten / NotRecorded / Unobserved
```

**There is no `Pending` anywhere in that vocabulary.** The tap fires only once a
delivery has reached a terminal fact, so a record is never unfinished — and the
Switchboard's `Disposition::Pending`, which means five different things at once
(never issued, evicted, still queued, in flight, the handler threw), is
deliberately not mirrored. A recorder answers about what it saw.

## The four honest answers

`find(bus_seq)` never answers a forgotten fact with something shaped like
"nothing happened":

| Answer | Means |
|---|---|
| `Retained` | here is the record |
| `Forgotten` | at or below the recorder's horizon: if it was ever kept, it is gone |
| `NotRecorded` | inside the observed range, above everything released, and not here — the recorder was watching and did not keep it |
| `Unobserved` | beyond anything the tap has shown it: still queued, never dispatched, or never issued. It does not guess which |

**The stated limit.** Below the horizon, `Forgotten` also covers "never kept" —
the record that would have said so is the thing that is gone, and remembering
every declined sequence would be the unbounded growth the window exists to
prevent.

## Retention classes

```text
Shared        the ordinary window
Dedicated     one shape's own window, with its own capacity
Protected     rare facts that must not compete with ordinary traffic
NotRetained   observed, deliberately not kept -- and COUNTED, never silent
```

Retention importance is **recorder policy**. A shape never declares itself
important and nothing on the wire can ask to be remembered. The only opinions
the default policy holds are structural and keyed on what the *bus* did — a
refusal, a failed handler and a lifecycle transition are protected — and each is
a flag a host can clear. Those outrank a shape rule, including a `NotRetained`
one: `TimerFired: NotRetained` means "the flood of beats is not worth
remembering", and the one beat that was refused is not part of that flood.

Capacities and budgets: [bounds](bounds.md#recorder-host-history).

## Two budgets, and they are forgotten separately

Metadata is bounded by records; payloads are bounded by **bytes**. A payload
released by the byte budget leaves its metadata exactly where it was, and the
record says which — `PayloadDisposition::Retained` with `PayloadState::Evicted`
is precisely "I had it and let it go". The reverse does not hold: a record
released from its window takes its payload with it, because a payload nothing
can name is unreachable.

Payloads are **serialized inside the tap callback**. `BusEvent::payload` points
into a `Message` that dies when the delivery returns, so a recorder that stored
the pointer would be a use-after-free the ordinary lane calls green.

## The structural blacklist is not a filter

```text
RETENTION POLICY   maker-controlled       which real facts deserve memory
RECORDER BLACKLIST architecture-controlled what is not a fact about the system at all
```

`RecorderBlacklist` excludes recorder machinery — a participant or a shape the
host declares as the recorder's own — from the recordable universe entirely,
before any rule is consulted. It exists to make recursive history impossible
rather than unlikely. Today the recorder authors no bus traffic (it is a tap
consumer that writes its log with an ordinary file handle), so a
default-constructed blacklist is empty and says so; the moment a recorder
mechanic does speak, the host declares it here.

It must never be used to hide ordinary Loom facts for being noisy. A `TimerFired`
is a real fact and belongs to policy; a recorder incrementing a counter is not a
fact at all.

## Policy changes are remembered, and nothing is published to remember them

`apply_policy` writes one `RecordKind::RecorderPolicy` record into the protected
window describing the transition, and sends nothing. A recorder that published
its own policy changes in order to observe them would manufacture the traffic it
exists to watch.

The change is **prospective**: a smaller window governs future retention, and
nothing already retained is destroyed by applying it. The excess drains as new
traffic arrives, and the policy record states how many records were above the new
bound at the moment of the change, so the trim is never a surprise.

Derived statistics — "300 beats omitted" — are **counters**, answered from
`counters()` and `tallies()`, never recorded as events.

## Persistence

`open_log(path)` appends each retained record as it is recorded, and
`close_log()` (or the destructor) flushes and closes. No scheduler, no background
thread, no idle detection, no compaction, no rotation.

The container is a stream of top-level values, `[u32 len][bytes]`, each one
canonical native bytes of `zen.recorder.Record` v1. That shape is Arena's and is
chosen for Arena's reason: `kMaxDecodedCells` bounds a *single* decode, so a
whole log as one nested document could not be read back. `Recorder::read_log`
re-admits every record through the one gate, so a corrupt or forged log is
refused rather than trusted.

**The log outlives the window.** It is append-only, so it keeps records the
in-memory windows have released — which is the boundary RTH-1 draws explicitly:
RAM-only data is not durable, and durability past the window is what the file is
for. It is bounded by `log_byte_budget`; when that is reached writing stops and
the log's own last record says so.

## What it deliberately does not do

No query language, no filtering surface, no `watch`, no triggers, no `await`, no
causal graph, no replay, no telemetry export, and no hot replacement. A mature
recorder that owned a policy, a high-water sequence, a hot window, a payload
cache and an open persistent segment would be a genuine candidate for a custom
handoff ceremony ([prepared-replacement](prepared-replacement.md)); changing its
*policy* is not that, and needs no replacement at all.
