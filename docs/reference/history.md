# History — what the host knows, and what it keeps (reference)

Zen remembered almost nothing about a message once the turn that carried it was
over: the Switchboard's journal keeps a *verdict* per delivery seq and nothing
else, and everything richer belonged to a participant — a `TerminalSession`'s
transcript, a `ConsoleEngine`'s tap window, an application tool's picture of one
operation. Nothing could answer a question about a message the asker did not
itself send or receive.

Two host-side lenses on the tap answer that, and they answer **different
questions**:

```text
loom::Recorder    what does Zen know RIGHT NOW?          volatile, bounded, admits by default
loom::Logger      what did Zen choose NOT TO FORGET?     durable, selective, REFUSES by default
```

Headers: `zen/history/recorder.hpp`, `zen/history/logger.hpp`, and the shared
structured fact in `zen/history/record.hpp` (target `loom::history`). The dump
witness is deliberately somewhere else: `zen/history/dump.hpp`.

**They are two owners, not two layers.** `logger.hpp` does not include
`recorder.hpp`, a `Logger` takes no `Recorder&`, and a `Logger` works in a
process that has no `Recorder` at all. A durable fact that had to ask a volatile
window for its contents would be durable only as long as that window.

## What they are, and what they are not

| Is | Is not |
|---|---|
| tap consumers, beside `ConsoleEngine` | inside the `Switchboard`, which is the component that runs for weeks |
| bounded, and they say what they forgot | a database, an event store, or an unbounded log |
| structured — records, never lines | a formatted terminal surface; nothing in `recorder.hpp` or `logger.hpp` returns a rendered string |
| host-owned, exactly the console's authority | weaves. Neither is addressable, neither accepts anything, and neither widens any participant's observation |
| beside the delivery path | a stage in it — a message reaches its recipient without passing through either |

Their authority argument is `ConsoleEngine`'s, verbatim: each takes a
`Switchboard&`, and holding one is already root authority
([terminal](terminal.md) has the side-by-side table naming which class is which
security role). No scoped-observation law is needed, and none is invented.

## One record

`HistoryRecord` carries only facts a `BusEvent` states — sequence, sender,
resolved target, addressed role, authored office, shape and version,
correlation, dispatch parent, handler duration, outcome, refusal reason and the
gate's detail, plus the recorder's own `record_seq`. Fields a log conventionally
holds and this bus cannot supply (a wall-clock stamp, a user, a severity) are
absent on purpose. Both owners speak it, and `fill_from_event` is shared so they
cannot drift into two slightly different readings of one event.

**`record_seq` is the record's identity, not the bus `seq`.** Not every retained
fact has a delivery number: a lifecycle transition carries none, and a policy
change is not a bus fact at all.

## Delivery truth and retention truth are different truths

```text
RecordedOutcome     what became of the DELIVERY   Delivered / Refused / HandlerFailed
Held                which windows CLAIM it now    LastCall / Recent / Protected (a SET)
PayloadDisposition  what the recorder DECIDED     None / Retained / TooLarge / NotRetained
PayloadState        what the recorder still HAS   Absent / Retained / Evicted / Declined
Horizon             what the reader can be told   Retained / Forgotten / NotRecorded / Unobserved
LogOrigin           where a DURABLE record came from   BusObservation / Diagnostic / PolicyChange
```

**There is no `Pending` anywhere in that vocabulary.** The tap fires only once a
delivery has reached a terminal fact, so a record is never unfinished — and the
Switchboard's `Disposition::Pending`, which means five different things at once
(never issued, evicted, still queued, in flight, the handler threw), is
deliberately not mirrored. A history answers about what it saw.

---

# Recorder — volatile working memory

## Two complementary kinds of memory, and a third window for the rare

```text
LAST CALL   per shape, `last_n` deep, DEFAULT 1.   "What was the last BuildFinished?
                                                    Has a TimerFired ever happened here?"
RECENT      one shared FIFO.                       "What happened AROUND now?"
PROTECTED   refusals, failed handlers, deaths.     "The rare thing I came looking for."
```

A last-call slot can say *`HandlerFailed` happened*; only the recent FIFO can say
what surrounded it. They are different questions and neither answers the other,
which is why noisy traffic can be kept out of recent context **without** being
made unrecordable — the correction RTH-1a makes to RTH-1's single window.

**One owning store, several claims.** A fact is stored once and the windows hold
its identity; it is released when the last window lets go, and `held` on a
retained record says which windows still claim it *right now*. A record in three
windows costs one record.

## Every observed shape is discoverable

By default every shape the tap has actually shown keeps its most recent
observation, so a fact can leave recent context without leaving memory:

```cpp
recorder.last_of("TimerFired");   // Retained + the record, long after the FIFO moved on
recorder.observed("TimerFired");  // has this shape happened in this process at all?
recorder.tallies();               // every observed bus shape, with its traffic
```

`last_n = 0` is the one way to make a shape genuinely undiscoverable, and it has
to be written down. The recorder answers about **observation**, never about
registration: a tap sees deliveries, so "registered but never sent" is a question
it does not claim to answer.

## The four honest answers

`find(bus_seq)` never answers a forgotten fact with something shaped like
"nothing happened":

| Answer | Means |
|---|---|
| `Retained` | here is the record |
| `Forgotten` | at or below the recorder's horizon: if it was ever kept, it is gone |
| `NotRecorded` | inside the observed range, above everything released, and not here — the recorder was watching and did not keep it |
| `Unobserved` | beyond anything the tap has shown it: still queued, never dispatched, or never issued. It does not guess which |

`last_of(shape)` uses the same vocabulary: `Unobserved` means the shape has never
been seen, `NotRecorded` means it has been seen and given no slot. Those are
different facts and never share a word.

**The stated limit.** Below the horizon, `Forgotten` also covers "never kept" —
the record that would have said so is the thing that is gone, and remembering
every declined sequence would be the unbounded growth the window exists to
prevent.

## Retention policy: three independent knobs

```cpp
RetentionRule{ shape, last_n, in_recent, retain_payload }
```

Three questions, three answers, because no single class can say what a heartbeat
wants — *keep the last one, take no context, keep no bytes*:

```text
TimerFired      last_n = 1   in_recent = false   retain_payload = false
BuildOutput     last_n = 3   in_recent = true    retain_payload = true
SurfaceCanvas   last_n = 1   in_recent = true    retain_payload = false
```

Retention importance is **recorder policy**. A shape never declares itself
important and nothing on the wire can ask to be remembered. The only opinions the
default policy holds are structural and keyed on what the *bus* did — a refusal,
a failed handler and a lifecycle transition are protected — and each is a flag a
host can clear.

**Protection decides whether a fact is KEPT; the shape decides whether it takes
RECENT CONTEXT.** So a muted shape's one refused beat is still kept, and a storm
of refused beats still cannot drown the build a maker came for. Admission is a
metadata decision keyed on a stable shape name and taken on the dispatch that
produced the event: no payload is decoded, no predicate is evaluated, and the
resolved rule is cached per shape on first sight.

Capacities and budgets: [bounds](bounds.md#recorder-host-working-memory).

## Two budgets, and they are forgotten separately

Metadata is bounded by records; payloads are bounded by **bytes**. A payload
released by the byte budget leaves its metadata exactly where it was, and the
record says which — `PayloadDisposition::Retained` with `PayloadState::Evicted`
is precisely "I had it and let it go". The reverse does not hold: a record
released from every window takes its payload with it, because a payload nothing
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
rather than unlikely. Today neither owner authors any bus traffic (both are tap
consumers; the Logger writes with an ordinary file handle), so a
default-constructed blacklist is empty and says so.

**The Logger has none and needs none**: its selection is a whitelist, so
machinery nobody named is already excluded by construction. The Recorder's
default is *admit*, which is why it is the half that carries this.

It must never be used to hide ordinary Loom facts for being noisy. A `TimerFired`
is a real fact and belongs to policy; a recorder incrementing a counter is not a
fact at all.

## Policy changes are remembered, and nothing is published to remember them

`apply_policy` writes one `RecordKind::RecorderPolicy` record into the protected
window describing the transition, and sends nothing. A recorder that published
its own policy changes in order to observe them would manufacture the traffic it
exists to watch. It is deliberately **not** given a shape state, so `tallies()`
stays an answer about bus traffic.

The change is **prospective**: a smaller window governs future retention, and
nothing already retained is destroyed by applying it. The excess drains as new
traffic arrives, and the policy record states how many records were above the new
bound at the moment of the change, so the trim is never a surprise.

Derived statistics — "300 beats omitted" — are **counters**, answered from
`counters()` and `tallies()`, never recorded as events.

---

# Logger — the durable selected record

## A whitelist, not a budget

Nothing is durable unless it was **named**, and what was named is not capped by
traffic that was not. RTH-1's persistence was the recorder's window written to a
file behind a global 8 MiB ceiling, which meant an idle application's heartbeat
consumed the horizon in about three minutes and a weave replacement an hour later
was silently unwritable. That failure is the reason this half exists.

```cpp
LoggerSelection {
    bool log_handler_failures = true;   // ANY shape — failure is not a property of a shape
    bool log_lifecycle       = true;    // Died / Revived
    bool log_refusals        = false;   // measured: ordinary refusals are neither rare nor severe
    std::vector<LogRule> shapes;        // { shape, cap }  — cap 0 == UNCAPPED
}
```

The shipped `default_selection()` is deliberately narrow, and source-traced
rather than invented — two categories that are rare *by construction*:

| Category | Shapes |
|---|---|
| what code is loaded | `LoadLibrary`, `ReloadLibrary`, `UnloadLibrary`, `UnloadRole`, `zen.LoadWeave`, `zen.SwapWeave`, `zen.ReloadWeave` |
| who may speak | `zen.RequestAuthority`, `zen.ApproveAuthority`, `zen.RefuseAuthority`, `zen.RevokeAuthority`, `zen.AuthorityGranted` |

Deliberately absent, each for a stated reason: the **queries** beside those
changes (`ListLibraries`, `QueryRole`, `ListLoaded`, `DescribeAuthority`,
`AuthorityDescription`, `ManagerState`) — a read is not a change; the **handoff**
vocabulary (`zen.PrepareShutdown`, `zen.Bequest`, `zen.ClaimBequest`) — the
replacement it belongs to is already marked by `zen.SwapWeave` and by the
structural `Died`/`Revived`; and every **application** shape, because this list
is Loom's and a host's traffic is the host's to name.

## Caps are per shape, never global

`LogRule::cap == 0` means uncapped, which is the default: a rare durable fact
should not need a number somebody has to have guessed right in advance. A shape
that reaches its own cap stops, writes one `PolicyChange` record saying so, and
leaves every other shape's horizon untouched — **a cap is never an ending**, and
a reader can always tell a bounded shape from a killed process.

"Uncapped" is a statement about the **file**. The Logger holds no records in
memory at all, only counters and its selection.

## Three origins, and the rule that matters

A durable record may come from somewhere other than the bus, and must never
pretend it was a Loom message:

| Origin | Written by | Carries |
|---|---|---|
| `BusObservation` | the tap | the `observation`, captured on the dispatch that produced it |
| `Diagnostic` | the host, via `write`/`info`/`warn`/`error` | severity, source, text — and an **empty** observation |
| `PolicyChange` | the Logger itself | what changed about what it keeps |

Nothing manufactures bus traffic to get a fact written down: `logger.error(...)`
sends nothing and is observed by nobody. `dump_log` prints the origin before the
content for the same reason.

## The stream

`open(path)` appends; `close()` (or the destructor) flushes. No scheduler, no
background thread, no idle detection, no compaction, no rotation.

The container is a stream of top-level values, `[u32 len][bytes]`, each one
canonical native bytes of `zen.history.LogRecord` v1. That shape is Arena's and
is chosen for Arena's reason: `kMaxDecodedCells` bounds a *single* decode, so a
whole stream as one nested document could not be read back. `Logger::read`
re-admits every record through the one gate, so a corrupt or forged file is
refused rather than trusted.

**A selected fact with nowhere to go is counted, never pretended.** A Logger with
no open stream is a live selection and an empty file; `counters().selected` still
rises and `counters().appended` does not.

## What both halves deliberately do not do

No query language, no filtering surface, no `watch`, no triggers, no `await`, no
causal graph, no replay, no telemetry export, no crash bundles, no stack walking,
and no hot replacement. A mature recorder that owned a policy, a high-water
sequence, hot windows and a payload cache would be a genuine candidate for a
custom handoff ceremony ([prepared-replacement](prepared-replacement.md));
changing its *policy* is not that, and needs no replacement at all.

**A future bounded forensic capture is not blocked by any of this.** The
recorder's owning store already separates a fact from the windows that claim it,
so a capture is one more claimant with its own budget and its own admission —
not a redefinition of the Recorder, and not a second filtering path.
