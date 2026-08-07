# Lifecycle laws (LIFE)

Reference: [lifecycle](../reference/lifecycle.md).

## LIFE-01 — `zen.Activated` means exactly one thing

LAW — `zen.Activated{sequence}` means *a new code incarnation has successfully
committed at this address*, and nothing else.

MEANS
- ten non-meanings are pinned in the header: not healthy, not ready, not a
  role-holder, state not preserved, no predecessor implied, not graceful,
  resources not promised, no loop commanded, no work re-commanded, system not
  ready;
- the one-field minimality is itself pinned — widening the shape means
  deleting a test that says why not.

DOES NOT MEAN
- "start your main loop" — what a weave does with its first breath is the
  weave's own business.

PROVEN BY — `include/zen/weave/lifecycle.hpp`; suite `weave` (minimality),
`provenance`.

## LIFE-02 — Participation is declared, never attempted

LAW — A weave receives lifecycle facts iff it lists the shape in its accepted
schemas. Nobody sends blindly and calls the refusal "optional".

MEANS
- a non-participant costs nothing: no message, no manufactured refusal, no
  sequence spent.

DOES NOT MEAN
- that declaring the shape grants anything beyond hearing it.

PROVEN BY — the control door asks `Kernel::accepts` first; suite `manager`.

## LIFE-03 — The sequence is finite and honest

LAW — Activation sequences are monotonic within an operator lineage, never
reused after an aborted attempt, and the allocator refuses at the boundary
rather than wrapping.

MEANS
- consumer identity is the pair (attested sender, sequence) — per attesting
  operator, checked by the consumer's cursor;
- gaps are legal (a scheduled-then-refused admission spends its number);
- at `INT64_MAX` or a negative revived lineage the door refuses, naming the
  boundary — it does not brick.

DOES NOT MEAN
- that sequences are globally unique across operators, or that Loom owns
  allocation (the operator supplies the number).

PROVEN BY — `activation_block()` preflight; suites `manager`, `kernel`
(sequence passthrough and gap cases).

## LIFE-04 — Lifecycle authority is Loom-owned and board-relative

LAW — Only trusted host infrastructure holding this Loom's `LifecycleAuthority`
can attest lifecycle facts. A grant to *emit* `zen.Activated` is not authority
to *attest* it.

MEANS
- the mint is private and non-static on the Switchboard, reachable through one
  friend in a host-wiring header no weave-authoring header includes;
- an authority names its issuing board and is refused elsewhere
  (`ForeignAuthority`) — anyone may own a Switchboard; nobody thereby owns
  yours;
- an ordinary `zen.Activated`, however well-shaped and well-granted, carries no
  attestation and consumers ignore it as a lifecycle fact.

DOES NOT MEAN
- that the shape is secret, or that sending it is forbidden — provenance, not
  payload type, is the distinction.

PROVEN BY — `LifecycleAuthority` + `issued_here`; suites `provenance` (decoy
board, impostor), `kernel`.

## LIFE-05 — A committed activation is not answerable

LAW — The activation delivered as part of an admission is Loom's own act, not a
send: no ordinary grant is consulted, and its handler holds **no** answer
authority — nobody asked anything.

MEANS
- `mail.answer()` inside that handler refuses visibly; `mail.defer_answer()`
  returns an invalid capability and consumes no bounded capacity;
- ordinary sends from inside the handler work normally — not-answerable is not
  mute (a prepared Timer's first act is an ordinary claim send);
- a later real ask is answerable normally: authority is scoped per delivery,
  never removed from the weave.

DOES NOT MEAN
- that `announce_lifecycle` changed — that is still an ordinary gated send with
  ordinary answer semantics;
- that `zen.Activated`-shaped ordinary messages became unanswerable — the
  distinction is provenance, not shape.

PROVEN BY — `deliver_admission` sets no reply authority; suite `kernel`
("first breath is not a question" cases, held-full capacity proof).

## LIFE-06 — A weave outlives its own callback

LAW — A Weave cannot be permanently removed, handed back, or destroyed while
Loom is executing that same Weave's callback. `unregister_weave(id)` returns
`nullptr`, changing nothing, when `id` is the weave whose `handle()` is running.

MEANS
- `nullptr` has exactly two meanings, both of them *nothing was removed*: the id
  is unknown, or the id is the active target. A host may simply retry once the
  callback has exited — by normal return **or** by exception unwind — and gets
  the weave then;
- the refusal is total, because the check precedes every mutation: no role
  released, no personal or office claim forgotten, no deferred conversation
  abandoned, no transaction invalidated, no registry entry erased, no ownership
  transferred;
- it covers **both** paths that call `Weave::handle` — ordinary delivery and the
  committed-activation delivery — through one check on one fact, the ambient
  active target each of them assigns around its own call;
- Loom's own internal discard obeys it too: `finish_txn` unregisters an aborted
  transaction's sealed candidate, and if that candidate is the running weave the
  discard fails and leaves sealed wreckage (nonpublic, belonging to a
  transaction that no longer exists) rather than destroying a live handler.

DOES NOT MEAN
- that no weave may be removed during a dispatch turn — removing a **different**
  weave from inside a callback works exactly as before, returns its owner, and
  performs its ordinary cleanup. The guard is exact to the active target, never
  to `in_dispatch_`;
- that removal became asynchronous, deferred, or queued — success still transfers
  a unique owner the caller may destroy immediately, which is *why* refusing is
  the only honest answer: Loom cannot both hand over unique ownership and keep it;
- that ownership became shared, that any lifecycle operation is forbidden from a
  callback, or that observers changed — a tap removing the just-delivered weave
  runs after that weave's member call has already returned
  ([MSG-11](messaging-laws.md));
- that ordinary weave code gained this reach. A handler is delivered a `WeaveBus`,
  which has no `unregister_weave`; the case exists only where host wiring
  deliberately supplies concrete `Switchboard&` access.

PROVEN BY — the active-target early return at the top of
`Switchboard::unregister_weave`; suites `switchboard` (R2F-B cases: self-removal
refused, retry after return, a different weave still removable, mutation-free
refusal, exception unwind) and `kernel` (committed activation, the `finish_txn`
discard route).

## LIFE-07 — Consumed transport bytes are history, not live channel storage

LAW — A framed channel's live receive and send buffers hold only the bytes that
still matter to framing: the unread/incomplete suffix on the way in, the backlog
still owed to the peer on the way out. Neither grows with the total volume that
has crossed the channel.

MEANS
- **the send side is bounded by the backlog, not the session.** After every
  `flush()` the already-sent prefix is smaller than the unsent remainder, so
  live storage is under **twice the backlog still owed** — and that backlog is
  itself capped by `kMaxBacklog`. A channel alive for weeks that has passed a
  terabyte holds no more than one that has just started;
- **reclamation is amortized, not per-frame.** The prefix is compacted only when
  it is at least as large as the remainder, so a move never copies more bytes
  than it discards: total copying stays linear in the bytes ever queued, never
  quadratic in the frame count. Ten thousand tiny frames do not perform ten
  thousand moves of the whole buffer;
- **the receive side reclaims unconditionally.** Every decoded prefix is erased
  at the end of the poll that decoded it, so a permanently incomplete suffix —
  the shape that defeats "clear only when the buffer is exactly empty" — pins
  nothing behind it;
- **nothing about framing moved.** Frame bytes, order, the length encoding,
  `kMaxFrameLen`, `kMaxBacklog`, EOF, failed state, non-blocking behavior and
  readiness are exactly as before. A compaction subtracts the same amount from
  both terms of the backlog cap, so the number that cap reads is invariant.

DOES NOT MEAN
- an **RSS guarantee**. The allocation is kept for reuse at its high-water mark;
  what is bounded is the channel's *live* buffer, not what the allocator has
  returned to the OS. "Capacity remains reusable" and "sent bytes remain part of
  the live buffer" are different claims, and only the second was the defect;
- `shrink_to_fit()` after every frame, a ring buffer, scatter/gather parsing, or
  any new transport machinery;
- a **message-history** feature. Nothing retains delivered frames for replay;
- flow control, backpressure, congestion control, or a queue-size policy. A peer
  that will not drain is still contained by `kMaxBacklog` failing the channel,
  exactly as before — reclamation moves the backlog, it never shrinks it;
- that a failed channel reclaims. Once `failed()`, `flush()` returns before
  doing anything: the state that records why it died is not disturbed.

PROVEN BY — the compaction branch at the end of `Channel::flush()`
(`src/isolation/channel.cpp`) and `BridgeChannel::flush()`
(`src/bridge/channel.cpp`), and the unconditional `inbox_.erase(0, pos)` in each
`poll()`. Suites `isolation` and `bridge` (R2F-C cases, one independent set per
framer: a never-idle channel over a persistent backlog, frames queued behind a
half-sent one, backlog-cap and failed-channel invariance, over-length refusal,
EOF, and the receive-side parity case). Published values:
[bounds](../reference/bounds.md#transport-channels-framed-byte-channels).

## LIFE-08 — A schema is retained by a live claim, never by having been registered

LAW — A Registry keeps a canonical schema discoverable while at least one live
claim requires it, and removes it from lookup when the last one is released.
Registration history retains nothing.

MEANS
- **there are two doors, and the difference between them is the whole model.**
  `register_schema(s)` is a claim nobody ever releases — what an application
  means when it publishes its own vocabulary — so "permanent" is ordinary
  lifetime rather than an exceptional flag. `claim(schemas)` hands back a
  `SchemaClaimScope`: move-only, RAII, released by the destruction of whatever
  owns it. There is no `unregister_schema` for a failure exit to forget;
- **claims are counted, definitions are not duplicated.** Any number of live
  owners may require one canonical `(name, version)`; the Registry holds one
  definition and one count. Releasing one claim changes nothing while another
  stands. Conflict semantics are untouched: a claim on a same-key
  different-content schema throws `SchemaConflict` and the incumbent definition
  is not disturbed;
- **acquisition is transactional.** A multi-schema claim validates the complete
  set before publishing any of it, so a disagreement about the last schema
  leaves no claim on the first. `register_weave` takes its accept-set, claim-set
  and state shape as one such transaction, and a Kernel takes a whole manifest
  as another;
- **the owners are the objects whose lifetimes already mean it.** A
  `WeaveRecord` holds the claim for what its weave hears, may say and persists;
  a Kernel's loaded-artifact record holds its manifest's; an isolation `Link`
  holds its mount's. The Registry is told nothing about weave identity, artifact
  names or mounts — it counts claims, and the layer that owns a lifetime is the
  layer that owns the claim;
- **a producer claims what it may speak.** A weave's accept-set is what it will
  hear; its grant's *named* send rules are what it may say, and those shapes are
  claimed by key (`claim_known`) so an authorized sender keeps its vocabulary
  after the weave that defined it is gone. A wildcard rule names no shape and
  therefore claims none;
- **a handoff has no gap.** A successor's claim is acquired before the
  predecessor's is released, so a shape both require is doubly claimed for the
  length of the swap and never falls to zero. A refused replacement releases the
  candidate's claim instead — a rejected reload candidate leaves no vocabulary
  behind;
- **nothing removed means nothing released.** `unregister_weave` refusing under
  [LIFE-06](#life-06--a-weave-outlives-its-own-callback) does not touch claims,
  because it does not touch anything.

DOES NOT MEAN
- **that a value can lose its schema.** A `Value` owns its schema strongly, and
  `lookup` hands back a strong owner. "Reclaimed" means *undiscoverable through
  this Registry*, never *destroyed*: an admitted value, a reader mid-decode and
  a caller holding an older snapshot all remain correct after the final release;
- **immediate heap reclamation.** Live reader snapshots and existing values may
  keep a removed schema alive for as long as they like. The guarantee is
  reachability, not free();
- **garbage collection.** Nothing traces, nothing runs periodically, nothing
  decides on a host's behalf. A claim is released by an owner being destroyed,
  at exactly that moment, on that thread;
- **provenance.** A claim answers *why must this vocabulary still resolve?*, never
  *who wrote this schema?*. Claimant, author, first registrar and current
  producer remain unrelated;
- **that every Registry-shaped store in the system is bounded this way.** The
  authoritative host registries are: the Switchboard's (the vocabulary every raw
  emission is gated against), the Kernel's manifest dependency registry, and an
  isolation host's. `RemoteConsole::registry_` is a client-side **learned mirror**
  with no claimant and is still append-only — see
  [known-seams](../reference/known-seams.md#a-remote-consoles-learned-schema-mirror-is-still-append-only);
- **that a shape stays speakable because it once was.** When the last acceptor of
  a shape leaves and no authorized producer claims it, the shape stops resolving,
  and a later emission of it meets the seam
  ([MSG-08](messaging-laws.md)) rather than being routed to nobody.

PROVEN BY — the claim/release pair in `detail::RegistryCore`
(`src/registry.cpp`), the scope member on `Switchboard::WeaveRecord`,
`Kernel::Loaded`/`Kernel::Manifest` and `IsolationHost::Link`. Suites `registry`
(the lifetime, shared-claim, conflict, transactionality, permanence, producer,
move/overlap, surviving-value, orphan-scope, boundedness and
concurrent-churn cases), `switchboard` (arrival/departure, shared shapes,
refused-removal, code swap, producer claim and its wildcard control, refused
mid-accept-set registration, 300-weave churn), `kernel` (the agreement wall
following live claims, no residue from a rejected candidate, 64 rejected
candidates), `isolation` (mount-scoped claim, repeated mount/unmount) and
`policy` (a broker unmounted under an authorized client).
