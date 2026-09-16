# Published bounds — reference

Every deliberate capacity in the current system, in one place. A bound
refusing is visible (`Exhausted`, a named `TxnReason`, or a stated shed) —
never a silent drop.

**The named constants own the values; this table mirrors them.** Nothing
mechanically holds the two in step — the tests use the symbols, never the
literals — so read a number here as a pointer to the constant beside it rather
than as the contract. The names are grep-able for exactly that reason.

## Switchboard

| Bound | Value | Overflow behavior |
|---|---|---|
| `kMaxDeferredAnswers` | 64 | `Exhausted` refusal on the tap; the immediate answer right survives. **Loom-wide, not per weave** |
| `kJournalCapacity` | 1024 | ring: older ticket outcomes read as `Pending`, exactly like unknown seqs |
| `kMaxPreparedReplacements` | 8 | `CapacityExhausted` before anything is inspected |
| `kMaxPreparationBudget` | 1024 | larger requests refused at `begin` |
| `kMaxTerminalOutcomes` | 16 | oldest outcome dropped (evidence, not authority) |
| `kMaxJointOperations` | 8 | joint-publication records **live or unreleased** per bus; `Exhausted` at `begin`, nothing reused — a record is kept until its operator releases it, so an operator that never releases meets this bound and never another operator's outstanding outcome ([joint publication](joint-publication.md#the-records-lifetime-and-release)) |
| `kMaxJointKeys` | 4 | keys one operation binds; `Exhausted` at `begin` |
| `kMaxJointOfferBytes` | 64 KiB | one offered value's serialized size; `TooLarge` at `offer` — a joint publication carries facts, not documents |
| activation sequence | `INT64_MAX` | the door refuses further activations, naming the boundary; it does not brick |
| deferred-answer tokens | 2^64, monotonic | deliberately unguarded: process-local, never persisted, +1 per deferral |

Also structural (not knobs): one active replacement transaction per incumbent
*and* per candidate; one preparation conversation per transaction; one live
joint operation per claim key.

## Recorder (host working memory)

The one bounded surface here whose capacities are **not** fixed constants: a
recorder's retention is policy a host sets and may change at runtime, so the
values below are the published defaults rather than the contract. Everything
else about them follows the same doctrine as the console's windows — history, on
which nothing is owed, discarded oldest-first, and counted. See
[history](history.md).

| Bound | Default | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `kDefaultLastN` | 1 | records | **per shape**: the most recent observations of that shape | ring, its own budget per shape |
| `kDefaultRecentCapacity` | 4096 | records | the shared recent-context FIFO | ring: oldest released, counted in `bounds().forgotten` |
| `kDefaultProtectedCapacity` | 512 | records | facts that must not compete with ordinary traffic (a refusal, a failed handler, a lifecycle transition, a policy change) | ring, its own budget |
| `kDefaultPayloadByteBudget` | 1 MiB | **bytes** | retained payloads, across all windows | oldest payload released; its METADATA is untouched |
| `kDefaultMaxPayloadBytes` | 64 KiB | bytes | one payload | recorded as `TooLarge`; the metadata still stands |

A record is **stored once** and claimed by whichever windows want it, so the
total held is bounded by `recent + protected + Σ last_n` and a fact in three
windows costs one record, not three. It is released when the last window lets go.

**Why the payload budget is in bytes and the windows are in records.** RTH-0
measured the two halves of Zen's traffic ranking differently: an idle
application's noise is 300 messages a second at 31–47 bytes each, while one
interactive `SurfaceCanvas` is up to 2.75 KiB and is ~90% of interactive bytes.
A single budget in entries bounds the wrong thing at one end or the other.

**"Protected" means it does not compete, not that it is permanent.** Its window
is bounded like every other and its releases are counted; a recorder that
promised indefinite memory in RAM would be promising a leak.

## Logger (durable record)

**Deliberately not bounded by a global budget**, and that is the one entry in
this document whose answer is "no number". RTH-1 gave the persistent log an
8 MiB horizon shared with all traffic, which meant an idle application's
heartbeat consumed it in about three minutes and a weave replacement an hour
later was silently unwritable. RTH-1a removed it: durable append is **uncapped**
unless a per-shape `LogRule::cap` says otherwise, and a shape that reaches its
own cap stops and writes a record saying so, so a cap can never be mistaken for
an ending.

| Bound | Default | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `LogRule::cap` | 0 (uncapped) | records | **one selected shape** | that shape stops; one `PolicyChange` record states it; every other shape is untouched |

The Logger holds **no records in memory** — only counters and its selection — so
"uncapped" is a statement about the file and never about RAM.

## Console (operator history)

Both windows are **history**: past observations kept for inspection, on which
nothing is owed. That is what makes discarding the oldest legitimate here and
illegitimate for a backlog. They are shared by the in-process `ConsoleEngine`
and the client-side `RemoteConsole` -- the same two constants, the same window
type, so the local and remote operator see the same horizon.

| Bound | Value | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `kConsoleTapCapacity` | 1024 | bus events | the tap window (`ConsoleEngine::tap_`, `RemoteConsole::tap_`) | ring: oldest evicted, counted in `Console::evicted().tap` |
| `kConsoleBufferCapacity` | 64 | received arrivals | the m1/m2/... reply buffer (`ConsoleWeave::received_`, `RemoteConsole::buffer_`) | ring: oldest evicted, counted in `Console::evicted().buffer`; its **label** then refuses |
| `kConsoleAskCapacity` | 32 | conversations held for a caller | the in-process engine's tracked conversations: still open, plus answered and not yet taken | **refuses** a new tracked conversation, never evicts — see below |

**A third bound, and it is deliberately not a window.** `kConsoleAskCapacity`
bounds what the console is *holding for a caller* — conversations still open, and
answers that have settled but have not been taken — not what it has *seen*. Nothing
is owed on history, so evicting its oldest entry is legitimate; a held conversation
is the opposite — dropping one loses a question somebody asked or an answer they
have not read — so a new tracked conversation is **refused at capacity and every
held one is left untouched** ([ANS-05](messaging.md#the-askers-own-book)). The send
still happens; what it loses is attribution, and `Submitted::ask == 0` says so
rather than handing back a handle that can never settle. Thirty-two is an
operator's number: a person driving a console by hand does not hold dozens of open
questions, and `asks` lists them when they do.

**Holding is something a caller asks for, and then owes back.** A send holds
nothing unless its caller says `ConsoleTracking::Tracked`, and every send through
the frontend `Console` interface — `ConsoleUi`, `RemoteConsole` — is untracked: its
replies are history, and nothing accumulates however long it runs. A tracked
conversation spends its slot when it is opened and gets it back when the caller
takes the answer (`take_settled`) or forgets the conversation (`forget_ask`, which
cancels nothing at the far end). The bound is applied **when a conversation is
opened** because that is the only moment anything can still be refused honestly: an
arrival cannot be turned away without losing an answer, and evicting a held answer
would erase it before it was read. It once counted open conversations only, and an
answer leaves the book when it settles — so a frontend that composed and pumped kept
every answer it was ever sent (measured: 1,000 sends, 64 replies in history, 1,000
answers retained).

**An arrival is kept with its routing facts**, not just its payload:
`BufferEntry` carries the bus-stamped `sender`, the `correlation` the sender
named, and whether Loom attested it as an answer. The window used to keep the
`Value` alone, which left every consumer unable to tell an answer it had earned
from a reply-shaped message any participant may send.

**Why those two numbers.** The tap matches `kJournalCapacity` deliberately: one
tap entry is roughly one journal entry, so an operator who can still *see* an
event on the tap can still *ask* the journal what became of it. The buffer is
sixteen times smaller because the unit is far heavier -- a `TapEvent` is a few
short strings, while a wire-arrived `Value` is bounded only by
`kMaxDecodedCells` -- and it matches `kMaxPendingDelivered`, the *pending* half
of the same client-side reply path.

**Ownership / reset.** Host-owned and automatic: not a constructor parameter,
not configurable, not widenable by a message. There is no clear operation --
the reset is object lifetime, and a fresh console starts a fresh window with
both counters at zero. `evicted()` is therefore a statistic about *this*
window, never a lifetime total a later console inherits.

**Labels are identities, not positions.** `buffer_at(N)` answers for the reply
labelled `mN`; the retained range is `m(evicted+1) .. m(evicted+size)`. An
evicted label refuses and says it was evicted -- it never re-binds to whatever
reply now occupies that slot, so a reference an operator wrote down either
still means what it meant or fails loudly. Eviction is visible without asking:
the tap and buffer panes carry it in their headings.

## Supplied host input (what `loom-host` reads before a command exists)

The console above keeps what the bus did; this is the step before it — the bytes of a
person's or a script's commands, read from stdin before any of them becomes a message. The
values, and the rules that apply them, live in
[`src/host/line_input.hpp`](../../src/host/line_input.hpp). They are **the same on every
platform**, by construction rather than by care: one platform-neutral waiting area,
`loom::host::HeldInput`, decides what a line is, what is too long, how much may wait and
what the end of input means, and the two platform readers (POSIX `poll`, the Windows reader
thread) only move bytes into it.

| Bound | Value | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `kMaxCommandBytes` | 4000 | bytes, line ending excluded | one command | **refused** where it stands (`LineInput::Status::TooLong`, with its input line number and first bytes): none of it runs — not shortened, not split — the rest of that line is discarded as it arrives, and the next line is read as usual |
| `kHeldInputBytes` | 64 KiB | bytes | everything the reader holds for the host at once: finished lines not yet taken **and** the line still arriving | the reader **waits**, and resumes as the host takes lines; nothing is dropped, reordered, or counted against a lifetime total |

**A backlog, so bounded by waiting, never by discarding.** Every command a producer wrote is
owed to the host, in order, so the bound is applied by not reading: what the host has not
reached stays with its producer — a pipe's writer waits, a file stays on disk, a terminal
keeps what was typed — and a script of any length is read as fast as the host runs it. The
Windows reader once did the opposite and moved everything into memory as fast as it arrived:
a 25.6 MB file of commands took about 37 MB before the host had run one.

**The only thing dropped is a line that was refused**, and it is dropped whole. Its rest is
discarded as it arrives instead of being held until its newline, which is what lets a line
that never ends cost nothing: the reader keeps reading and discarding, and the host keeps
serving the bus. Nothing of a refused line is ever handed out, so no shortened command and
no second half of one can run.

**Why 4000.** A Linux terminal in its ordinary (canonical) mode keeps only the first 4095
bytes of a typed line and drops the rest without a sign; a Windows console delivers a long
typed line whole (both measured: WSL2 kernel 6.6, Windows 11 26200 console, lines to 100,000
characters). A limit below 4095 means a line that terminal cut short is always refused and
never run, and one number on both platforms means a command either works everywhere or is
refused everywhere. It is still far above any command the host has — a `start` with a long
path is a few hundred bytes.

**Why 64 KiB.** It must hold the longest unfinished command plus one read (4 KiB), so a
reader waiting for room can never be waiting for a newline it has no room to read; that is a
compile-time assertion beside the constants. Beyond that it only sets how much of a burst is
taken per wake-up, and one Linux pipe's worth is plenty. It counts bytes in one buffer, not
lines in a queue, so a flood of empty lines cannot hold more than its bytes say.

**How full it gets is not the contract.** A POSIX reader reads only when the host asks and
nothing is finished, so it rarely holds more than one read; the Windows thread reads ahead
until the area is full. Both stay within the limit, and `LineInput::held()` reports the
bytes held now and the most ever held, which is how `host_line_streams` and
`host_line_input` prove it on each platform with the same assertions.

**Ownership / reset.** Host-owned and automatic: not a parameter, not configurable. The
reset is the reader's lifetime. A reader that is replaced takes what it held with it — at
most `kHeldInputBytes` — and on a pipe or a file that can include part of a line, whose rest
the next reader then receives as a line of its own; the supplied host reads stdin with one
reader for its whole life.

## Terminal session (one participant's own record)

The same two-window split, and the same reasoning, one tier down: a
[terminal session](terminal.md) keeps a wide window of cheap ENTRIES and a
narrower one of the heavy VALUES they refer to. Both are history; nothing is
owed on either. Both use `loom::BoundedHistory`, the primitive the console's
windows always used and which moved to
[`zen/bounded_history.hpp`](../../include/zen/bounded_history.hpp) at TERM-0 so
there is one ring rather than six.

| Bound | Value | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `kTranscriptCapacity` | 256 | transcript entries | `Transcript::entries_` -- a participant's own record | ring: oldest evicted, counted in `Transcript::evicted()` |
| `kReceivedCapacity` | 64 | received `Value`s | `Transcript::received_` -- the `rN` store the `$rN.field` syntax reads | ring: oldest evicted, counted in `Transcript::received_evicted()`; its **id** then refuses |
| `kMaxOutstandingAsks` | 8 | conversations | how many asks one participant will track at once -- the number the terminal hands its `loom::AskBook` | the next ask is refused LOCALLY; nothing is authored and the outstanding ones are untouched |

**`loom::AskBook` has no default capacity, and that is a bound too.** The number
above is the *terminal's* product decision, stated where that decision lives. The
reusable record ([`weave/ask_book.hpp`](../../include/zen/weave/ask_book.hpp))
takes its bound at construction and refuses a book with no room, so a number
invented in the substrate can never become every asker's limit by accident. What
the book does fix is the *policy*: at capacity the NEW conversation is refused and
the outstanding ones are untouched — never `loom::relay`'s shed-the-oldest, which
is right for a middleman and wrong for the participant whose own questions these
are.

**An entry is metadata; a received value is not.** An entry is a few short
strings and an id, so a session's worth of scrollback is cheap; a received
`Value` is bounded only by `kMaxDecodedCells`, which is why the store that keeps
them is four times smaller -- the same argument that made the console's reply
buffer sixteen times smaller than its tap.

**Eviction cannot cost a conversation.** Outstanding asks live in the session,
never in the transcript, so evicting the visible `SUBMITTED` line for an ask does
not lose the fact that this participant is still waiting on it. Pinned in suite
`terminal`; it is the one place where "history may be forgotten" and "an
obligation may not" meet.

**Ids are identities, not positions**, exactly as the console's `mN` labels are:
`received(N)` answers for the message with that id, the retained range is
`(evicted, evicted+size]`, and an evicted id refuses *and says it was evicted*
rather than re-binding to a newer message.

**The vocabulary is deliberately unbounded**, and that is not an omission: every
entry is placed by the HOST at mount, so there is no traffic that can grow it and
nothing to evict. Everything untrusted traffic can grow is in the table above.

## Bridge (remote operator)

What the component *is*, and what it trusts: [bridge](bridge.md).

| Bound | Value | Behavior |
|---|---|---|
| `kMaxOperatorConnections` | 32 | accept-then-shed, `declined_count()` visible |
| `kMaxPendingDelivered` (client) | 64 | pending unknown-schema replies bounded, drained on `SchemaNone`. An **active backlog**, so it is bounded by refusal (a visible `BridgeRefused`), never by eviction -- dropping the oldest would discard an obligation |
| `kMaxAbsentSchemas` (client) | 64 | remembered `SchemaNone` answers; FIFO, oldest evicted. A **memo**, so eviction costs at most one repeated `Describe` -- and stops a host from growing the client one entry per novel unknown shape |

## Transport channels (framed byte channels)

Both framers -- the isolation `Channel` (parent side of an out-of-process Weave
host) and the portable `BridgeChannel` -- share these.

| Bound | Value | Scope | Behavior |
|---|---|---|---|
| `kMaxFrameLen` | 64 MiB | one frame's payload | over-cap: the channel is marked `failed()` (send) / the framer fails cleanly, no over-read (receive) |
| `kMaxBacklog` | 64 MiB | the **unsent** send backlog, and the unread receive buffer | `failed()` -- a peer that will not drain is contained, never allowed to block, hang or OOM the host |
| live send storage | < 2x the unsent backlog | the outbox buffer | not a knob: an invariant of the amortized prefix reclamation ([LIFE-07](../laws/lifecycle-laws.md)) |
| live receive storage | the unread/incomplete suffix | the inbox buffer | not a knob: the decoded prefix is erased at the end of every poll that decoded it |

The two caps overlap on the send side: since `kMaxFrameLen == kMaxBacklog`, a
single frame at exactly the frame cap already exceeds the backlog cap by its
5-byte header, so the largest frame that can actually be queued is
`kMaxBacklog - 5`. The per-frame check is therefore **masked** by the backlog
check for any one frame -- deliberate overlap, not a redundant guard: the frame
cap is what a *receiver* enforces on a length it was told.

Neither live-storage row is a capacity a caller can exhaust; both are statements
about what the channel is allowed to keep. They are bounds on *history*, and
that is the whole content of [LIFE-07](../laws/lifecycle-laws.md).

## Schema registry (bounded by claims, not by a number)

The one entry here with no constant, because the bound is a lifetime rather than
a capacity. A `Registry`'s population is *the schemas something live still
requires* — not every schema ever registered
([LIFE-08](../laws/lifecycle-laws.md#life-08--a-schema-is-retained-by-a-live-claim-never-by-having-been-registered),
[values-and-admission](values-and-admission.md#registry)).

| What retains a schema | Until |
|---|---|
| `register_schema(s)` | the Registry is destroyed (a claim with no end) |
| a weave's registration | its `WeaveRecord` is erased — accept-set, declared claim-set, state shape, and the shapes its grant *names* as sendable |
| a loaded artifact | its Kernel record is erased (`unload`, a reaped adapter, a throw on the way in) |
| a mounted child | its `Link` is erased (`unmount`) |
| a reload candidate | its `Manifest` goes out of scope — a refused candidate leaves nothing |

**Overflow behavior: none, because there is no cap.** A host that holds a
million live weaves has a million weaves' vocabulary; what BL-0 removed is
growth from *history*. A long-running host that repeatedly loads and unloads
distinct shapes returns to its baseline.

**Reclaimed ≠ freed.** Removal is from *current lookup*. A `Value` owns its
schema strongly, `lookup` hands back a strong owner, and an older reader
snapshot keeps its own entries alive — so a schema may legitimately outlive its
Registry membership in memory. As with [LIFE-07](../laws/lifecycle-laws.md), the
bound is on what is *live and reachable*, never an RSS guarantee.

## Gate / serialization

Depth and size caps make hostile input total (see
`values-and-admission` and the fuzz suite); the UI vocabulary pins tree depth
≤ 256 and per-kind child arity.

| Bound | Value | Scope | Overflow behavior |
|---|---|---|---|
| `kMaxBinaryDepth` | 64 | one nesting chain | `MalformedBytes` |
| `kMaxListCount` | 2^20 | **one** list | `MalformedField`, "list count exceeds cap" |
| `kMaxFieldBytes` | 2^28 | **one** `Text`/`Bytes` | `MalformedField` (also capped by remaining input) |
| `kMaxDecodedCells` | **65,536** | **the whole decoded value** | `MalformedBytes`, "…exceeds the materialization budget…" |
| `kMaxTypeDepth` | 64 | one schema descriptor's type | thrown refusal from `decode_schema` |

### The decode-materialization bound

**Wire-size limits bound serialized bytes. Decode-materialization limits bound
the trusted host structure those bytes may create. Neither implies
application-semantic validity.**

Serialized size and decoded structural size are different facts. A zero-field
`Message` has a zero-byte presence bitmask, so it costs no body bytes at all — a
list of them commands a host-side population unrelated to the input's length. A
compact value may legitimately represent many values; it may not command
effectively unbounded host work.

- **Unit** — one *decoded cell*: one `Cell`-sized slot the decoder
  materialises. One per **declared** field of every message it enters (a `Value`
  allocates exactly that many `std::optional<Cell>` slots, present or not) and
  one per element of every list it decodes. So a message of *n* fields costs *n*
  whether or not those fields arrive; a list of *k* elements costs *k*, plus
  whatever each element then costs. `Text`/`Bytes` **payload** bytes are not
  counted — a 1 MiB `Bytes` field is one cell (its size is `kMaxFieldBytes`'
  business).
- **Scope** — one allowance per **top-level decode**, shared by every nested
  message, list, field and recursive helper. It is never reset per container, so
  two individually-modest lists cannot be summed past it. Schema descriptors are
  ordinary values and spend the same budget.
- **Boundary** — **inclusive**: a decode totalling exactly 65,536 cells is
  accepted; the first cell beyond it is refused.
- **When** — spent *before* the cells exist. A list charges its whole declared
  element count before building the first element. Exhaustion is a refusal, not
  an allocation regretted afterwards, and never a `std::bad_alloc` backstop.
- **Failure** — the ordinary parse-failure model: `admit()` returns a rejection
  with `ErrorKind::MalformedBytes` and a detail naming the budget. No partial
  `Value` escapes, none is admitted, no registry entry is derived from it, no
  message is delivered.
- **Ownership** — host-owned and automatic. Not a `parse()` parameter, not
  widenable by a message, grant, schema, or payload; changing it is a build
  decision (`src/detail/binary.hpp`).

65,536 cells is roughly 6 MiB of worst-case decoded structure — an order of
magnitude above the largest value any current consumer sends. A value larger
than the bound cannot cross a serialized boundary at all: an in-process `Value`
may exceed it, but nothing will re-admit its bytes.

## Isolation resource defaults (computed, no knob)

memory = RAM/8 (cap 1 GiB, floor 128 MiB) · pids = 512 · cpu_weight = 100.
`with_unlimited_memory()` removes the memory cap **alone** — no grant removes
`pids.max`, so no grant can license a fork bomb.

**Each dimension is imposed only where its cgroup-v2 controller is delegated
to this host**, and the attestation says so per dimension rather than implying
a cap nothing wrote: where the pids controller is absent, `pids.max` is unset,
the headline reads `FORK-BOMB STOP NOT ENFORCEABLE`, and a fork bomb is
bounded only by the host-wide pid limit. These numbers are what a *delegated*
leaf imposes — see
[capabilities § delegation](capabilities.md#delegation-is-what-makes-a-resource-cap-real).

## Zengine Timer (owned there, listed for reach)

`kMaxHandoffEntries = 32` · `kPreparedClaimBeats = 8` (derived + published) ·
`kBeatCapMs = 10`. Owned by the **separate Zengine repository**, at
`Zengine/docs/reference/timer-continuity.md` — quoted here for reach, not
linked, because a Loom checkout does not contain it.

## Dispatch-refusal notices

No pending-send ledger is added. An envelope retains one extra unsigned scalar:
the opted-in author's incarnation. The existing life scalar supplies the other
half. A notice carries metadata proportional to the original authored identifiers,
never a copy of the original payload, and pins no deferred answer or conversation.

One refused envelope is consumed before at most one notice is appended. Thus
notice generation alone cannot increase the queue's envelope count beyond the
backlog it replaces. This bounds incremental growth, not the pre-existing
ordinary send queue. No arbitrary numeric notice quota is introduced.

If the sender disappeared, changed life/incarnation or closed its door, the
notice is disposable. Allocation failure or delivery-sequence exhaustion may
discard it while the original refusal retains its host evidence. There is no
persistence/eventual-delivery promise and no refusal-of-refusal chain.

AskBook optionally retains one scalar per existing bounded local ask. The
terminal adds one attempt scalar and one shared pointer per transcript entry;
authenticated refusal metadata is allocated only for refused receipts, retained
under kTranscriptCapacity. Its exact notice Value uses the existing received
window. Neither window retains the original refused payload. Transcript eviction
does not evict the separate pending ask; local forgetting removes that ask's
attempt binding, so late notices cannot settle a newer operation.
