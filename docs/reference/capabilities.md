# Capabilities — reference

Authority: who may say what, to whom — and what the OS is asked to contain.
Laws: [GATE-03](../laws/admission-laws.md),
[MSG-02](../laws/messaging-laws.md).

## The grant (in-process)

Every weave carries a `Grant`, default **empty** (minimal authority). Send
rules select shape→target (`allow_to_any`, `allow_any_to`, `allow_to_role`,
`allow_any`); the bus authorizes each weave-originated delivery against its
sender's grant **at delivery**, before role resolution and before the gate
(`CapabilityDenied`). The host is the only source of grants — there is no
in-band widening.

**The trust boundary is which bus you hold.** A weave holds only a `WeaveBus`
(`Mail`): it stamps the weave's own identity on everything and routes through
the gated path. The concrete `Switchboard` — `send`/`publish` ungated, grants
assigned, lifecycle authority mintable — is host root authority. Holding a
`Switchboard&` *is* being a host for that Loom, and only that Loom
([LIFE-04](../laws/lifecycle-laws.md)).

Three tiers that never imply each other: public **shape** (anyone may
represent it) · exact **grant** (permitted to emit it) · **authority**
(lifecycle/answer standing, never grantable through the message system).

## Where a grant comes from (the powerbox)

The floor: unknown mods mount with the default grant plus a send-rule to the
storage **role**. Anything above the floor is a persisted, content-hash-keyed
grant record only the host writes. Brokers prove the pattern: an
out-of-process **StorageBroker** (role `"storage"`) scopes each mod's keyspace
by the stamped sender; a **NetworkBroker** (role `"net"`) does allow-listed
TCP for weaves that themselves hold no OS network capability. The ask
(`zen.CapabilityAsk`, `ZEN_ASK`) is advice, never authority.

## OS containment (out-of-process, Linux/WSL)

A weave may instead run in a child process (`zen-weave-host`),
indistinguishable on the bus, its output re-admitted through the one gate with
the sender stamped from the connection. The sandbox imposes, per grant flag,
and **positively re-confirms** from the kernel's own view:

- **Network** — user+net namespace; a real `connect()` gets `ENETUNREACH`;
- **Filesystem** — `FsAccess` graduated levels (None → ReadOnly → WriteScoped
  → WriteNoExec → WriteAnywhere) via a private mount-namespace allow-list view
  (secrets are *absent*, not hidden);
- **Resources** — per-weave cgroup-v2 leaf; conservative computed defaults;
  a fork bomb is bounded by `pids.max` even under `with_unlimited_memory()`
  (**no grant can license a fork bomb** — structural).

`containment()` reports only what was actually imposed *and confirmed*, and
the runtime **fails safe** when it cannot confirm (dev-mode converts a
known-gap refusal into a visibly-uncontained warning — never a false claim).

**The honest threat tier: abuse, not escape.** The sandbox stops buggy or
greedy code. seccomp is the named, unbuilt escalation to escape-tier. Any
prose implying "hostile-proof" is wrong; say abuse-tier.

Delegation is invocation-dependent: enforcement suites run under a delegated
cgroup scope (`tests/run-under-scope.sh`); a plain shell lands in the root
cgroup and the harness **fails hard** rather than skipping green.

## Tests

Suites `capabilities`, `isolation`, `policy` (the enforcement halves execute
under the scope, with executed-count assertions so a green can never mean
"silently skipped").
