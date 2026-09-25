# Bridge (the crossing between two hosts) — reference

The host side of a connection from another process: a framed socket, an
**admission decision** the host makes over every connection, a per-session
**proxy-participant** on the bus under the grant that decision chose, and the
protocol the two sides speak. Laws:
[MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender),
[GATE-01](../laws/admission-laws.md#gate-01--one-gate),
[MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo),
[MSG-12](../laws/messaging-laws.md),
[LIFE-07](../laws/lifecycle-laws.md#life-07--consumed-transport-bytes-are-history-not-live-channel-storage).
Capacities: [bounds](bounds.md#bridge-remote-operator). The supplied host's
use of the connecting side: [running-loom](../guides/running-loom.md#10-link-to-another-host).

**Read the admission section before you bind a listener anywhere.**

## What Bridge is

A **`Switchboard&` cannot cross a socket.** What crosses instead is:

```text
a handshake      "here is who I claim to be, and what I present"  -> admitted as X, or refused: why
discovery        "who is on the bus, and what shapes do they accept?"
a send           serialized message bytes the host re-admits, stamps and routes
a delivery       serialized bytes routed to this session, WITH what the bus stamped on them
a settlement     the host's word that what a send asked to follow has all been dispatched
the tap          a COPY of each bus event -- only for a session admitted with observation
```

The bus itself stays entirely host-side, single-threaded, FIFO.

**The shape is deliberately the out-of-process weave's.** `BridgeServer`
registers one proxy per admitted connection — an ordinary `Weave` on the bus
whose `handle()` ships each delivery down its socket — exactly as
`IsolationHost` registers an `OutOfProcessWeave` per mounted child. Same
proxy-participant pattern, pointed at a peer.

**Two consumers, one crossing.** The *operator's* console
(`zen-console-remote`, `zen-bridge-probe`) is one principal with the whole bus;
a *guest* — another Loom host's participant, a Workshop's agent — is admitted
deliberately and granted narrowly. The crossing serves both through one
policy seam, and the difference between them is entirely the policy's answer.

**Where it lives.** `loom::bridge` (`zen-bridge`: the channel, the protocol, the
server, the console-free client, the link envelope) is **exported** by
`find_package(loom)` since the two-host crossing: a Loom host that links to
another host reaches it that way, and Loom's own supplied host does. The
operator's console over it (`RemoteConsole`, `zen-bridge-console`) stays
unexported with the console engine it needs. Nothing of the console's
dependency closure crosses the package boundary.

## Admission: who decides, and when

Reaching the socket is not authority. A connection has **no proxy on the bus —
and so can act on nothing — until the host's policy has answered its Hello.**

```cpp
using BridgeAdmission = std::function<ConnectionVerdict(const ConnectionRequest&)>;
BridgeServer server(bus, listener, policy);
```

The policy sees what the peer **claimed** — a name, a credential, a protocol
version — and where it came from, and answers one of three things:

| verdict | what happens |
|---|---|
| **Admit** (`ConnectionAdmitted`: a `loom::Grant`, an accept mode, an established name, `observe`, a payload `encoding`) | a proxy is registered under that grant; `Welcome` carries the session id and the established name; sends are stamped and routed |
| **Refuse** (a reason) | `Denied` carries the reason, the connection is severed, nothing was registered and nothing it sent acted |
| **Defer** | the connection waits in `AwaitingDecision`: its sends are refused aloud (`SendRefused: not admitted`), never queued; `BridgeServer::decide(connection, verdict)` settles it later |

`Defer` + `decide` is the seam an interactive decision attaches to: a console
command today, a per-connection popup tomorrow. Nothing downstream of the
verdict changes for it.

`operator_admission()` is the old model stated as a policy: every connection is
the operator — `allow_any()`, accept-any, observing the whole bus — and its
established name is whatever it claimed. **Under that policy reachability of
the socket is authority**, and a listener served with it must not be exposed
to an untrusted network. It is what the in-tree consoles use.

A peer speaking another protocol version is refused in words
(`protocol v3 is not spoken here; this host speaks v4`) before the policy is
asked. A malformed Hello is refused. Any frame before Hello severs.

### Identity

Three facts, kept apart on purpose:

```text
claimed name        the peer's word about itself          data the policy judges
established name    the host's word about the session     the policy's answer, on Welcome
session             the proxy's WeaveId on this bus       minted at admission, never reused
```

The session is the only one of the three that is an identity here. It is what
the bus stamps as the sender of everything the connection sends, what a target
sees in `mail.sender()`, and what an answer is delivered to. A reconnect is a
**new** session; a late answer addressed to an old one reaches nobody
(`NoSuchTarget` on the tap). A remote integer id is never local authority: the
policy's grant is, checked at every send, under the proxy's own id.

**Wire `sender` and `reply_to` are read and discarded.** A forged `reply_to`
cannot redirect a session's replies to a third party, and a forged `sender`
cannot impersonate another participant
([MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender)); pinned
by forging the hostile frame directly (suite `bridge`).

## What a session is told

`Delivered` carries what the bus stamped on the delivery beside the payload:

```text
sender            the bus's stamp of who spoke
correlation       the number the session put on its ask, echoed by an answer
answers_ask       Loom's own word that this is THE answer to an ask this session sent (ANS-05)
dispatch_refused  this is a zen.DispatchRefused notice about one of this session's own sends
authored_role     the office the sender deliberately spoke as, or empty
```

A peer that only got bytes could not tell an attested answer from any admitted
participant's helpful `zen.Result`; v4's crossing no longer discards the
difference.

A `Send` may carry `kSendSettle`. The host then opens a
[fence](messaging.md#fences-when-what-one-send-set-in-motion-has-been-dispatched)
on the envelope it queues and, once everything that envelope set in motion on its bus has
been dispatched, tells the session `Settled` under the send's correlation — once. That is
not an answer and is not ordered against one: a far owner may answer first (its answer is
a delivery the fence itself counts) or later (a deferred answer spent from an unrelated
delivery is not the fence's). It says nothing about work deferred to a timer or a later
turn. A connection waits on at most `kMaxSettlingPerConnection` settlements; past that,
or past the bus's own `kMaxFences`, the Send is refused **before the bus** — never quietly
sent without one — and a publication, which has no one delivery to follow, cannot ask. The proxy declares `zen.DispatchRefused` among its doors so a
session hears its own dispatch refusals
([MSG-12](../laws/messaging-laws.md)), and the construction layer's answer to
`zen.DescribeAccepted` (`zen.AcceptedShapes`), which no ordinary participant
declares, so a session allowed to ask what a weave accepts has somewhere for
the answer to land -- whatever else its accept mode admits.

**The outcomes of one send, kept apart** — because they send a peer to
different places:

| outcome | how the peer learns it |
|---|---|
| refused at admission | `Denied`, then severance |
| dropped before the bus (malformed header, unknown shape, gate refusal, not admitted) | `SendRefused` with the correlation and the reason |
| refused by the bus (`CapabilityDenied`, `NoSuchTarget`, …) | `Delivered` flagged `dispatch_refused`, carrying the notice |
| answered | `Delivered` flagged `answers_ask`, under the correlation |
| what it set in motion has been dispatched (asked with `kSendSettle`) | `Settled`, under the correlation, once |
| the socket ended after the send | nothing — the outcome is **unknown**, and a peer must not resend on its own |
| silence | nothing; the ask is still open on the peer's own book |

**What a peer that asked for settlement owes itself.** The two outcomes above are separate
frames, unordered against each other, and a peer that asked for both has not got what it asked
for until both have come. Loom's own Python client holds that rule: a conversation with
`settle` is complete on its attested answer AND its `Settled`, in either arrival order, and a
wait that runs out says which half is missing while keeping the half that came
([sessions § 7](../guides/sessions.md#7-writing-a-tool)). A refusal — of the send or of the
dispatch — completes such a conversation whatever was asked, because nothing is then in motion
to settle. A peer that treats the answer alone as the end has silently asked for one thing and
waited for another; the flag then costs a frame and buys nothing.

## The payload encoding a session speaks

A payload is one of Zen's two serializations, and the host's policy chooses which, per session,
when it admits it (`ConnectionAdmitted::encoding`):

| encoding | what crosses | for |
|---|---|---|
| `Native` (the default) | the canonical binary: positional, schema-guided, carrying the schema's content id | every Loom host speaking to another; a C++ peer |
| `Compat` | the self-describing JSON envelope, `{"zen":1,"schema":...,"version":...,"fields":{...}}` (`compat::serialize` / `compat::parse`) | a peer that is not written in C++ — a Python client or worker of a [session](../guides/sessions.md) |

Nothing else changes: the frames are the same v4 frames, a compat Send is parsed as JSON and then
**re-admitted through the same gate** against the shape this bus resolves (with the same decode
budget, and the JSON decoder refuses a field the shape does not declare), and a compat session's
deliveries, and its `Describe` answers, are the admitted values serialized as JSON. One session
speaks one encoding: a compat session's native payload is refused before the bus in words, and a
native session's JSON is refused as any malformed payload always was. Authority does not depend on
the encoding. The supplied host's session door admits every session as `Compat`; Workshop's guest
door, and every link between hosts, stay `Native`. Suite `bridge`, the `encoding:` cases.

## The tap

A session admitted with `observe` receives a copy of every bus event, as the
operator always did. A session admitted without it receives **only** what is
delivered to its proxy. A guest is not given a tap by any policy Loom ships;
richer observation of a host is that host's to grant through its own owners.

## The connecting side

`BridgeClient` (`zen/bridge/client.hpp`) is a socket, a handshake and a frame
reader, with no console in it: `hello(claimed, credential)`,
`await_admission(ms)`, `send`/`send_to_role` (optionally asking for settlement)
and `publish` under a correlation, `describe`, `poll(events)` — every far frame
decoded to a `BridgeEvent` the caller owns. A send is **queued** until the next
`poll` or `flush`. It decodes no payload: the shapes belong to the participant
that asked.

`zen/bridge/link.hpp` is how an ordinary weave asks across a link a host holds
for it: `loom.link.Ask{role | target, payload bytes, settle}` to the link's
office, under the asker's own correlation. The link answers each ask **once,
through Loom's answer door** — a deferred answer Loom binds to the asker's
incarnation and correlation — so the asker's `mail.answers_ask()` is its own
bus's word. The answer is either the far owner's (only a delivery the far bus
attested as the answer to this crossing, re-admitted through the local gate,
never in the link's own `loom.link.*` vocabulary) or `loom.link.Outcome{refused
| dispatch-refused | unlinked | lost}`, the link's word for a crossing that did
not come back that way. A far participant's ordinary speech to the session is
never an answer. The asker's correlation never crosses: the link puts its own
`attempt` on the wire — never reused — and a session's `epoch` bounds what a
reply can reach, so equal correlations from separate askers, replies in any
order, duplicates and late words from an ended session each land where they
belong or nowhere. With `settle` the answer is held until the far host's
`Settled`. Every far frame becomes a `loom.link.Crossed` record the link says
to itself — far session and established name, far stamp and office, attempt,
kind, bytes — which is how the host's history holds the crossing, and what the
link acts on (only its own record acts). The supplied host mounts one such link
per `links` row of its boot plan
([running-loom § 10](../guides/running-loom.md#10-link-to-another-host)).

**An asker that does not speak the canonical binary** — a Python worker over a compat session —
may put Zen's JSON envelope in `loom.link.Ask.payload`. The link admits it through **this** bus's
gate against the shape this host resolves and puts the admitted value's canonical bytes on the
wire, so the far host receives exactly what a C++ asker's `ask_role` sends. A shape nothing here
declares cannot be encoded and is refused (`Outcome` `refused`, attempt 0) before anything crosses,
as is an envelope the gate refuses. That is the same requirement the far ANSWER already had: to
speak a far vocabulary through a link, some participant of this host must declare it.

**A subscription made through a link** — `loom.observe.Subscribe` to the far host's relay
([observation](observation.md)) — is the one far conversation that outlives its answer. The link
keeps its **custody**: the far relay's lifetime and subscription number, the far participant that
answered, the session (`epoch`) and the local asker. It forwards that relay's later words
(`Observed`, `Gap`, `Ended`) to the asker as ordinary speech — never as an answer — with `link`,
`epoch` and `session` filled and `cause` translated from its own attempt to the asker's own
correlation, or 0. A word about any other subscription, from any other far participant or from an
ended session is counted (`stray_observations`) and dropped. When the session ends, each
subscription it carried is told `Ended` `lost`; when the asker is gone, the link asks the far relay
to release. At most `LinkWeave::kMaxWatches` per link ([bounds](bounds.md#observation-relay)).

## Servicing

`service()` is the I/O half and only that: accept, read and dispatch every
complete frame (a Send is one gated `send_as` under the proxy's grant; a Hello
is one admission decision), push a deferred discovery refresh, flush, reap. It
takes **no bus turn**, so a host whose loop is a delivery-driven drain services
the crossing from inside a delivery — Zengine's Workshop does, on its Timer's
beat. `step()` is `service()` plus one bus turn: drain-to-idle by default, or
the bounded turn under `set_bounded_dispatch()`
([MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo)).
`wait_and_step(ms)` blocks in `poll`/`WSAPoll` over `{listener, connections}`
first; `run(tick_ms)` loops that until `stop()`.

Registry reads are never done inside the tap observer callback: `on_tap` copies
event fields and sets a dirty flag; the refreshed weave list is pushed after
dispatch returns.

`on_connection(tell)` reports every state change of every connection — accepted,
awaiting a decision, admitted, refused, closed — with the connection as it
stands, so an inventory can show a session and drop it rather than show a dead
one as live. `connections()` is the same inventory on demand.

A server may be **owned by a weave** -- Workshop's guest door is one -- and die
with the bus. Two things then hold. The Switchboard's destructor empties its
registry into a local before any weave is destroyed, so the server's own
re-entrant unregistration finds nothing to erase from a map being torn down;
and a proxy, which the bus may destroy *before* the weave that owns the server
(registry order, not the server's), clears the connection's pointer to it as it
dies, so the server's destructor finds a null rather than a freed object. The
bridge suite's `teardown:` case holds both, and the sanitizer lane is where the
second one showed.

## Validation: what Bridge checks, and what it deliberately does not

Four layers, in the order a byte meets them. Only the first two are Bridge's.

| Layer | Owner | What it decides |
|---|---|---|
| framing | `BridgeChannel` | is this a complete, in-bounds frame? |
| protocol | `BridgeServer::on_frame` | is this opcode legal *here*, in this connection's state? |
| conformance | the one gate (`admit`) | are these bytes a well-formed instance of the shape they claim? |
| meaning | the receiving weave | is this request *sensible* for my domain? |

**Framing.** Length-prefixed `[u32 payload_len][u8 op][payload]`, little-endian,
read through a bounds-checked `Cursor` — a truncated or lying length is
rejected, never over-read. A frame over `kMaxFrameLen`, or an undrained backlog
over `kMaxBacklog`, fails the channel.

**Protocol.** The handshake is load-bearing and anti-Postel: **any frame before
`Hello` severs the connection**, and so does a second Hello. Host→client
opcodes arriving inbound are ignored. A malformed `Describe` is dropped. A
Hello's claimed name and credential are bounded (`kMaxHelloFieldBytes`),
because a peer has earned nothing yet.

**Conformance.** A session's `Send` payload is re-admitted host-side through the
**one gate**, exactly as a loaded library's emission and an isolated child's
`Emit` are ([GATE-01](../laws/admission-laws.md#gate-01--one-gate)). Bridge
resolves the claimed `(name, version)` against the bus registry, refuses if it
is unknown, and refuses if `admit` refuses. A bridge frame can therefore never
register a schema or introduce a shape.

**Meaning is not Bridge's, on purpose.** A structurally valid message that is
nonsense for the domain is delivered, and the receiving weave refuses it.

An operator's bytes are decoded host-side, so they spend the host's shared
`kMaxDecodedCells` allowance — a compact frame cannot command unbounded host
materialization ([bounds](bounds.md#the-decode-materialization-bound)).

## Authentication posture

**Bridge authenticates a credential exactly as far as the host's policy does,
and provides no transport security.** There is no TLS and no peer-credential
check in the mechanism; a credential in a Hello is bytes the policy compares
to something it holds. That is enough for a maker's own two processes on one
machine, and it is what it is: a shared secret, as private as the file it lives
in.

> **Do not bind a listener on an interface an untrusted party can reach.**
> Under the operator policy, reachability is operator authority; under any
> policy, a credential crosses in the clear.

Two facts about the shipped listeners: `bridge_listen_tcp` binds
`INADDR_LOOPBACK` unconditionally (a mitigation, not authentication — WSL2
forwards localhost by design, and any forward re-exposes the port);
`bridge_listen_unix` sets no socket-file permissions.

## Trust and threat posture

| | Current state |
|---|---|
| **transport boundary** | a framed stream socket, bounded and non-blocking; a misbehaving peer is contained, never allowed to block, hang or exhaust the host |
| **connection authority** | the host's `BridgeAdmission`: a grant of the policy's choosing per session, or refusal, or a deferred decision |
| **conformance validation** | the one gate, host-side, on every send; unknown shapes refused |
| **application semantics** | **absent by design** — the receiving weave's job |
| **authentication** | a credential the policy judges; no transport security |
| **sandbox containment** | **not applicable** — Bridge does not sandbox anything |

An admitted session is an ordinary participant with the grant it was given: no
lifecycle authority, no ungated send, no grant assignment, no Sense read
authority, and — unless the policy says `observe` — no tap. Threat tier:
**abuse, not escape**.

## Bounds

`bounds.md` is the capacity authority. Past the connection cap the server
**accepts then closes** and counts it (`declined_count()`); refusals are counted
too (`refused_count()`).

## Tests

Suite `bridge` — transport round-trip and EOF-as-an-event (both AF_INET and
AF_UNIX), the forged-wire-sender pin, the connection cap, the pre-`Hello`
severance, hostile-`Send` refusal, malformed framing, the client's bounded
caches, a SIGKILLed peer reaped across two real processes, the bounded-dispatch
cases, the C-1 remote-window cases — and the crossing's: a refused connection
registers no proxy and its sends act on nothing; the established name is the
policy's word; a guest's send is stamped from the session and answered with
Loom's attestation; what the grant does not cover is refused at the bus and the
guest is told; a deferred connection acts on nothing until decided; a version
mismatch is refused in words; a guest gets no tap and an operator still does; a
disconnected session's proxy leaves and a late answer settles nothing; and the
link, end to end across two buses — two askers under one correlation answered
in reverse order, answers and refusals each reaching their own asker, a far
participant's ordinary word under an ask's correlation, duplicates, unknown
attempts and a reply for an ended session, a forged local record, an answer in
the link's own vocabulary, a replaced asker, and settlement in either order and
against real far work. `tests/package/stranger_bridge.cpp` reaches the server,
the client and the link envelope through `find_package(loom)` alone.
