# Bridge (the remote operator crossing) — reference

The host side of the remote-operator console: a framed socket, a
per-connection **proxy-participant** on the bus, and the operator-protocol
they speak. Laws: [MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender),
[GATE-01](../laws/admission-laws.md#gate-01--one-gate),
[MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo),
[LIFE-07](../laws/lifecycle-laws.md#life-07--consumed-transport-bytes-are-history-not-live-channel-storage).
Capacities: [bounds](bounds.md#bridge-remote-operator). Console semantics:
[bounds § console](bounds.md#console-operator-history).

**Read the authentication section before you bind a listener anywhere.**

## What Bridge is

A **`Switchboard&` cannot cross a socket.** The in-process console holds one;
a remote console cannot. Bridge is the answer: the console *engine* runs
client-side (`RemoteConsole`), and its three host interactions — discovery,
the tap, and sends — cross as framed messages the host answers and streams.

What crosses is therefore not "the bus". It is:

```text
discovery      "who is on the bus, and what shapes do they accept?"
the tap        a COPY of each bus event, streamed as it happens
a send         serialized message bytes the host re-admits and routes
a delivery     serialized reply bytes routed to this operator
```

The bus itself stays entirely host-side, single-threaded, FIFO.

**The shape is deliberately the out-of-process weave's.** `BridgeServer`
registers one `OperatorProxy` per connection — an ordinary `Weave` on the bus
whose `handle()` ships the delivery down its socket — exactly as
`IsolationHost` registers an `OutOfProcessWeave` per mounted child. Same
proxy-participant pattern, pointed at an operator instead of a hosted weave.

**What it is for.** Driving a live Loom from another process or another OS:
the Windows console (`zen-console-remote`) attached to a WSL-hosted bus, an
operator attached to a long-running service, a non-interactive crossing proof
(`zen-bridge-probe`). Transport-agnostic by construction — AF_UNIX for the
local WSL↔WSL loop, AF_INET `127.0.0.1` for the real Windows→WSL crossing.

**What it is not for.** It is not an RPC surface for applications, not a
public network service, not a weave-hosting mechanism (that is
[kernel](kernel.md) in-process and [capabilities](capabilities.md)
out-of-process), and not an authentication boundary — see below.

**Where it lives.** `zen-bridge` (`loom::bridge`) is an in-tree library and is
**deliberately not exported** by `find_package(loom)`, alongside the console,
the TUI and the UI trio — those are Zengine-destined and each moves in its own
port phase. A consumer of the installed package has no Bridge.

## Connection authority

`authorize_connection(socket_t)` (`include/zen/bridge/server.hpp`) is the one
chokepoint. Today it returns `OperatorGrant{true}` for every connection —
**model A: reachability IS authority**. `BridgeServer::accept_new()` sheds a
connection past the cap first (accept-then-close, counted), and otherwise
consults the chokepoint before anything else happens on that socket: a
declined grant closes the socket, registers no proxy, and reads no frame.

An authorized connection is then registered on the bus as

```cpp
bus_.register_weave(std::move(proxy), loom::Grant{}.allow_any(),
                    loom::AcceptMode::AnyRegistered);
```

— the most-granted ordinary participant (broad send authority, accepts a reply
of any registered shape), and **still a grant, not host root**. An operator
holds no `Switchboard&`: it cannot mint a `LifecycleAuthority`, cannot
`send`/`publish` ungated, cannot assign grants. Its every send is authorized
against that grant at delivery, before role resolution and before the gate,
exactly like any other weave's.

That pair — the function and the one `register_weave` call that consumes its
result — is the entire connect-authority seam. Deferred models change only
those two lines: **model B** a bearer token (possession-is-authorization, no
identity), **model C** per-connection graduated grants, where the *weaver*
concept is born because differential authority is the first place
authorization needs authentication. The chokepoint is built; the token is not.

### Where connection identity comes from

The proxy's `WeaveId` **is** the operator's identity on the bus, and the host
stamps it from the connection on every send it makes on the operator's behalf.

**Wire `sender` and `reply_to` are read and discarded.** The `Send` frame
carries `wire_sender` and `wire_reply_to` fields; `BridgeServer::on_frame`
parses them (so the header's shape is fixed) and then constructs

```cpp
loom::Message msg(std::move(a).value(), loom::WeaveId{}, c.id, correlation);
bus_.send_as(c.id, loom::WeaveId{target}, std::move(msg));
```

with `c.id` — the connection's proxy — as both the stamped sender and the
reply target. An honest client sets both wire fields to 0; a malicious one
forges them and the bridge stamps over both. A forged `reply_to` therefore
cannot redirect an operator's replies to a third party (the confused-deputy
guard), and a forged `sender` cannot impersonate another participant
([MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender)).

This is pinned by a test that **forges the hostile frame directly** rather
than through the honest client API, because the honest API cannot express the
attack: suite `bridge`, *"the sender is stamped from the connection — a FORGED
wire sender loses"*.

`target` and `correlation` are *not* authority and are taken from the wire:
`target` is a destination the bus resolves and may refuse, `correlation` is an
opaque echo the operator chose.

**Every operator sees the whole-bus tap.** Under model A every connection is
the same principal, so `on_tap` copies each `BusEvent` to every connection.
Operator proxies are excluded from the discovery list (`push_weaves`) but are
still addressable by their small-integer ids — harmless while all operators
are one principal, and named here as the cross-principal surface model C
inherits.

## Authentication posture

**Bridge provides NO authentication.** There is no token, no key, no
challenge, no peer-credential check, and no TLS. `authorize_connection` does
not look at the socket it is handed.

The operational consequence, stated plainly:

> **Reachability of the bridge socket is operator authority.** Any party that
> can connect gets a full operator grant on your bus: it can enumerate every
> participant and the shapes they accept, read every bus event through the
> tap, and send any admissible message to any target.
>
> **Do not bind a bridge listener on an interface an untrusted party can
> reach.** Securing that reachability is a deployment responsibility, and it
> is the only safety property this component has.

`zen-bridge-host` prints that sentence at startup rather than leaving it to
documentation, and the header says it above the type. Two external reviews
have reached the same categorical position on a network-exposed Bridge — do
not — and nothing in the mechanism has moved since.

Two honest facts about the shipped listeners, one mitigating and one not:

- **`bridge_listen_tcp` binds `INADDR_LOOPBACK` unconditionally** — it takes a
  port and no address, so the shipped helper cannot be pointed at a public
  interface even by mistake. That is a real mitigation and it is not
  authentication: `BridgeServer` accepts *any* `socket_t` an embedder hands
  it, including one bound to `INADDR_ANY`; WSL2 forwards localhost by design
  (which is the crossing this exists for); and any tunnel, port-forward or
  proxy in front of it re-exposes the port. Loopback bounds *who can reach it
  by default*, never *what a reacher may do*.
- **`bridge_listen_unix` sets no socket-file permissions.** It `unlink`s a
  stale path and binds; access to the resulting node is whatever the ambient
  umask produced, and the bridge does not manage it. On a shared host, place
  it in a directory whose permissions you control.

Loom's own sockets set `FD_CLOEXEC` at creation. That is defence in depth so
Loom's descriptors do not walk into someone else's child; it is explicitly
**not** the sandbox boundary — see
[capabilities § the exec boundary](capabilities.md#the-exec-boundary-three-independent-facts).

## Validation: what Bridge checks, and what it deliberately does not

Four layers, in the order a byte meets them. Only the first two are Bridge's.

| Layer | Owner | What it decides |
|---|---|---|
| framing | `BridgeChannel` | is this a complete, in-bounds frame? |
| protocol | `BridgeServer::on_frame` | is this opcode legal *here*, in this connection's state? |
| conformance | the one gate (`admit`) | are these bytes a well-formed instance of the shape they claim? |
| meaning | the receiving weave | is this request *sensible* for my domain? |

**Framing.** Length-prefixed `[u32 payload_len][u8 op][payload]`,
little-endian, read through a bounds-checked `Cursor` — a truncated or lying
length is rejected, never over-read. A frame over `kMaxFrameLen`, or an
undrained backlog over `kMaxBacklog`, fails the channel.

**Protocol.** The handshake is load-bearing and anti-Postel: **any frame
before `Hello` severs the connection.** Host→client opcodes arriving inbound
are ignored. A malformed `Describe` payload is dropped.

**Conformance.** An operator's `Send` payload is re-admitted host-side through
the **one gate**, exactly as a loaded library's emission and an isolated
child's `Emit` are ([GATE-01](../laws/admission-laws.md#gate-01--one-gate)).
Bridge resolves the claimed `(name, version)` against the bus registry, refuses
if it is unknown, and refuses if `admit` refuses. A bridge frame can therefore
never register a schema or introduce a shape — it can only name one the host
already knows.

**Meaning is not Bridge's, on purpose.** Bridge does not know what any
application shape means and never validates a range, an invariant or a
sequence. A structurally valid message that is nonsense for the domain is
delivered, and the receiving weave refuses it — because collapsing those two
into one word would make "the bridge accepted it" sound like "the request is
sound". Application services validate their own semantic ranges; that is
stated the same way in
[capabilities](capabilities.md#work-the-host-does-on-behalf-of-a-contained-participant).

**One consequence worth naming.** An operator's bytes are decoded host-side,
so they spend the host's shared `kMaxDecodedCells` allowance — a compact frame
cannot command unbounded host materialization
([bounds](bounds.md#the-decode-materialization-bound); suite `bridge`, the
R2F-A end-to-end case).

## Bounds

`bounds.md` is the capacity authority; the numbers are not repeated here.

- connections, and the client's pending/absent-schema caches:
  [bounds § Bridge](bounds.md#bridge-remote-operator);
- per-frame and per-backlog transport caps, and the live-storage invariant
  that keeps a busy channel from growing by session volume:
  [bounds § transport channels](bounds.md#transport-channels-framed-byte-channels);
- what a remote operator *retains* — the tap window and the `m1/m2/...` reply
  buffer, both bounded histories with stable labels and visible eviction:
  [bounds § console](bounds.md#console-operator-history). The client-side
  windows are the same two constants as the in-process console, so a local and
  a remote operator see the same horizon.

Past the connection cap the server **accepts then closes**, and counts it in
`declined_count()` — a reconnecting fd-hog is contained (greedy is inside the
threat tier) and the shedding is observable, never silent.

## Trust and threat posture

Six things that are **not** one thing. Collapsing them into "secure" is the
error this section exists to prevent.

| | Current state |
|---|---|
| **transport boundary** | a framed stream socket, bounded and non-blocking; a misbehaving peer is contained, never allowed to block, hang or exhaust the host |
| **connection authority** | one chokepoint, `authorize_connection`; today full operator grant for every connection |
| **conformance validation** | the one gate, host-side, on every operator send; unknown shapes refused |
| **application semantics** | **absent by design** — the receiving weave's job |
| **authentication** | **absent** — reachability is authority |
| **sandbox containment** | **not applicable** — Bridge does not sandbox anything; OS containment is [capabilities](capabilities.md), and no binary in this tree composes `BridgeServer` with `IsolationHost` |

An operator is the *most-granted ordinary participant*, and that is a real
ceiling: no lifecycle authority, no ungated send, no grant assignment, and
**no Sense read authority at all** — `allow_any()` adds a send rule, and
observing a claim needs its own observe rule, which nothing here adds
([SENSE-05](../laws/sense-laws.md)). It is not host root. It is also not
nothing — treat an operator connection as you would treat a shell on the box.

Threat tier: **abuse, not escape**, the same as the rest of Loom.

## Failure and refusal — what an operator actually sees

Two kinds, kept distinct because they send an operator to different places.

**Per-frame, non-fatal — the connection survives.** A `Send` that dies before
it reaches the bus produces a `SendRefused` frame carrying the correlation and
a reason, which the client surfaces on its tap as a `BridgeRefused` line
(honestly labelled: it is *not* a bus event, because no bus event exists).
Three causes: a malformed `Send` header (correlation 0 — the header did not
parse far enough to yield one), an unknown schema, and a gate refusal carrying
the gate's own first error. **No fate is dark**: a send is either taken by the
bus (and then observable on the tap like any other) or refused aloud.

**Fatal — the connection is torn down.** A frame before `Hello`; a frame or
backlog over cap; a transport error; peer EOF. All of them mark the channel
`done()`, and `reap_dead()` unregisters the proxy before destroying it, so no
further delivery can land on a dead connection. Disconnect is handled as an
**event**, never as a hang — which is why the loop is a `poll`/`WSAPoll`
multiplexer rather than a blocking read.

Refusals *after* the bus are ordinary bus refusals — `CapabilityDenied`,
`NoSuchTarget`, a gate refusal at delivery — and they reach the operator
through the tap like every other participant's, with their structured reason
([MSG-05](../laws/messaging-laws.md#msg-05--refusals-are-structured-and-observable),
[guides/diagnostics](../guides/diagnostics.md)).

## Composing it

`step()` is one non-blocking iteration: accept → drain and dispatch inbound
frames → pump the bus → push a deferred discovery refresh → flush → reap.
`wait_and_step(timeout_ms)` blocks in `poll` over `{listener, connections}`
first; `run(tick_ms)` loops that until `stop()`.

**A host that also runs a perpetual in-process service must call
`set_bounded_dispatch()`.** By default `step()` calls `pump()`, which drains
to empty — and a self-re-arming service (a repeating Timer) never lets the
queue empty, so `step()` never returns to poll its sockets and operators
freeze. With it set, `step()` dispatches the backlog present at entry and
moves on. It takes **no number**, deliberately: the numeric version was
shipped, measured 17× slower by a real consumer, and withdrawn
([MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo),
[known-seams § event-loop composition](known-seams.md#event-loop-composition)).

Registry reads (`list_weaves`, `accepted_schemas`) are deliberately **not**
done from inside the tap observer callback: `on_tap` copies event fields only
and sets a dirty flag, and `step()` pushes the refreshed weave list after
`pump()` returns. The bridge does not lean on an unstated bus property.

## Tests

Suite `bridge` — transport round-trip and EOF-as-an-event (both AF_INET and
AF_UNIX), the forged-wire-sender pin, the connection cap, the pre-`Hello`
severance, hostile-`Send` refusal, malformed framing, the client's bounded
pending/absent caches, a SIGKILLed operator reaped across two real processes,
the bounded-dispatch cases, and the C-1 remote-window cases. `zen-bridge-probe`
is the non-interactive end-to-end crossing proof against a live
`zen-bridge-host`.
