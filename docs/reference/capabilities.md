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

### Fresh access, inherited descriptors, and the exec boundary

Three different facts, and reading any one as the others is the mistake C-2
was. A **capability namespace** decides what a *fresh* attempt can reach. It
decides nothing about a descriptor that was **already open** and crossed
`execve` — a connected socket, an open file, a pipe, a terminal is simply
*there*, at `FsAccess::None`, in a netns with no interface. So the child gets
a third, separate guarantee:

**Exec-boundary descriptor hygiene.** Every spawn of `zen-weave-host` goes
through one fork/`execve` path, and immediately before `execve` the child
closes **every descriptor except an explicit allow-list**. The embedding host's
own descriptors — which Loom never created and cannot annotate — do not cross.

The intentional set is exactly:

```text
0, 1, 2    stdin/stdout/stderr — DELIBERATELY KEPT.
           An intentional ambient capability, not an oversight: the child
           shares the host's console, which is what carries its crash and
           sanitizer output. It is the one ambient reach that survives.
3          the weave-host protocol transport (the socketpair end).
```

Mechanism: `close_range(2)` where the kernel has it (called by syscall number,
so no glibc floor moves), otherwise enumeration to the `RLIMIT_NOFILE` **hard**
limit. `/proc/self/fd` is deliberately unused — the restricted view does not
mount `/proc`. If **neither** can be applied the spawn **refuses**; a child
that kept the host's descriptors is never allowed to run under a containment
claim. Loom's own sockets additionally set `FD_CLOEXEC` at creation, which is
defence in depth and explicitly *not* the boundary.

What this does **not** say: nothing here constrains the child's **environment**
(`execve` passes the host environment through), and the control fd is a real
channel to the host, by design — *contained* has always meant no external
reachability, not no-IPC.

**The honest threat tier: abuse, not escape.** The sandbox stops buggy or
greedy code. seccomp is the named, unbuilt escalation to escape-tier. Any
prose implying "hostile-proof" is wrong; say abuse-tier.

### Work the host does *on behalf of* a contained participant

A cgroup bounds the child's own process. It does not bound work the child asks
the **host** to do, and the host does real work for it: an isolated child
declares its schemas in its manifest, the host registers them, and the host then
parses and admits that child's emissions in the host process, before any grant
is consulted. Anything unbounded on that path is a resource cap the sandbox
exists to impose and does not.

One such path is closed and pinned: **an isolated participant cannot command
unbounded host-side decode materialization through a compact serialized value.**
Every decode of untrusted bytes spends one shared, host-owned allowance of
`kMaxDecodedCells`, spent before the structure exists
([bounds](bounds.md#the-decode-materialization-bound)) — the same central
decoder every seam funnels through (child emissions, manifests, snapshots,
Bridge frames, dynamic-library bytes, persistence loads).

That claim is exactly this wide. It does **not** say all denial of service is
impossible, that all host work is bounded, or that host memory cannot be
exhausted through any API. Specifically still true:

- a large but in-budget value is still allowed to be expensive;
- queue growth, channel buffers, and service-specific state have their own,
  separate bounds — this one is the decoder's;
- application services must still validate their own semantic ranges;
- native in-process code is trusted and is bounded by nothing here.

Delegation is invocation-dependent: enforcement suites run under a delegated
cgroup scope (`tests/run-under-scope.sh`); a plain shell lands in the root
cgroup and the harness **fails hard** rather than skipping green.

## Tests

Suites `capabilities`, `isolation`, `policy` (the enforcement halves execute
under the scope, with executed-count assertions so a green can never mean
"silently skipped").
