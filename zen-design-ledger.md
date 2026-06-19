# Zen design ledger — built, designed, and open

**Purpose.** This ledger exists to keep a clean line between what is *implemented* and
what is only *designed*, so work can continue without confusion and so neither the
author nor any assistant later mistakes architecture-we-permit for code-that-exists.
Everything under "Built" ships and is verified. The pillars in §2 are the **design
record**: some have since been built in phases (B1, B2) — their **Status** lines mark
exactly what — and the rest is design the current codebase *allows*, not a line of it
written. When in doubt, assume a thing is not built unless a **Status: built** line (or
the Built section) says so.

---

## 1. Built (ships, green under `-Werror` + ASan/UBSan, GCC 11.4 / WSL)

| Layer | What it is | Status |
|---|---|---|
| `zen-core` | self-describing value + the one gate. Schema (7 frozen kinds, FNV content-id), Value (positional), `admit` (the sole validator), Registry (immutable `(name,version)`), serialization (native canonical binary + compat JSON), `Unverified` (untrusted-until-proven). | built |
| `zen-switchboard` | in-process bus, first live boundary. Gated delivery (admit at delivery vs the recipient's accept-schema), single-threaded FIFO `pump` with reentrancy guard, observer tap, `Shard` interface, lifecycle (`snapshot`/`kill`/`reload` + `swap_state`), abstract `Bus`. | built |
| `zen-kernel` | DLL loading across a true C ABI. Versioned descriptor, `ZenByteSink` ownership (no cross-allocator free, no host pointer into library memory), host adapter (a loaded Shard *is* a `Shard`), bytes-as-currency re-admitted through the gate, validate-then-commit hot-reload, safe teardown, manifest-as-gated-Value (the minimal schema-as-value precursor). | built |
| `zen-author` (header-only) | low-ceremony authoring. `ZEN_SHAPE` (schema-from-struct, Kind deduced from C++ type, version required), `ShardBase` (typed handlers, derived accept-set / snapshot / revive, `Mail` as the sole outbound path), `mount<>()`. | built |
| Level 0 hardening | dispatch selector → `(name,version)` (null-deref fix), loud no-match, `swap_state` split from the crash-revival budget, emit-honesty by test with `Mail` reserved as the enforcement chokepoint, `content_id`-site grep sweep, seam-readiness review. `same_identity` strengthened to true `(name,version,content_id)` identity. | built |
| Capabilities **(B1)** — `grant.hpp` + switchboard + kernel door | the in-process grant model. Per-Shard `Grant` (send-rule selectors over shape→target, plus reserved OS-capability flags); capability-gated delivery (the `ShardBus` a handler receives stamps its identity and authorizes against its grant *before* the gate → `CapabilityDenied`); `Switchboard::send/publish` are the ungated host root, the gated `ShardBus` is all a Shard ever holds; public `send_as`/`publish_as` (host re-enters a Shard's output with the sender stamped from the connection); the kernel's `LoadLibrary` door is itself a gated capability, demonstrated against native Shards. | built |
| Isolation **(B2)** — `zen-isolation` library + `zen-shard-host` child | out-of-process hosting + crash supervision. A Shard runs in a child process, indistinguishable to the bus (a proxy that *is* a `Shard`); framed, bounded, defensive unix-socket IPC (per-frame + backlog caps, EOF = death, never blocks the host); the child reuses the kernel C ABI and links no zen-core (a byte shuttler); child output is re-admitted through the one gate host-side with the sender stamped from the connection; a single-threaded `step()` (drain IPC → `pump` → supervise) keeps the bus's FIFO/reentrancy intact; on crash, bounded reload from the host-owned snapshot then quarantine. **Isolated, not sandboxed** — the grant's OS-capability flags stay inert (that is B3). | built |

The spine holds across every boundary: one gate everywhere, untrusted-until-proven,
immutable published schemas, the kernel holds grammar not answers. The content-id
fast-path is deliberately *untaken* so "one gate, every delivery" stays literally true.
The grant is now projected onto two **real** boundaries — the message boundary (B1)
and the process boundary (B2); the syscall boundary (B3) is the remaining projection.

---

## 2. The three pillars (design record — partly built)

This section is the **design record** for the three pillars. Where a pillar (or a phase of
one) has since been built, its **Status** line says so and §1 carries the shipped summary;
the prose here is the settled design it was built from. Pillar 1 is **built (B1)**; Pillar 2
is **built as B2** with the sandbox (B3) still to come; Pillar 3 is **still wholly design**.
Anything marked *designed, not built* is design the codebase allows — not a line of it written.

### 2.1 The console (the first human-facing Shard)

A Shard authored entirely in the low-ceremony layer plus a thin frontend; it mostly
*spends* what the substrate already banked.

- **Discovery-first.** Browse Shards → view a Shard's accepted message shapes → fill
  fields → send. Discovery is not a beginner's crutch; it is the single source the
  whole interaction derives from (`list_shards` → `accepted_schemas` → walk the shape →
  `send`). Knowing the command path *is* knowing the system, because the path names the
  Shard it talks to and the shape it sends.
- **Speed-runnable guided, one path two speeds.** There is one canonical command path
  (shard → message → fields). Guided = walk it slowly with the bus answering each step;
  speed-run = supply the whole path up front; partial = the engine asks only for the
  gaps. Not two modes — one path, and your speed along it is how much of it you already
  hold in your head.
- **Engine / frontend boundary (the load-bearing seam).** The *engine* turns a **partial
  command path** into either *the next prompt* (incomplete) or *a gated message*
  (complete). It is taste-empty. The *frontend* feeds input and renders the result.
  Every future frontend — GUI autocomplete, saved aliases, an AI English-to-path layer —
  is the same engine fed differently; a GUI's "show valid completions as I type" is
  exactly what the engine already computes to ask its next question.
- **Terminal-first, flipbook rendering.** v1 is a terminal using a live-redrawn prompt
  region (raw mode, read every char, repaint the current line + a derived panel below it
  while committed history scrolls up — the readline technique). The shard column shows a
  red "no such shard" the instant you typo, because the engine resolves the partial path
  against the live registry each keystroke. GUI (SDL3 / ImGui) is a *later, separate*
  phase and merely *another frontend* on the same engine.
- **Observer + injector, not reply-receiver.** The console watches the tap (delivered
  payloads and every refusal, legible because routing reasons are separated from gate
  reasons) and injects via `send`/`publish`. It deliberately does **not** receive typed
  replies — a console accepting arbitrary shapes is the first thing that wants a hole in
  the silhouette, which is a capability question, deferred. You inject and watch the
  consequences ripple on the tap.

### 2.2 Pillar 1 — Capabilities / grants (the silhouette, made enforceable)

The kernel grants **capabilities, not permissions**, and the default grant is nearly
empty. A Shard's reach into the world is its silhouette: which message shapes it may
send, to which targets, plus the dangerous OS-relevant grants (ask the kernel to load
more code, touch the filesystem, reach the network).

- The bitcoin-miner reframe: arbitrary code is not the danger — *ambient authority* is.
  Capabilities make most bad behavior **unsayable** (a Shard with no network grant cannot
  reach the network over the bus; the only things it can *do* are send granted messages).
- This answers "should the kernel accept messages": **yes** — the kernel exposes a
  control surface as a Shard-like participant (`LoadLibrary` / `ReloadLibrary` /
  `UnloadLibrary`), reachable and discoverable like anything else, so operating the system
  is the same gesture as using it. But that surface is the single most dangerous
  capability there is, so it **cannot be ambient** — the right to send the kernel a
  `LoadLibrary` is a *grant*, held by a few (the console, a supervisor).
- **What this requires (and the bus lacks today):** delivery is currently gated on
  *shape* only. Capability-gating adds the second question at delivery — not just "is this
  a well-formed X" but "may *you* send an X to *them*."
- **Honest limit:** this governs the **message** boundary, not the **instruction**
  boundary. A loaded native `.so` can make syscalls directly regardless of its bus grant.
  That gap is closed only by Pillar 2 (process isolation in B2, the OS sandbox in B3).

**Status: built as B1** (in-process). The grant, capability-gated delivery
(`CapabilityDenied` before the gate), the trusted-`ShardBus`-vs-root-`Switchboard`
split, and the kernel's gated load-door all ship and are tested. What is *not* yet
enforced: the grant's **OS-capability flags** — they are recorded on the grant and
shaped for the sandbox, but inert until B3 makes them absolute at the syscall boundary.
So B1 makes the grant real at the message boundary exactly as this design intended,
with its OS-relevant parts deferred — not weakened — to the isolation phases.

### 2.3 Pillar 2 — Isolation, then the sandbox (two phases: **B2 built**, **B3 designed**)

> **Refinement settled this stretch.** The original framing (the old heading: "crash-isolation
> and capability-enforcement are *one phase*") folded both into a single out-of-process move.
> Building it clarified they are **two phases**, and splitting them is the honest move: process
> isolation ships real containment *now* (B2), and the OS sandbox that makes "no network"
> absolute is separable, additive work (B3). Conflating them would have forced B2 either to
> over-claim (call a process boundary a sandbox) or to wait on the fiddly syscall work before
> delivering any containment at all.

The instruction-layer gap (a Shard with an empty grant can statically link or `dlopen`
a networking library and call `connect()` directly, outside the kernel's knowledge) has a
**two-step** answer.

**Built as B2 — process isolation (containment, not a sandbox).** Out-of-process, the OS
boundary means a child **cannot touch host memory** and a crash **cannot take the host
down**: the host detects the death, contains it, reloads from a host-owned snapshot bounded
by `max_reloads`, then quarantines. Hosting is a per-Shard mount choice; the bus cannot tell
a hosted Shard from a native one; the grant is still enforced at the **message** boundary
(child output is gated host-side, sender stamped from the connection). What B2 deliberately
does **not** do is enforce the grant's OS-capability flags — it reports its containment
honestly as *"isolated, not sandboxed,"* and the flags stay inert. That honesty is
load-bearing: a process boundary stops a *crash*, not a `connect()`.

**B3 — the OS sandbox (designed, not built).** B3 projects the grant's OS-capability flags
onto a real syscall-level profile applied to the child *before* it loads the `.so`, turning
"isolated" into "isolated **and** sandboxed." The seam is already in place (hosting is
out-of-process, the grant carries the flags, the child is the one place a profile installs),
so B3 is additive — no rework to the gate, the bus, the wire format, or B2's supervisor:

- Out-of-process, the OS gives instruction-level enforcement the bus can't: a **network
  namespace with no interface** makes `connect()` fail *regardless of what's linked or
  loaded*; seccomp-bpf filters the syscall set; cgroups cap CPU/memory; dropped capabilities
  close privileged ops.
- **The unification:** the grant is one source of truth projected onto whatever
  boundaries the hosting mode provides. In-process it's bus-enforced (partial, advisory
  below the bus). Out-of-process it's *also* syscall-enforced (absolute). So
  crash-isolation (survive a Shard segfault) and capability-*enforcement* (make "no
  network" absolute) are the **same out-of-process move viewed twice**.
- **Mode maps onto trust, and the grant decides the mode.** A first-party Shard you
  compiled → in-process (fast, bus-enforced, vetting is the tool). An untrusted
  mod-authored Shard → out-of-process under an OS sandbox scoped to its grant (slower,
  OS-enforced; the linked-libcurl trick fails because the process has no network).
  Process isolation makes the transitive-vetting problem **moot** for OS-relevant
  capabilities — the sandbox contains the whole process whatever it loads.
- A **full-trust, not-sandboxed, in-process** mode is explicitly wanted: trusted
  first-party native-speed code, bus-enforced only. It is one end of this spectrum — and
  it ships (B1: in-process, the grant enforced at the message boundary). The spectrum now
  has **two of its three points built**: in-process/bus-enforced (B1) and
  out-of-process/isolated-but-not-sandboxed (B2); only out-of-process/OS-sandboxed (B3)
  remains. B2 made the middle point — isolate for *crash* containment without paying for a
  full sandbox — a real, distinct choice rather than an all-or-nothing jump.
- **Honest limits:** sandbox config is real work (seccomp is fiddly; namespaces are
  robust because coarse). Not every capability has an OS shadow (bus-only grants like "may
  send DamageEvent" stay bus-enforced). Out-of-process costs IPC + serialize-at-the-boundary
  — which is *why* bytes-as-currency was built, and why isolation is per-Shard, not the
  default. And the grant *decision* is irreducible trust: a granted network capability is
  real power. The honest sentence is *"Zen makes a Shard's power minimal-by-default,
  explicit, observable, and — out-of-process — OS-enforced; the trust in a granted
  capability is yours to give, and giving it is real."* Not a sandbox in-process; not
  "arbitrary code made harmless" even out-of-process.

**Status: B2 built, B3 designed.** B2 (out-of-process hosting + crash supervision +
honest "isolated, not sandboxed" status) ships and is tested in Debug and under ASan/UBSan.
B3 (the OS sandbox that enforces the grant's OS-capability flags) is designed, not built —
additive on the seam B2 leaves in place.

### 2.4 Pillar 3 — Gated continuous access (the safe shape of shared state)

Driving case: a health display wants current/max HP continuously accurate without
polling the bus every frame.

- **First, the poll model is the problem, not bus speed.** HP changes on damage, not per
  frame — so **push, not pull**: combat publishes `HPChanged{current,max}` when it
  changes, the display caches it and renders the cache at native speed, and most frames
  carry zero bus traffic. Flipping pull → push makes most of the per-frame cost evaporate.
- **For genuinely high-frequency cross-Shard reads** (a transform a dozen systems read
  every frame): a Shard publishes a value into a **typed, read-only slot the runtime
  mediates**, and readers hold a **handle, not a pointer**. The handle yields the current
  value with no per-read message (a deref, not a round-trip); it is schema'd (no
  reinterpreting bytes); and — the property a raw pointer can never have — its lifetime is
  mediated, so when the owner dies or hot-reloads the handle goes **safely invalid** (reads
  return "unavailable") instead of dangling. *(Which component owns the slot — kernel vs
  switchboard — is an unsettled detail; what matters is mediation by something that tracks
  lifetime.)*
- `VarStorage` (the old prototype) is the right *shape* — a handle yielding a current
  value while abstracting the backing. The cross-Shard evolution changes only the backing:
  "gated published slot" instead of a raw pointer or arbitrary function. In-process *within
  one Shard*, `VarStorage` as-is is fine — it's C++, use pointers freely.
- **Decision: no raw pointers across Shards.** A raw-pointer Shard can't be isolated
  (pointers don't cross a process boundary → forces in-process, forecloses the sandbox),
  can't be safely hot-reloaded (dangles on swap), is invisible to the tap (loses the
  observability that made the miner detectable), and is write-capable unless you're
  perfect. Blessing it would *contract* potential, not expand it — and once a raw-pointer
  mode is marketed as "fast," it becomes the default reach and the ecosystem loses the Zen
  properties through convenience. **Not forbidden**, though: two trusted first-party
  in-process Shards may share a pointer by mutual agreement like any two objects in one
  address space; the runtime neither provides nor blocks it; doing so steps outside the
  guarantees, knowingly — symmetric with the linked-libcurl case. The safe path is the
  easy default; stepping off is a deliberate, named exception.
- **Honest limits:** the real cost isn't danger, it's reintroducing **shared mutable
  state** — the thing pure message-passing eliminated to stay clean. Single-threaded FIFO
  means no torn reads *on the bus thread*, but if a reader runs on a different thread than
  the bus pump (e.g. a render thread reading while combat writes), that boundary needs
  double-buffering or an atomic snapshot. So this is a **per-slot** trade, not a global
  default: an easy yes for eventually-consistent display data (a one-frame-stale HP bar is
  invisible), a real question wherever a reader needs a transactional consistent view.

**Status:** designed, not built. Genuinely new — not in any prior seam list.

### 2.5 How the three pillars interlock

They are one model: **a grant, projected onto the boundary its hosting mode provides.**

*(Realized so far: B1 made the **message** boundary real, B2 the **process** boundary; B3
will make the **syscall** boundary real. The model below is unchanged — it is now partly
built rather than wholly designed.)*

- The **grant** (Pillar 1) is the single source of truth for what a Shard may do.
- The **hosting mode** is decided by trust (Pillar 2): full-trust in-process → fast,
  bus-enforced, vetting is the tool; isolated out-of-process → crash-contained now (B2),
  OS-enforced and scoped to the grant once sandboxed (B3).
- The mode determines which **enforcement boundaries** apply (bus-only vs bus + syscall)
  *and* which **access mechanisms** are even available (Pillar 3): holding a slot handle is
  itself a grant, and an isolated Shard **cannot** hold one (no shared address space) — so
  it gets push-messages instead. Which is a second reason push is the right default and
  slots are the trusted-in-process optimization.

---

## 3. Deferred seams (unchanged, recorded so they don't evaporate)

Still deferred-with-intent, per the Level 0 seam-readiness review: migration transform
registry; emit *enforcement* (chokepoint reserved at `Mail`); full schema-as-value beyond
the manifest precursor; multi-threaded dispatch; request/response await; content-id
fast-path (kept untaken on purpose); cross-language libraries; behavioral contracts;
static-struct half of the codegen marriage. The three pillars above now subsume or
sharpen several of these — and three have since **shipped** as B1/B2: capability-gating,
cross-process delivery, and crash isolation.

---

## 4. Resolved: build order → **B** (capability + isolation first)

**Decided.** Route **B** was chosen, and the phases built since are named for it: **B1**
(capabilities), **B2** (isolation), **B3** (the OS sandbox, next). The demand-loading use
case (gameplay code loading and unloading as a *hot path* — the game's own spawn logic
sending the kernel `LoadLibrary` when an ogre appears) settled it: it makes
kernel-control-over-the-bus the hot path, which forces capability-gating before that surface
is safe to expose. The two options, kept for the record:

- **Option A — console first.** Ship the human instrument sooner; the console manages the
  kernel via a direct, trusted C++ call (both are floor-level), and drives loaded Shards
  over the bus. Capability + isolation comes after.
- **Option B — capability + isolation layer first.** Build the kernel's message door plus
  the bus's "may you send this to them" gate (and the in-process/out-of-process hosting
  split), with demand-loading as the proving ground; the console is then born *on top* as
  the first grant-holder, in the real model.

**Resolved: B** — the author's call, made. The console's *value* (discover and drive a
live system) doesn't strictly need kernel-control-as-messages, but the demand-loading case
showed the capability layer is the real spine of everything multi-Shard, and being born
holding a real grant beats retrofitting one. The counter-argument for A was real (the
console feels half-real until "operate the kernel like everything else" is true, and the
human instrument in hand sooner accelerates everything) — but B won, and B1 + B2 now ship,
so the console (§2.1) will be born *on top*, as the first grant-holder, in the real model.
**B3 (the OS sandbox) is the remaining isolation step**, and the natural next phase.
