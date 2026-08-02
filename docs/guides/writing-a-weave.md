# Writing a weave

One task: a participant that hears one shape, keeps state, and speaks another.
The whole program below compiles today — it is
[`examples/heartbeat_woven.cpp`](../../examples/heartbeat_woven.cpp), built in
the normal lane.

## Shapes are structs

```cpp
#include <zen/weave.hpp>
#include <zen/switchboard.hpp>

struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq));   // name, version, fields — the schema is derived
};
struct Pong {
    std::int64_t seq;
    ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq));
};
struct Count {
    std::int64_t handled;
    ZEN_SHAPE(Count, 1, ZEN_FIELD(handled));
};
```

A published `(name, version)` is frozen forever — to evolve a shape you
publish a new version ([GATE-04](../laws/admission-laws.md)).

## A weave is a class with typed doors

```cpp
class Responder : public loom::WeaveBase<Responder, Count,
                                         loom::Accept<Ping>, loom::Emit<Pong>> {
public:
    void on(const Ping& p, loom::Mail& mail) {
        ++state_.handled;               // Count is your state; snapshot/revive derive
        mail.reply(Pong{p.seq});        // speak — through Mail, as yourself, gated
    }
};
```

`Accept<...>` is what you hear (your accept-set — anything else refuses before
it reaches you). `Emit<...>` is what you declare you say. `state_` is your
declared state struct; the bus can snapshot, kill and revive you from it
without another line of your code.

## Mount it

```cpp
loom::Switchboard bus;                       // the host owns this
loom::WeaveId responder = loom::mount<Responder>(bus);
bus.send(responder, loom::Message(loom::to_value(Ping{7})));
bus.pump();                                  // nothing runs until the host pumps
```

`mount<>` registers the weave with a default grant matched to its `Emit`
declarations; a host that wants tighter reach passes its own `Grant`
(`mount_granted`). Speak only through the `Mail` you are handed — it stamps
your identity and authorizes every send against your grant.

## What happens, and the common failures

Delivery runs your `on(...)` only after the message passed authorization and
the gate. When something doesn't arrive, the refusal is a named, observable
event — the usual first three:

- `NotAccepted` — the target never declared that shape. Add it to `Accept`.
- `CapabilityDenied` — *your* grant doesn't permit that shape to that target.
  The host decides grants; ask for reach, don't grab it.
- `GateRefused` — the shape didn't conform (which field, expected/actual are
  in the refusal).

See [diagnostics](diagnostics.md) for reading the tap and the journal.

## Deeper

[Messaging](messaging.md) (answers, roles, publish) ·
[dynamic weaves](dynamic-weaves.md) (the same class, loaded from a library) ·
reference: [messaging](../reference/messaging.md),
[values-and-admission](../reference/values-and-admission.md).
