# Capabilities — reference

Authority: who may say what, to whom — and what the OS is asked to contain.
Laws: [GATE-03](../laws/admission-laws.md),
[GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides),
[MSG-02](../laws/messaging-laws.md).

## The grant (in-process)

Every weave carries a `Grant`, default **empty** (minimal authority). Send
rules select shape→target (`allow_to_any`, `allow_any_to`, `allow_to_role`,
`allow_any`); the bus authorizes each weave-originated delivery against its
sender's **effective** authority **at delivery**, before role resolution and
before the gate (`CapabilityDenied`). The host is the only origin of authority,
and a weave still cannot widen itself — but a host may now appoint an
administrator, so the sentence has three parts rather than one:

```text
BASELINE   attached by the host at admission (register_weave / Kernel::load /
           IsolationHost::mount) and never changed thereafter
DELEGATED  replaceable at any time by a holder of a host-minted GrantAuthority,
           on the ONE subject that capability names, within its ceiling
EFFECTIVE  baseline union delegated — what the bus checks, at delivery
```

`Grant` therefore has two halves, and the type says which is which: a
`LiveAuthority` (send rules + observe rules), plus containment policy. Only the
first is delegable, because only it is read at the moment of use.

## Live delegation (GRANT-0)

`host_grant_authority(bus, subject, ceiling)` — the one expression that mints
one, in [`zen/host/grant_wiring.hpp`](../../include/zen/host/grant_wiring.hpp),
needing a `Switchboard&` exactly as lifecycle minting does. The capability
carries **which board** issued it (weakly — it expires with that Loom), **which
subject** it governs, and the **ceiling** it may never exceed. The holder is an
ordinary weave: from inside an ordinary handler it calls
`mail.delegate_authority(cap, requested)` and `mail.describe_authority(cap)`.

| | |
|---|---|
| what it can change | the subject's delegated send rules and observe rules — atomically, as one replacement (grant, revoke, widen, narrow are all this) |
| what it can never change | the baseline (a union cannot subtract), any other subject (the subject is *in* the capability, so there is no argument to point elsewhere), and OS/filesystem/resource reach (`LiveAuthority` has no words for them) |
| what it can read | that one subject's baseline, delegated and effective authority — through the same predicates the bus applies, so an administrator needs no second map that merely believes what the Kernel enforces |
| how it fails | `NoAuthority` (inert), `ForeignBoard` (another Loom's, or a dead one), `NoSuchSubject`, `ExceedsCeiling` — and on every one of them, nothing changed |

**The ceiling is not the holder's own grant.** "What a Weaver may say" and "what
a Weaver may hand out" are different questions, and only the host answers the
second. Containment is semantic: `any` contains each exact selector; a rule
naming an **office** and a rule naming a **WeaveId** contain one another in
neither direction, whoever holds that office right now — otherwise today's
routing would become permanent authority.

**Revocation is effective at delivery**, including for a message queued while
the authority was still held: nothing on an envelope remembers what was true
when it was authored. And the administrator never becomes the sender — no
message is queued at all, so the governed subject retries its own action and
the target sees the subject.

This is **message authority only**. It is not "grants are now mutable": an
isolated child's namespace, mount view and cgroup leaf were built before it
ran, and no write in this process moves them.

**The first holder of one is the [Weaver](weaver.md)** (WEAVER-1) — an ordinary
weave that puts an authority request in front of a human being and installs the
answer. Keep the four apart: the **Kernel** enforces, a **`GrantAuthority`** is
the administration mechanism, the **Weaver** is one policy delegate, and the
**operator seat** is a WeaveId a host chose to treat as the user. None of them
implies the next, and the current console is a *bootstrap* operator rather than
a permanent user identity.

**The trust boundary is which bus you hold.** A weave holds only a `WeaveBus`
(`Mail`): it stamps the weave's own identity on everything and routes through
the gated path. The concrete `Switchboard` — `send`/`publish` ungated, grants
assigned, lifecycle authority mintable — is host root authority. Holding a
`Switchboard&` *is* being a host for that Loom, and only that Loom
([LIFE-04](../laws/lifecycle-laws.md)).

Three tiers that never imply each other: public **shape** (anyone may
represent it) · exact **grant** (permitted to emit it) · **authority**
(lifecycle/answer standing, never grantable through the message system).

**The grant bounds speech, not memory.** Everything above is *bus* authority,
and in-process it is the only authority Zen mediates. A native weave —
compiled in, or `dlopen`ed by [the kernel](kernel.md) — executes in the host
process's own address space and can read and write host memory directly,
without sending anything. Admission, grants and role routing do not create
memory isolation and cannot: **an in-process weave is trusted at the
process-memory level**, which `Kernel::containment_note()` states in one line
(*"in-process; trusted; no OS sandbox"*). Projecting a capability onto a
boundary the OS enforces is what the next sections are for, and that is a
different mechanism with a different threat model — see
[guides/dynamic-weaves](../guides/dynamic-weaves.md#what-loading-it-in-process-means).

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

- **Network** — user+net namespace; a real *fresh* `connect()` gets
  `ENETUNREACH`. What that does and does not cover is
  [three separate facts](#the-exec-boundary-three-independent-facts);
- **Filesystem** — `FsAccess` graduated levels (None → ReadOnly → WriteScoped
  → WriteNoExec → WriteAnywhere) via a private mount-namespace allow-list
  view. The view is built by *addition*, so what it contains is enumerable —
  [the inventory is below](#the-filesystem-view-what-is-in-it), and it is
  written out rather than summarised because a slogan cannot be checked;
- **Resources** — per-weave cgroup-v2 leaf; conservative computed defaults.
  `with_unlimited_memory()` removes the memory cap **alone** — no grant
  removes `pids.max`, so **no grant can license a fork bomb** (structural).
  The fork-bomb stop is `pids.max`, which the host can only impose **where
  the pids controller is actually delegated to it**; where it is not, the
  attestation says `FORK-BOMB STOP NOT ENFORCEABLE` rather than claim a cap
  nothing wrote ([below](#delegation-is-what-makes-a-resource-cap-real)).

`containment()` reports only what was actually imposed *and confirmed*, and
the runtime **fails safe** when it cannot confirm (dev-mode converts a
known-gap refusal into a visibly-uncontained warning — never a false claim).

### The filesystem view: what is in it

`build_view_plan` (`src/isolation/host.cpp`) constructs the child's whole
world by **addition**: a fresh `tmpfs` root, a fixed set of read-only binds, an
optional writable submount, then `pivot_root` into it and detach the old root.
Nothing carries over implicitly, so the view is exactly this list — which is
why it is a list rather than a sentence about secrets.

**Present at every level, including `FsAccess::None`:**

| In the view | How | Why it is there |
|---|---|---|
| `/usr`, `/lib`, `/lib64`, `/bin`, `/etc` | recursive bind, **read-only** (each only if it exists on the host as a directory) | the dynamic loader's closure — `zen-weave-host` and the weave `.so` cannot start without it |
| the directory holding `zen-weave-host` | recursive bind, read-only | so `execve(exe)` resolves *inside* the view |
| the directory holding the weave's `.so` | recursive bind, read-only (skipped when it is the exe dir, or under it) | so `dlopen` resolves inside the view |
| `/` itself | the `tmpfs` root, remounted **read-only** after its mountpoints exist | otherwise the read-only/noexec intent would leak through a writable root |

**Added by level:**

| Level | Extra | Writable? |
|---|---|---|
| `None` | — | nothing is writable |
| `ReadOnly` + a granted path | that host tree, recursive bind | no |
| `WriteScoped` + a granted path (today only the StorageBroker's grant carries one; a mod is `None` and never gets a path) | that host directory bound at `/scratch` | **yes**, and **persistent** (real host storage) |
| `WriteScoped` with no path | a fresh `tmpfs` at `/scratch` | yes, ephemeral |
| `WriteNoExec` | a fresh `tmpfs` at `/scratch`, `MS_NOEXEC` | yes, ephemeral, no native `execve` from it |
| `WriteAnywhere` | **no view is built at all** — this level resolves to *granted*, not *contained*: the unrestricted host filesystem, by the grant | yes, everywhere |

**What `FsAccess::None` removes.** Everything not in the first table — the
host's home directories, `/root`, `/var`, `/tmp`, `/opt`, `/srv`, `/mnt`,
`/media`, `/sys`, and any host path outside a bound tree — and there is no
writable location anywhere in the view. Two removals worth naming because
software assumes them:

- **`/proc` is deliberately not mounted.** PIDs are not namespaced, so a
  `/proc` in this view would be the *host's* process table. Its absence
  removes a whole class of reach — and is also why the exec-boundary sweep
  below cannot use `/proc/self/fd`;
- **`/dev` is not mounted either** — no `/dev/null`, no `/dev/urandom`, no
  `/dev/tty`. A weave must not assume a device node exists. This is a
  *filesystem* absence and nothing more: `getrandom(2)` is a syscall and is
  unaffected, since no seccomp filter is applied (see the threat tier below).

The old root is unmounted (`MNT_DETACH`) rather than merely left
unreferenced, so it cannot be walked back into.

**What `FsAccess::None` does *not* remove**, stated exactly because this is
the half a summary loses:

- **`/etc` is present and readable at every level.** Whatever the host keeps
  there is in the view — `passwd`, `group`, `hostname`, `resolv.conf`, the CA
  bundle, and any application configuration or credential a deployment has put
  under `/etc`. Nothing filters it; the whole tree is bound;
- `/usr`, `/lib`, `/lib64`, `/bin` likewise — the full system software set,
  read-only, including whatever a deployment installed there;
- **the deployment directory.** The exe dir and the `.so` dir are bound whole,
  so a weave can read its *siblings*: other weave artifacts, and any data file
  a host placed beside them.

So the honest sentence is: **the view contains the system software set, the
system configuration tree, and the deployment directory — read-only — and
nothing else unless a grant added it.** A secret is absent here only if it is
not in `/etc` and not beside the artifact. That is a real and useful boundary;
it is not "secrets are absent" as an unconditional claim, and this reference
used to say the latter.

Writable submounts are noexec **only** at `WriteNoExec`, and even there the
block is native `execve` — not code an interpreter already inside the view
chooses to run.

### Delegation is what makes a resource cap real

Every cgroup dimension is written **only where its controller is delegated to
this host**, and each is reported per dimension:

| Dimension | Where delegated | Where not delegated |
|---|---|---|
| memory | `memory<=NMiB` — or `memory unlimited-by-grant` where `with_unlimited_memory()` opted out, which is a grant fact and not a delegation one | `memory UNCAPPED (no memory controller delegated — not enforceable on this host)` |
| pids (the fork-bomb stop) | `pids<=N`, and the honest scope reads *"pids.max bounds a fork-bomb (no grant licenses one) where the pids controller is delegated"* | the **headline** reads `resources: memory contained but FORK-BOMB STOP NOT ENFORCEABLE (no pids controller delegated — pids.max unset)`, and the scope clause says a fork bomb is bounded only by the host-wide pid limit, not per-weave |

The headline carries it rather than a footnote: an absent fork-bomb stop is at
least as load-bearing as a not-contained network cap, so it is surfaced as
prominently. The confirmation clause is qualified the same way — where pids is
not delegated there is no `pids.max` to read back, and the string says that
instead of implying a readback that never happened.

**So the guarantee is conditional, and the condition is nameable.** Where the
pids controller is delegated and imposed, a fork bomb is bounded by `pids.max`
even under `with_unlimited_memory()`, and no grant lifts it. Where it is not,
the runtime does not claim it: `resource_note` / `resource_attestation` are
pure functions with unit tests for **every** delegation posture, so each
posture's exact wording is pinned rather than assumed. The lattice is the
same everywhere — *detect · impose · positively reconfirm · report only what
was imposed* — and a host that cannot enforce at all refuses the mount rather
than downgrading it (dev-mode converts that refusal into a visible
uncontained warning, never a false claim).

### The exec boundary: three independent facts

Reading any one of these as the others is exactly the mistake C-2 was. A
**capability namespace** decides what a *fresh* attempt can reach — and nothing
else. It says nothing about a descriptor that was **already open** and crossed
`execve`, nor about what the child was **told**. Each has its own boundary:

| fact | what decides it |
|---|---|
| namespace / resource containment | the netns, mountns and cgroup leaf, positively re-confirmed |
| **descriptor** inheritance | the exec-boundary sweep — an explicit allow-list |
| **environment** inheritance | the authored child environment — explicit entries only |

**Descriptor hygiene.** Every spawn of `zen-weave-host` goes through one
fork/`execve` path, and immediately before `execve` the child closes **every
descriptor except an explicit allow-list**. The embedding host's own
descriptors — which Loom never created and cannot annotate — do not cross.

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

**The environment is authored, not inherited.** `execve` used to receive
`environ`, so a weave at `FsAccess::None` with no network was still handed the
host's `HOME` and `PATH`, the addresses of the session bus, the compositor and
the audio server, whatever tokens the embedding process held — and any `LD_*`,
which the loader acts on *before* any Zen code in the child runs. None of that
was a capability Zen granted; it crossed because the host possessed it.

The child's environment is now built entry by entry, and the current authored
set is:

```text
<empty>
```

That is a measurement, not a preference. On the canonical toolchain, in all
three lanes, `zen-weave-host` reaches `main()`, completes loader startup,
`dlopen`s a real weave, checks the ABI and constructs the instance under
`env -i`; the binary carries no `RPATH`/`RUNPATH`, and every library it needs
(including `libasan`/`libubsan` in the sanitizer lane) resolves from the system
paths the view already binds. Sanitizer behaviour is compiled in
(`-fsanitize=… -fno-sanitize-recover=all`); this tree reads no `*SAN_OPTIONS`
anywhere, so none is forwarded and none is needed.

Default deny is **structural**: the builder starts empty and the only way in is
an explicit entry, so a variable a future host introduces is absent without
anyone maintaining a list. There is no blacklist — a blacklist of
secret-looking names leaves every unlisted variable's authority intact. If the
authored environment cannot be constructed the spawn **refuses**; it never
falls back to `environ`, and there is no option that disables this.

Two consequences worth stating plainly: a host-set `LD_PRELOAD` (or any `LD_*`)
does **not** reach the sandbox, and a host-set `ASAN_OPTIONS` no longer
configures the child's sanitizer runtime.

What this still does **not** say: the control fd is a real channel to the host,
by design — *contained* has always meant no external reachability, not no-IPC.

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

### The granted-network positive control, and the endpoint it uses

The network case proves containment in two halves, and the second one is the
control that makes the first mean anything. Withholding `Network` must be *why*
the child cannot reach the network — not the machine having no usable network, not
a dead endpoint, not a broken probe, not a child that is muzzled whatever the grant
says. So an otherwise-identical child **with** `os_cap::Network` runs the same probe
against the same endpoint and must succeed.

**The endpoint belongs to the test.** Before either child is mounted, the case binds
a listener on `127.0.0.1:0` — the kernel picks the port, so nothing is reserved,
occupied, firewalled or guessed — and hands that port to the probe in the Ping's
`seq`. Both halves aim at it, so the only meaningful difference between them is the
grant:

```text
WITHOUT os_cap::Network   containment reports contained
                          connect() -> ENETUNREACH, before a SYN is ever sent
                          (its netns has no interface, so the verdict does not
                           depend on what is listening on the far side)

WITH    os_cap::Network   containment reports granted
                          connect() succeeds, one token byte goes down it,
                          and the test's own listener accepts and reads that byte
```

The positive witness is therefore **success**, not a particular kind of failure, and
both ends are asserted: the child reports that it wrote, and the listener reports
that it arrived. A handshake that cannot carry data is not counted as reach.

The granted child can reach a listener held by the test process because `Network`
granted imposes no network namespace at all — the child is in the test's own netns,
so that `127.0.0.1` is the same `127.0.0.1`. It does not get there by inheritance:
the exec boundary closes every descriptor but the control fd and the standard three
([three separate facts](#the-exec-boundary-three-independent-facts)), so a child
that reaches the endpoint reached it over the network.

#### Why it is written this way — a retired environment exception (BL-VER-07 → BL-VER-08)

**This is history, not an operator instruction.** It is kept because the shape of
the old mistake is the reason for the current design, and because a reader who finds
BL-VER-07 cited elsewhere should be able to learn that it no longer applies.

The positive control used to connect to the literal **port 1** and require
`ECONNREFUSED`, on the reasoning that "nothing listens there, so a reachable stack
refuses". That inferred an open door from the noise made by knocking on somebody
else's closed one — and it made the result depend on how the *host* treats a closed
port. On a WSL2 host in mirrored networking mode it does not treat it at all: the
SYN is black-holed, no RST comes back, and `connect()` burns the kernel's whole
retry budget — measured at **123.4 / 124.1 / 124.4 s** over three runs with
`tcp_syn_retries = 6`, ending in `ETIMEDOUT (110)` and never `ECONNREFUSED (111)`.
Against the case's 2000 ms budget, nothing had arrived. A twenty-line C program with
no Loom anywhere near it behaved identically, which is what settled *environment,
not defect* — and it is why the repair was to the test's assumption and not to the
isolation implementation, which BL-VER-08 left untouched.

BL-VER-08 also measured the black hole more precisely than BL-VER-07 recorded it:
on that host it is **not** closed ports in general. Closed ports at 1, 7, 80, 443,
1023, 1024, 2000, 9999, 30000 and 60000 all black-holed, while a closed port inside
the local ephemeral range (`ip_local_port_range = 44620 48715`) answered
`ECONNREFUSED` in 0.4 ms. The mirroring intercepts the ports the Windows side might
be serving and leaves the Linux ephemeral range alone.

None of that can affect the current test, which no longer asks any closed port
anything. On the exact WSL2 mirrored-networking host that produced the ~124 s stall,
the case now runs in **0.09–0.13 s** and the official lane passes.

```text
the former mirrored-WSL exception    RETIRED — it does not apply to current source
what replaced it                     an endpoint the test establishes and owns
ECONNREFUSED in the positive oracle  GONE
```

If the network case fails now, **BL-VER-07 does not explain it.** Investigate it as
new. In particular the two halves fail for different reasons and the difference is
the diagnosis: a contained child answering anything but `ENETUNREACH` means the
netns, not the endpoint; a granted child failing to connect at all means the grant
or the endpoint, and the endpoint is inside the test.

PROVEN BY — `tests/test_isolation.cpp` (the case
`"network is OS-enforced: a child without the Network grant cannot reach the
network"`, identified by that sentence and never by a line number) and
`tests/enforcement_gate.hpp` (the executed-count assertions). Evidence trail:
`Zen/reportbacks/TERM-0-RB.md` §66 and `Zen/reportbacks/BL-VER-07-RB.md` (the
measurement and the exception it recorded), `Zen/reportbacks/BL-VER-08-RB.md` (the
replacement witness and the retirement).
