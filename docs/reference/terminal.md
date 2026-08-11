# The terminal session — reference

An **ordinary Loom weave that a presentation can drive**: one identity, one
grant, one supplied vocabulary, its own transcript. Laws:
[MSG-01..07](../laws/messaging-laws.md),
[ANS-01..07](../laws/answer-authority-laws.md),
[GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides).

```text
the Kernel enforces.  the Weaver decides.  the session acts.
```

[WEAVER-1](weaver.md) built the second line. This is the third, made into
something you can hold — and the test of whether it earned the name is that
everything it can do is an **ordinary participant's power**, or is not here.

## What it is, and what it is not

| | is | is **not** |
|---|---|---|
| `loom::TerminalSession` | one weave with one WeaveId, one admission grant, a host-supplied vocabulary and its own transcript | a host, a root, an administrator, or anything that starts with more authority than the host gave it |
| `loom::ParticipantChannel` | an outbound door bound to one identity | authority — every message through it is authorized at delivery against that weave's own effective authority |
| `loom::TerminalVocabulary` | type knowledge: which schemas may be described and composed | permission to send them, or evidence that anybody accepts them |
| `loom::Transcript` | what this participant legitimately came to know | a bus log, a journal, a delivery record, or another weave's traffic |
| `loom::TerminalDesk` | one presentation showing two participants | one participant — it refuses to pair a weave with itself |

Held by a terminal session: **nothing else**. No `Switchboard&`, no `Kernel&`,
no `IsolationHost&`, no host root send, no observer, no journal, no registry
read, no weave enumeration, no `allow_any`, no `observe_any`, no load
capability, no filesystem and no network. Being a terminal confers **none** of
those, and there is no verb on its channel that takes a sender, so it cannot
speak as anybody else — not because it is refused, but because the sentence has
nowhere to put the other identity.

## Presentation is not authority

One screen may present several participants. That does not merge them.

```text
session>   the governed participant       weave A, grant A, vocabulary A
operator>  the seat the Weaver obeys      weave B, grant B, vocabulary B
debug>     the HOST looking                not a participant at all
```

Every command is authored by exactly one participant, named at the moment it is
given. There is **no fallback**: a command the session lacks authority for is not
retried as the operator, and one the operator lacks authority for is not retried
as the host. That would be privilege escalation by convenience.

`TerminalDesk` is where the refusal to merge lives, and it refuses at
construction rather than at the decision that would abuse it — the same wall
`loom::Weaver` puts between its operator seat and its governed subject, one layer
up:

```cpp
loom::TerminalDesk desk(session, operator_seat);  // throws if they are the same weave
```

## The three powers "inspect" used to hide

```text
TYPE KNOWLEDGE       which shape schemas can be described and composed
SERVICE DISCOVERY    which weaves and roles currently exist
TRAFFIC OBSERVATION  which messages are flowing
```

A terminal session has the **first** and neither of the others.

Its vocabulary is **supplied by the host at mount**, not discovered. That is
narrower than the console's registry read on purpose: enumerating a live
Switchboard's registry is a fact about the running world rather than about this
participant, and it needs a `Switchboard&` no ordinary weave holds. Service
discovery is deliberately **omitted** — a terminal that can speak to an office
the user names is a useful terminal, and none of the workflows below needed it.

`knows()` adds type knowledge; `accepts()` adds type knowledge **and a door**.
The accept-set handed to the bus is exactly the doors — never the catalog, and
never `AcceptMode::AnyRegistered`.

## Submitted, received, answered — and never "delivered"

```text
SUBMITTED  I authored this and Loom took it. Whether it was delivered, refused
           at the gate, refused for want of authority, or dropped for want of a
           target, I DO NOT KNOW.
RECEIVED   this arrived here, past my own door.
ANSWERED   ...and Loom itself says it answers an ask I made.
```

This inherits the standing
[sender-cannot-observe-send-fate](known-seams.md#sender-cannot-observe-send-fate)
seam and does not paper over it. A `Submitted` entry carries **no** outcome
field to be filled in later, and two sends with opposite fates produce
identical records (pinned in suite `terminal`). The console can print
"delivered" because a console holds a `Switchboard&` and reads the journal; a
participant cannot, and does not.

The visible consequences a terminal UX must live with:

- a denied send looks exactly like a successful one, so **nothing may auto-retry
  or auto-request authority on denial** — the user asks, explicitly;
- "awaiting an answer" means only that. Not that the request was delivered, not
  that anyone is working on it, not that a person saw it;
- a request to a policy office whose seat is unreachable simply stays pending.

## Ask, answer, and which conversation an answer belongs to

An ask is an ordinary gated send that the participant additionally remembers. No
protocol is added and the far end is not obliged to answer.

What settles it is **Loom's provenance and Loom's correlation**, never a shape
that looks like a reply:

```text
mail provenance   answers_ask() — written only by the Switchboard's answer doors
correlation       the ASK's own, copied onto the answer by enqueue_answer()
```

Both the immediate answer door (`answer_as`) and the deferred one
(`spend_deferred_as`) reach the same `enqueue_answer`, which sets
`msg.correlation = <the request's correlation>`. So an ordinary participant can
tell **which** of several outstanding asks an answer belongs to, from Loom's own
record — no request id is invented, and none is needed.

A terminal therefore supports **several outstanding conversations**
(`kMaxOutstandingAsks`, currently 8). The bound is the terminal refusing to grow
an unbounded map, not a limit of Loom: the (N+1)th ask is refused **locally**,
nothing is authored, and the N already outstanding are untouched — a new ask
must never displace a conversation somebody is waiting on.

This is strictly stronger than the standing consumer obligation for the
[standard reply shapes](../../include/zen/weave/standard_shapes.hpp) ("match
correlation AND bus-stamped sender"): an unsolicited `zen.Ack` from a weave that
merely holds the grant for it carries no answer provenance at all, so it is
recorded as the unsolicited message it is and settles nothing.

### Awaiting, and stopping

The core never pumps. It owns the *state machine*; the presentation owns the
*loop*:

```cpp
while (session.awaiting() && turns-- > 0) { bus.pump(); }
```

No thread, no sleep, no busy loop, and Loom dispatch is never blocked.

`cancel_ask` stops waiting **locally and only locally**. Loom has no
cancellation vocabulary, so nothing at the far end is told and its answer may
still arrive; when it does it is recorded as the authenticated answer it
genuinely is, matched to no outstanding ask. The transcript says exactly that.

## Authority

Requesting authority is **sugar and only sugar** over an ordinary role-addressed
ask:

```cpp
session.request_authority("Work", 1, "some.service", "so I can finish the job");
// == ask(to_role("loom.weaver"), "zen.RequestAuthority" v1, {shape, version, to_role, purpose})
```

There is **no Weaver in the terminal core**: no include, no type, no branch. The
office is an address, the request is an ordinary composed message out of the
host-supplied vocabulary, and a host that did not supply that shape gets an
ordinary `UnknownShape`. The operator's four decisions (`Approve`, `Refuse`,
`Revoke`, `DescribeAuthority`) are not even sugar — a presentation authors them
as ordinary messages from the operator seat's own door.

**Approval performs nothing.** `TerminalSession::handle` contains no send of any
kind — not a reply, not a retry, not an acknowledgement — so "authority is not a
broker" is a property of the shape of that function rather than a rule somebody
has to keep. The user retries, explicitly, and the target sees the *session*.

## The transcript

A presentation-neutral model, not output. Entries carry structured facts so a
console renderer, a graphical pane and a future executor can each present the
same record without parsing each other's strings.

Trusted facts are kept apart from payload, always:

```text
sender          the bus stamp                     TRUSTED
authored_role   an office Loom verified at authorship, or empty
answers_ask     provenance no ordinary enqueue can write
<payload>       whatever the message says about itself   NOT trusted
```

A payload field that happens to be called `requester` is never copied into a
trusted one. For `zen.AuthorityPrompt` specifically: the **Weaver** is the
sender, `prompt.requester` is the Weaver's own trusted fact, and
`requester_says` is prose the requester wrote — the field's own name says so.

### Bounded, and never at the cost of a conversation

```text
kTranscriptCapacity  256 entries      metadata; a session's worth of scrollback
kReceivedCapacity     64 messages     the retained Values, bounded only by the
                                      decode budget, so a smaller window
```

Both count what they dropped (`evicted()`), because a bounded surface that
claimed to be complete would trade a memory lie for an observability lie. A
received-message id is a **stable identity**: once evicted it refuses rather than
re-binding to a newer message, and `$rN.field` references say which absence it
was.

Outstanding asks live in the session, **not** in the transcript, so scrolling
past the horizon can never lose the fact that this participant is still waiting
(pinned in suite `terminal`).

### Ordering

`ObservationOrder` is *the order this presentation observed events* — never a
canonical global Loom history, which would need the tap. Two participants driven
by one host loop may share one counter, and then a merged chronology means
something exact. `TerminalDesk` **requires** that they share one; otherwise
"merged" would be an interleaving the presentation invented.

## Text

The core keeps every received value **verbatim** — not escaped, not truncated,
not normalized. A core that sanitized what it stored would decide, once and for
everybody, what every future renderer may see.

`loom::safe_terminal_text` is a **renderer's** rule, applied at the last moment
by whichever presentation has a terminal to protect. It escapes exactly the bytes
that can steer one — C0 controls and DEL — and passes everything at 0x80 and
above through untouched, so UTF-8 survives. That is sufficient: no UTF-8
sequence can encode a C0 byte.

It is deliberately **not** the Weaver's `safe_operator_text`, which escapes every
non-ASCII byte. That one guards one small security-critical surface where being
unreadable is cheaper than being wrong; a terminal that did the same would
corrupt every ordinary message carrying a name. Both exist, and neither is
applied to a value.

## The console is a different thing, and stays one

| | `loom::ConsoleEngine` | `loom::TerminalSession` |
|---|---|---|
| what it is | a trusted **host/debug operator lens** | an ordinary delegated participant |
| holds | `Switchboard&`, a whole-bus tap, the journal | a vocabulary and one identity-bound door |
| grant | `allow_any()` | whatever the host gave it, and nothing more |
| accepts | `AcceptMode::AnyRegistered` | exactly its declared doors |
| can enumerate weaves/roles | yes (registry read) | **no** |
| can see other weaves' traffic | yes (tap) | **no** |
| can say "delivered" | yes (journal) | **no** |

Both are legitimate; they are not the same security role and forcing one class to
serve both would have made the weaker one look like the stronger. The console
keeps its powers and its name. What moved down at TERM-0 is the **typed
composer** (`loom::compose_message` and the Arg/Ref/FieldValue vocabulary) and
the bounded-history primitive, so there is one assumption ladder and one ring
buffer rather than two of each; every name is unchanged, in the same namespace.

## Running it

`zen-terminal-repl`
([`src/terminal/terminal_main.cpp`](../../src/terminal/terminal_main.cpp)) boots a
governed session, a separate operator seat, a Weaver and a service, and lets a
person drive whichever participant they name.

**It hosts its own Loom.** It does not attach to another process: there is no
socket, no token, no authentication and no remote anything. A "terminal" here is
a presentation of participants that live in the same process.

Its `debug>` lens is the **host's** own tap and registry read, labelled as such
on every line, and wired into neither participant.

## What this does not govern

The [operator seat is a WeaveId, not a person](known-seams.md#the-operator-seat-is-a-weaveid-not-a-person):
the host designated it. There is no account, no login and no authenticated human
anywhere in this. A Weaver's death does not revoke what it installed, so a
terminal must not tell a person that closing anything took authority back. And
nothing here persists: restarting forgets every transcript and every approval.

PROVEN BY — [`include/zen/terminal/`](../../include/zen/terminal/),
[`include/zen/host/terminal_wiring.hpp`](../../include/zen/host/terminal_wiring.hpp),
`tests/test_terminal.cpp` (suite `terminal`), and the unchanged `weaver`,
`grant` and `console` suites.
