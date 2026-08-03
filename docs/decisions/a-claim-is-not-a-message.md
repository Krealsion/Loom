# A claim is not a message, and must not become a second state system

**Status: current (R2E-0).** Laws: [SENSE-01..05](../laws/sense-laws.md).
Reference: [senses](../reference/senses.md).

## The pressure

Consumers kept wanting the same thing: *what does this participant currently
claim is true?* A renderer, an inspector, a UI binding, a status panel, an editor
warning — each asks repeatedly for state somebody already knows.

Expressed as traffic that is `ask → FIFO → handler → answer → FIFO → reader`:
six steps and two queue turns, adding latency and envelopes and **no causality**.
The bus is the wrong instrument for a question whose answer is already sitting in
somebody's field.

## The risk that shaped every decision

The obvious failure is not "Senses do not work". It is **Senses working so well
that Zen quietly acquires a second state system beside the message system** — a
shadow world where things are true without anything having happened, where
readers couple to producers' internals, and where the FIFO story stops being the
whole story.

Every decision below is that risk being priced.

## The decisions

### Visible at the claim call, not at handler completion

Three settlement rules were available: at the call, at handler completion, or at
an explicit settlement step. The third is a second lifecycle rail and a
forgettable one. Between the first two the decisive fact is that they are
**externally indistinguishable**: dispatch is single-threaded and non-reentrant
([MSG-01](../laws/messaging-laws.md)), so no other participant can run between a
claim call and the end of the handler that made it. The only observer that can
tell them apart is the claimant observing its own claim.

So Loom takes the simpler one and gains no hidden transactional handler
semantics. The ordering guarantee consumers actually wanted — a reader queued
behind a change sees it, one queued ahead does not — falls out of FIFO, not out
of anything the repository does. **The repository does not participate in
causality at all**, which is the sharpest available answer to "is this a second
state system?".

### By value, always

A reading owns its value. No pointer, no reference, no alias into a claimant
appears anywhere in the type, so `other.sense.health = 9000;` has no spelling.
The cost is a copy per read; the thing bought is that coupling a reader to a
producer's memory is not a discipline anyone has to maintain.

### Two key spaces, not one keyed by "who currently holds the office"

Personal claims are keyed by `WeaveId`; office claims by role name — separate
maps. This makes "holding the office is not claiming as the office"
**structural** rather than a check somebody could forget, and it is
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit)'s law in
a new category, reusing the same `as_role(...)` grammar because it is the same
law.

### Stale claims are stamped, never withheld

After a role moves, a role-bound reading returns the predecessor's claim with
`office_holder_is_current = false`. The rejected alternative — return nothing —
collapses *"this office has never claimed"* and *"this office's claim is the
previous holder's"* into one empty answer. Those are different facts, and the
implementation discipline forbids an ambiguous empty result where refusal, stale
and missing are meaningfully different. A reader wanting the strict view writes
one visible line, and it being visible is the point.

### A new, narrow, default-absent read authority

Reading requires an observe rule the host grants explicitly. Reusing a send rule
was the convenient option and was rejected by name: a send rule answers *"may you
emit this shape **there**"*, a read answers *"may you pull it"*, and reporting one
as the other sends an operator to edit the wrong thing.

Because the floor is empty, **no existing weave gained any reach from Senses
existing**. That is what keeps a latest-claim repository from becoming a
universal data-exfiltration rail — the thing a "just let anyone read it" design
would have quietly built.

### A third declaration list

`Claims<...>` is not `Emit<...>`, twice over: a Sense is not an emitted message,
and `Emit` is informational and does not register — so discovery would still have
had to wait for a runtime claim to accidentally reveal a shape (the Rule Garden
already hit exactly that with response-only shapes). `Claims<...>` registers at
mount and is enforced at claim time.

## The honest answer to the risk

A Sense **is** state that lives outside a weave's own fields, and pretending
otherwise would be marketing. What keeps it from being a second state system is
that it has none of the properties that make a state system a parallel world:

- it cannot be written except by its subject, deliberately, once per key;
- it cannot be read without authority;
- it carries no causality and imposes no ordering;
- it never predicts, retries, or reconciles;
- it holds no history;
- it is bounded by current keys, so it cannot accumulate a world.

It is a **published latest claim with truthful authorship** — closer to a
weave's snapshot (which Loom has always had, and which nobody calls a second
state system) than to a database. The category exists so that a consumer asking
"what do you say is so?" does not have to fake it with traffic.

## What would falsify this

If applications start using Senses to **coordinate** — claiming in order to make
another participant act, or reading in a loop to detect a change — the category
has been misused into a message bus, and the correct response is to make that
harder, not to add change notification. Change notification is exactly the door
through which a second state system would walk in, and this phase deliberately
did not build it.
