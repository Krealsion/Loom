# Zen design ledger — built, designed, and open

**Purpose.** This ledger exists to keep a clean line between what is *implemented* and
what is only *designed*, so work can continue without confusion and so neither the
maker nor any assistant later mistakes architecture-we-permit for code-that-exists.
Everything under "Built" ships and is verified. The pillars in §2 are the **design
record**: some have since been built in phases (B1, B2) — their **Status** lines mark
exactly what — and the rest is design the current codebase *allows*, not a line of it
written. When in doubt, assume a thing is not built unless a **Status: built** line (or
the Built section) says so.

**Standing rule — "proven" means a test asserts it.** In any Zen doc, calling a property *proven*
means a **regression test asserts it**. A property that holds only by reading the code is **"true by
construction, not yet pinned"** — say it that way, never "proven." (The project's own ethos applied
to its prose: never claim an enforcement, or a proof, you didn't earn. The harness was made honest
to match — a security proof that cannot run **fails** rather than skipping green.)

---

## 1. Built (ships, green under `-Werror` + ASan/UBSan, GCC 11.4 / WSL)

| Layer | What it is | Status |
|---|---|---|
| `loom` | self-describing value + the one gate. Schema (7 frozen kinds, FNV content-id), Value (positional), `admit` (the sole validator), Registry (immutable `(name,version)`), serialization (native canonical binary + compat JSON), `Unverified` (untrusted-until-proven). | built |
| `zen-switchboard` | in-process bus, first live boundary. Gated delivery (admit at delivery vs the recipient's accept-schema), single-threaded FIFO `pump` with reentrancy guard, observer tap, `Weave` interface, lifecycle (`snapshot`/`kill`/`reload` + `swap_state`), abstract `Bus`. | built |
| `zen-kernel` | DLL loading across a true C ABI. Versioned descriptor, `ZenByteSink` ownership (no cross-allocator free, no host pointer into library memory), host adapter (a loaded Weave *is* a `Weave`), bytes-as-currency re-admitted through the gate, validate-then-commit hot-reload, safe teardown, manifest-as-gated-Value (the minimal schema-as-value precursor). | built |
| the **weave** layer (header-only) | low-ceremony weaving. `ZEN_SHAPE` (schema-from-struct, Kind deduced from C++ type, version required), `WeaveBase` (typed handlers, derived accept-set / snapshot / revive, `Mail` as the sole outbound path), `mount<>()`. | built |
| Level 0 hardening | dispatch selector → `(name,version)` (null-deref fix), loud no-match, `swap_state` split from the crash-revival budget, emit-honesty by test with `Mail` reserved as the enforcement chokepoint, `content_id`-site grep sweep, seam-readiness review. `same_identity` strengthened to true `(name,version,content_id)` identity. | built |
| Capabilities **(B1)** — `grant.hpp` + switchboard + kernel door | the in-process grant model. Per-Weave `Grant` (send-rule selectors over shape→target, plus reserved OS-capability flags); capability-gated delivery (the `WeaveBus` a handler receives stamps its identity and authorizes against its grant *before* the gate → `CapabilityDenied`); `Switchboard::send/publish` are the ungated host root, the gated `WeaveBus` is all a Weave ever holds; public `send_as`/`publish_as` (host re-enters a Weave's output with the sender stamped from the connection); the kernel's `LoadLibrary` door is itself a gated capability, demonstrated against native Weaves. | built |
| Isolation **(B2)** — `zen-isolation` library + `zen-weave-host` child | out-of-process hosting + crash supervision. A Weave runs in a child process, indistinguishable to the bus (a proxy that *is* a `Weave`); framed, bounded, defensive unix-socket IPC (per-frame + backlog caps, EOF = death, never blocks the host); the child reuses the kernel C ABI and links no loom (a byte shuttler); child output is re-admitted through the one gate host-side with the sender stamped from the connection; a single-threaded `step()` (drain IPC → `pump` → supervise) keeps the bus's FIFO/reentrancy intact; on crash, bounded reload from the host-owned snapshot then quarantine. **Isolated, not sandboxed** — the grant's OS-capability flags stay inert (that is B3). | built |
| OS sandbox **(B3)** — `zen-isolation/sandbox` + native enforcement | the detect→apply→know→refuse-or-proceed **honesty lattice** and the first real syscall-level enforcement. `detect_enforcement()` **probes** (never assumes) what this host can impose, per-capability; the **Network** flag is enforced by launching a child into a no-interface user+net namespace (native `fork`+`unshare(CLONE_NEWUSER+CLONE_NEWNET)`, sandboxed branch only — `posix_spawn` unchanged when Network is granted), so a child without the grant gets `ENETUNREACH` from a real `connect()` regardless of what the `.so` links; `containment()` is generated from what was *actually* imposed and **positively confirmed** (the child's `/proc/<pid>/ns/net` inode differs from the host's — verified, not inferred; never a false claim); a *surprise* real-entry failure refuses in **both** strict and dev mode (no run-while-claiming-contained path); per-capability resolution + an iterating `containment()` make B4 "a probe + an enforcement call"; a default-strict **dev-mode** knob converts a *known-gap* refusal into a visibly-uncontained warning; hard-vs-graduated capability vocabulary (`FsAccess`, safe default) reserves filesystem's home. | built |
| Filesystem sandbox **(B4)** — `zen-isolation/sandbox` mount-ns view | the **first graduated capability**, enforced by a private mount namespace. The grant's `FsAccess` level (None → ReadOnly → WriteScoped → WriteNoExec → WriteAnywhere, default `None`) picks a point on a safe→dangerous axis; the child `pivot_root`s into an **allow-list** view (fresh tmpfs root, the loader closure + its own `.so` bind-mounted read-only via `mount_setattr(AT_RECURSIVE)`, an optional scratch tmpfs, root remounted read-only — built **private-first** to stop reverse mount propagation), so a stranger's Weave cannot read your `$HOME` secrets (they are *absent*, not hidden), cannot write outside `/scratch` (`EROFS`/`EACCES`), and at `WriteNoExec` cannot `execve` what it writes (`EACCES`); `WriteAnywhere` is the honest opt-out (reported *not contained, by grant*). Confirmed via a distinct `/proc/<pid>/ns/mnt` inode + the same fail-safe/dev-mode/surprise-failure discipline; the OS verdict is proven on the bus (the fs-probe still emits — sandbox ≠ muzzle). `FsAccess` is now the single source of truth (the binary `FilesystemRead/Write` flags were removed). **The map regression fix:** this host refuses a child's self-map, so the **parent** writes the child's uid/gid maps over a pipe handshake (which also hardened B3's netns entry). | built |
| Resource containment **(B5)** — `zen-isolation/sandbox` cgroup-v2 | the **first quantitative capability** (a *limit*), enforced via a per-Weave cgroup-v2 leaf — completing the threat-model ladder (network + filesystem + resources). The grant's `ResourceLimits` (memory/pids/cpu) default to **host-computed conservative** values (no knob): memory = RAM/8 capped 1 GiB / floored 128 MiB, pids = 512, cpu_weight = 100; `with_unlimited_memory()` is the **only** opt-out — it removes the memory cap alone (**pids stays bounded — no grant can license a fork bomb**; a structural invariant, not a default). The host discovers its **delegated** base, builds the no-internal-processes hierarchy (drain into a `zen-supervisor` leaf, enable `+memory +pids`, and `+cpu` where delegated), and at the spawn **sync point** the parent moves the child's pid into its leaf before release. A memory bomb is **OOM-killed within its cgroup** (host survives → reload-then-quarantine, proven with a granted-survives **negative control**); a fork-bomb is bounded by `pids.max` — **even under `with_unlimited_memory()`** (the headline invariant proof). Confirmed via `/proc/<pid>/cgroup` + limits read-back; fail-safe/dev-mode as usual — resources **never resolve to `Granted`** (a leaf with at-least-pids is always created when cgroups work). **Delegation is invocation-dependent** — a plain `wsl bash` lands in the root cgroup (no delegation → fail-safe refuse), so the suite runs under a delegated scope (`run-under-scope.sh`); `cpu.weight` is set-and-confirmed where the cpu controller is delegated, honestly reported absent on this host (which delegates only memory+pids). An absolute `cpu.max` quota is a named future opt-in. | built |
| Policy **P1–P2** — the powerbox (Storage, then Network brokers) | the first **policy** layer (§2.6): *where a grant comes from*, proven with two brokers on two capabilities. **The ask** (advice, never authority): manifest **v2** `requests` → `zen.CapabilityAsk` + an ergonomic `ZEN_ASK`, gated like the accept-set; `declared_ask` surfaces it but the grant is unmoved. **The floor + grant-record:** `mount_mod` floors an unknown mod (default `Grant` + a send-rule to the storage **role**); authority above the floor comes only from a persisted, **content-hash-keyed** JSON grant-record the host writes (`record_grant_delta`, TCB-only — the host's pen). **Role-addressing:** register under a role, `send_to_role`, `allow_to_role`/`permits_role`, **authorize-by-role *before* resolution**, singleton, reload-stable, unheld-role → `NoSuchTarget`. **The broker:** a new sender-less `Op::EmitRole` frame + `ZenHostApi::send_to_role` close the out-of-process seam (sender stamped from `link.id`, never the wire); persistent-`WriteScoped` binds `storage_root` writable (TCB-only; mods stay `None`); the out-of-process **StorageBroker** (role `"storage"`) scopes each mod's keyspace by the stamped sender. Proven end to end with negative controls: mod-vs-mod scoping, floor-without-disk, ask-is-not-a-grant, reload-keeps-state; broker-down → `NoSuchTarget`. **Session-scoped** (keyed by the ephemeral sender), stated honestly in `containment()`; first-class persistent identity (save-files) is the named successor. **P2** generalizes the powerbox to a second capability (network): `GrantDelta` gains `roles`; the floor grants storage to all but **net to none** (net is a recorded delta — and only the role send-rule, never `os_cap::Network`, so a net mod stays OS-network-denied and reaches the net solely through the broker); the out-of-process **NetworkBroker** (`os_cap::Network`, `FsAccess::None`, role `"net"`) validates `host:port` against a **software allow-list** (NOT OS — the higher-trust broker, honestly reported) and does raw TCP. Negative controls: mediation (a net-denied mod reaches the allowed loopback listener *only* via the broker; its own `connect()` → ENETUNREACH), allow-list scoping, floor-denies-net. | built |
| Console **Stage 1** — `zen-console` engine + a plain terminal (the first **doing-layer** component; §2.1) | the first thing a *human* uses: a message-native bus participant. The **engine** (`ConsoleEngine`) is **frontend-agnostic and fully testable with no terminal** — it returns **domain data** (weaves, field descriptors, received Values, the buffer), never formatted text or a widget tree; the terminal is a throwaway skin a GUI later replaces, inheriting the engine whole. **Discovery-first:** `weaves`/`describe` read the registry, so it drives shapes it has never seen. **Two layers:** the registry *guides* at compose-time (fields/types, `construct_blind`); the **gate enforces** at send-time (`send_as` → admit; a missing field is a clean `GateRefused`, never a mis-send). **Wildcard-accept** (`AcceptMode::AnyRegistered`) — the one bus change: accept any *registered* shape, **gated against the registry-resolved schema** (unregistered → refused, reaches no one); opt-in, ordinary Weaves unchanged. The console is an in-process raw Weave buffering every reply (`m1`,`m2`,…), the **most-granted participant** (broad `allow_any` send + wildcard-accept + tap + discovery) but each a **grant**, not a bypass — sends go through the gated `send_as`. Proven frontend-free: participant loop, gated-send backstop, discovery on an unseen shape, wildcard-accept + unregistered-refusal. Successors: **Stage 2** (dataflow/`$m1.field` + assumption ladder), **Stage 3** (UI-as-data TUI). | built |
| Console **Stage 2** — the dataflow layer (`zen-console`: references + the assumption ladder; §2.1) | turns the console into a **dataflow surface** — the text-mode prototype of the flowchart crown. **A reference is a wire:** `$mN.field` reads a scalar `Cell` off an immutable buffered `Value` and routes it into a new message (output→input by typing). Resolution is the **engine**'s (`resolve_ref`, standalone-tested; the label format `mN` is the engine's, the terminal only lexes `$label.field`); a reference only ever **reads** the buffer, errors clean on missing entry/field/empty. Scalar-only this stage. **The assumption ladder** (`compose` over a list of literal/reference args, each optionally `field=`-named): **named wins → positional (declaration order, fails *as a whole* and falls through on any mismatch) → type-directed (unique open field per arg) → prompt**; a still-open required field also prompts. `NeedsInput` is **structured data** (open fields + unplaced args), never a printed string. Coercion is narrow: a numeric literal widens Int→Float, a reference matches its resolved type exactly. **The gate is the backstop that lets the ladder guess fearlessly** — a wrong-typed value is caught (at compose, since the engine knows both types; the gate the unconditional floor beneath), never a silent mis-send. The terminal lexes narrowest-type literals / `$mN.field` / `field=value` (quote to force Text: `"5"` text, `5` Int) and renders the prompt plainly. Proven frontend-free: reference round-trip, each ladder rung incl. positional fall-through + prompt-on-ambiguous, gate-backstop on a wrong-typed reference, reference errors. Successor: **Stage 3** (UI-as-data TUI — intent and relationship, never absolute position/size). | built |
| Console **Stage 3** — UI-as-data: the renderer-agnostic widget tree (`zen-console` + a termios TUI; §2.1) | the doing-layer's **capstone**: the console's OWN interface becomes **data**, giving "the GUI inherits the engine" real structural support. The engine library emits a **semantic widget tree** (intent + relationship) built from its public domain data; the *same tree* a terminal renderer resolves to box-characters a GUI later resolves to pixels (one description, many renderers — like an HTML DOM and a screen reader). **THE BET, made structural:** **no geometry member on `Widget`** (a closed, geometry-free member set — no `x`/`y`/`w`/`h` field to write) **+ a name-based compile-time tripwire** on ~10 coordinate spellings (`int x` fails to build; `int px` compiles clean — **defense in depth, not unrepresentability**); layout happens **only in a renderer**. Vocabulary (small, general, used here only for the console): `VStack`/`HStack`/`Region`; `List`/`Log`/`Text`/`Field`; an **overflow policy** + a **focus marker** + a **weight** *relative* grow-hint (never an *absolute* size). `Widget` is one value type with defaulted `==` → the tree is **one value**, headlessly assertable and region-diffable. **One real renderer + a renderer-agnosticism proof:** the full-screen **termios TUI** (`console_tui.cpp`, hand-rolled ANSI, NO ncurses/no new dep) is the only production renderer and the only place cells exist; a ~50-line test-only `render_outline` walk consumes the same tree to prove it carries no medium (ignores `weight`; the TUI resolves `weight` as a relative size in cells). **Guidance is engine-produced** (`guidance_for(partial)` advances empty→weave→shapes→fields, carried in the `Field` hint). **Message-driven dirty:** the single bus observer (`record_tap`) sets per-region flags (`buffer` on a reply to the console, `weaves` on death/revival, `tap` on any event), drained by `take_dirty()` — the Zengine retained-mode idea, change-signal = bus messages. **Symmetric input seam:** renderer-agnostic semantic actions (`FocusNext`/`Activate`/`Edit`/`Submit`/…) the `ConsoleUi` controller applies; the raw-key→action map is the TUI's only terminal-coupled code, so a GUI inherits input too. Proven frontend-free: tree structure; no geometry member (fence + structural-`==`); one-tree-two-consumers; guidance advances; a bus message drives the buffer pane; a scripted-action TUI smoke incl. the NeedsInput prompt. Green in Debug + ASan/UBSan. Successors: a **general Weave-emitted-UI protocol**, the **GUI renderer**, geometric/fabric-level UIs, the **result-graph buffer**. | built |
| Console **— the terminal-backend seam + a Windows backend** (cross-platform TUI; §2.1) | extracts the TUI's only platform-specific code into a **`TerminalBackend` seam** (`terminal.hpp`, in the TUI exe, NOT the engine) — `is_interactive`/`size`/`read_byte`/`read_byte_timeout` + RAII raw-mode; `make_terminal()` is the one per-platform symbol. **POSIX backend = the existing termios/ioctl code MOVED, behavior-identical** (the safety property: Linux preserved by construction, not rewritten); **Windows backend = the Win32 Console API by-the-book** (VT input/output so the *same* ANSI renderer + escape-key parsing run unchanged; `srWindow` size; graceful VT-unavailable fallback). `console_tui.cpp` is now **platform-header-free**; `zen-console` stays terminal-free. **Build-verify division** (the cpu-seam arrangement): Linux is *proven* green (ctest 20/20 Debug + ASan/UBSan, TUI smoke unchanged), the Windows backend is *best-effort, Josh-verified in CLion*. **CMake gates** `zen-kernel`/`zen-isolation`/`zen-weave-host` + the Linux-only test sources/weavelibs/suites (incl. `test_capabilities`, which loads a real `.so` via the kernel) behind `if(NOT WIN32)` so the portable subset configures on Windows — **invisible on Linux** (full target/suite set unchanged). **Trusted-code, in-process, NO containment surface** (no `IsolationHost` in the TUI build — the sandbox is Linux/WSL; containment honesty belongs to the WSL-bridge phase). **Seams appreciated:** this seam is the hook the **WSL remote console** (a socket `TerminalBackend`) and the **GUI** (another renderer) plug into; flagged for the remote phase — **output not yet behind the seam** (`draw()`→stdout; a socket backend wants a `write()`/`flush()`) and **the TUI owns the I/O loop** (sync blocking-read; async would invert it); and the gate exposes **native-Windows `.dll` loading** (`dlopen`→`LoadLibrary`) as a hooked-but-probably-never seam (WSL-hosting dominates). | Linux built/green; Windows Josh-verified |
| **The remote-operator bridge** — the WSL crossing (`zen-bridge` + `zen-console-remote`; §2.1) | the first thing across the **host boundary** as a participant: a Windows console drives a WSL-hosted bus. **Fat-client + an operator-protocol of messages:** a remote console can't hold a `Switchboard&`, so the engine runs **client-side** and discovery/tap/send become framed messages the host answers/streams (the twice-deferred *discovery-as-messages* purification — discovery + the tap stop being privileged host methods and become gated messages; purifies invariant 1). The frontend is **unified across transports** — a `loom::Console` interface both `ConsoleEngine` (direct) and `RemoteConsole` (over the wire) implement, driving the SAME `ConsoleUi` + shared renderer (`tui_render`); the ladder/`resolve_ref`/`describe` are shared free functions (no duplication); the in-process path stays direct (unified at the interface, not forced through bytes). The send is an **out-of-process `Emit`**: compose runs client-side against a wire-fetched schema (`Describe`→encoded `Schema`, the IPC currency) + the local buffer, and the assembled bytes ship as a `Send` the host **re-admits through the one gate + `send_as`-stamps**. **The connect-authority chokepoint** (`authorize_connection`→full `OperatorGrant`, model A — reachability IS authority): the WHOLE B/C future-proofing in **that function PLUS the `allow_any()`/`AnyRegistered` registration that consumes its result** (model B bearer-token — makes the *unauthorized-connection forge* sayable: a failed-token connect must register no proxy, process no frame; model C graduated/differential grants that narrow `allow_any()` per operator, **where "Weaver" is born** — the identity trigger); hook, not feature. Operator proxies are hidden from discovery but *addressable* (small-int ids) — same-principal under model A, a cross-principal channel the identity phase must treat as surface. **Provenance:** the sender (+`reply_to`) is stamped from the CONNECTION, never the wire — pinned by a test that **forges the wire frame** a raw client manufactures and proves the stamp wins (the unsayable-attack discipline, mirroring `forge_client`). **Event-driven single-threaded multiplexer** (**`poll`/`WSAPoll`** — no `FD_SETSIZE` ceiling — over {listener, conns} / {input, socket}; the Windows `WaitForSingleObject` deadline-loop) — the client's readiness-to-receive, NOT bus concurrency (the bus stays FIFO; no threads; reentrancy untouched); **disconnect handled as an event** (a closed/`SIGKILL`ed peer → readable-then-EOF → `reap_dead` unregisters its proxy). **Squared edges** (bounds stated + pinned; no dark fates): a stated connection cap (`kMaxOperatorConnections=32`, accept-then-shed + `declined_count()` — a reconnecting fd-hog is contained); the handshake is load-bearing (a pre-Hello frame *severs*, anti-Postel); the three Send-path drops (malformed/unknown-schema/gate-refused) emit **`SendRefused`** (per-frame, non-fatal → a `"BridgeRefused"` client tap kind, honestly NOT a bus event); the client bounds pending unknown-schema replies (`kMaxPendingDelivered=64`, drained on `SchemaNone`); and the tap observer stays **copy-only** — the weave-list refresh (a bus registry read) is **deferred** to after `pump()`, since reads-during-dispatch is not a *stated* Switchboard guarantee. **Output behind the seam** (`write`/`flush` added; the socket-frame backend is hooked-not-built). **Portable transport** (`BridgeChannel` mirrors the isolation `Channel`; one `#ifdef` for POSIX vs Winsock): **AF_UNIX** local, **AF_INET 127.0.0.1** the crossing (WSL2 localhost-forwarding). **Honest containment:** the security boundary is the **reachability of the socket** — a deployment responsibility, not authenticated by the bridge; abuse-tier. **Proven both altitudes:** the local mechanism (`bridge` suite, 12 cases / 135 assertions) green **Debug + ASan under the scope, `-Werror`** + natively on Windows via MinGW (16/16 portable; 10 bridge cases there — fork-kill + AF_UNIX POSIX-gated); the **real Windows→WSL crossing** (a MinGW Windows process drives the WSL bus end-to-end: discovery + describe + gate-send + reply + tap + graceful disconnect) — proven by me, not Josh-verified-only (except interactive raw-mode *input* on Windows, the established CLion division). **Relocation argued to fall out** (true by construction, *not yet pinned*) as "snapshot (exists) + ship over the bridge (now real) + revive in sandbox (exists) + re-point senders" — and reading the Switchboard sharpens the cost to **one new primitive, not zero**: no operation binds a *replacement participant behind an existing `WeaveId`* (register mints fresh; reload/swap preserve the id but `admit` against the unchanged state-schema, so a divergent shape is gate-refused), so step (d) needs either a `WeaveId`-rebind or **location-transparent addressing** — the seam relocation will pull. | built/green (Linux + Windows); crossing proven |

| **The Poke weave** — live inspect/manipulate under `ZEN_EXPOSE`/`ZEN_HIDE` (`weave/poke.hpp` + `weave/poke_weave.hpp`; §2.1) | the first debugging capability, **by message only** — the debugger is a weave like any other. **The access model** (`weave/shape.hpp`): with **no tag** a field is read-exposed + write-hidden; `ZEN_EXPOSE` opts *in* to write, `ZEN_HIDE` opts *out* of raw read (message-only value); one spelling, two scopes — field scope in the `ZEN_SHAPE` list, a bare `ZEN_EXPOSE();`/`ZEN_HIDE();` at struct scope **IS apply-to-all** (shape-bits OR'd per field; one primitive, pinned by comparing the two spellings). The bits ride `FieldEntry`, invisible to `build_schema()` — a tagged struct derives the **same content-id** as an untagged twin (pinned vs hand-built). **The honesty boundary:** tags govern *value* access only — `access_of<T>()` lists every field's name/type/tag-state, nothing filters it; **no secret state** (a hidden field is visible *as hidden*). **The doors:** every woven Weave answers `zen.PokeDescribe/PokeRead/PokeWrite/PokeResetState` (→ `zen.PokeStructure/PokeValue/PokeAck/PokeRefused`) in `handle()` **before maker dispatch**, from pure standalone-tested enforcement functions; a `static_assert` stops a maker intercepting (an answered structure can't lie); every refusal is an honest answer with a reason. Values cross **as text** parsed against the field's declared kind (`from_chars`/`to_chars`, exact) — a bad literal refuses cleanly; **scalars only this phase** (non-scalars stay structure-visible). **No new authority:** answers are ordinary gated sends — `mount()` adds `allow_poke_answers` (plain `allow_to_any` rules), `mount_granted` stays sovereign (an ungranted weave's answers are `CapabilityDenied`, pinned); `emitted_schemas()` stays the maker's declaration. **The Poke weave** is an ordinary participant: commands (`zen.PokeInspect/Get/Set/Reset{target}`) forward the protocol shape with a `seq` correlation; the answer relays back to the asker **only if the bus-stamped sender is the poked target** (a forged answer from a granted third party is *sayable* and *dropped* — pinned); pending pokes are honest bounded state (itself poke-inspectable). **Driven from the existing console with zero console changes** (pinned end-to-end: inspect shows hidden-as-hidden; set on exposed acks + lands; set on un-exposed refuses with reason; hidden raw-read refuses while the target's own front-door query still serves it computed — the message interface stays sovereign). **Level-0 hardened** (`final` on `handle`/`accepted_schemas` seals maker interception; whole-state detection value-checked; misplacement `static_assert`; authority prose right-sized) after a multi-lens adversarial review. Suite `poke`: 19 cases / 147 assertions, green Debug + ASan under the scope + Windows MinGW. | built |

| **Legibility sugar** — the standard reply shapes + the relay pattern + the layer rename (`weave/standard_shapes.hpp` + `weave/relay.hpp`; §2.1) | the message vocabulary put under the **least-complete-information razor**: carry the least that still leaves a *complete* image — test each field by its absence (reader *confused* = load-bearing; merely *less-informed* = sediment). **The standard reply shapes** (`loom::Ack` = `zen.Ack`, zero fields — the correlation carries the what; `loom::Refused{reason}` = `zen.Refused` — a refusal without its reason is an incomplete image, and the reason is written self-contained; `loom::Result{value}` = `zen.Result` — the payload is the image): ordinary registered/gated/content-id'd shapes (pinned vs hand-built twins), one vocabulary every protocol shares — deliberately NOT `Error`/`Value` (`loom::Error` is the gate's admission *fault*; a `Refused` is a deliberate answer *by policy*; `loom::Value` is the value type itself). **The rule, recorded:** standardize the contentless and simple-payload replies; a bespoke reply survives ONLY by breaks-on-absence. **The poke migration proves the collapse:** `PokeValue{field,type,value}`/`PokeAck{op,field}`/`PokeRefused{op,field,reason}` → the three standard shapes (their `op`/`field`/`type` restated what the correlation, the request, and the structure already carried — the reset-refusal's blocking field, the one datum NOT in its request, lives in the reason); `zen.PokeStructure` **survives by the razor** (a weave's full structure is genuinely protocol-specific — remove its fields and the reader is confused). **The relay pattern** (`weave/relay.hpp`): the command→forward→answer dance expressed once — `loom::forward`/`loom::relay` over a bounded `loom::RelayState` (an ordinary shape: snapshots, revives, poke-inspectable), the **stamped-sender anti-forgery wall written once in the substrate**; carries only the correlation bookkeeping (no reply-type registry, no timeouts, no knobs); the Poke weave's state IS a `RelayState`, its handlers one-liners, its `Accept<...>`/`Emit<...>` untouched and visible (sugar removes bookkeeping, never the contract). **The layer rename:** `switchboard/weave.hpp` → `switchboard/weave_contract.hpp` (raw contract / authoring sugar `weave/weave.hpp` / umbrella `zen/weave.hpp` now legible at a glance; boring diff, identical green). **Named follow-on, not built:** broker/bridge adoption (`StorageValue` stays bytes-bespoke unless a bytes result earns entry — its empty-means-absent sentinel could become an honest `Refused`; `NetResponse{ok,data}` could split into `Result`\|`Refused`; the bridge's `SendRefused` is a socket-layer op — a design question). Suite `poke`: 20 cases / 154 assertions, green Debug + ASan under the scope + Windows MinGW. | built |

| **UI-Builder Phase A** — the component vocabulary (`console/component.hpp` + the evolved `console/ui.hpp`; §2.1) | the semantic tree **evolved in place** into components-with-typed-slots — shapes only (no renderer, no Builder panels, no live binding, no routing/presenter runtime). **ONE tree, not two:** the console's `Widget` IS the component tree; the evolution added **abstract interaction intent** (`activatable`/`editable`/`reorderable` — what the operator may *do*, never which key; they **replaced `focusable`**, cut by the razor: written-never-read, focus-eligibility derivable from intent), the **`Slot` kind** (a typed open hole `slot_name`+`slot_accepts` ∈ {Component, Route, scalar Kind}; a slot is a *position*, so it is a node; its children are the placeholder preview), **data binding** (`from_field` — declared, resolved by a later phase) and **route addressability** (component `name` = address; node `route_to` = navigation intent). The TUI renders the evolved tree unchanged (+ a Slot projection) — the standing agnosticism proof; the outline **prints** intent/bindings/routes (meaning) and **ignores** weight/overflow (hints), the two-renderer discipline pinned. **A schematic is data:** `zen.ui.Node`/`zen.ui.Component`/`zen.ui.Presenter` v1 (hand-registered dotted names; twins pinned) ride `to_value → serialize → parse → admit → from_value` through the same single validator as the bus. **The wire form is flat** (a schema cannot self-reference; nested decode is depth-capped): `nodes` + index `children`, root 0, pre-order; `flatten()`/`tree_of()` the lossless pair, pinned by structural `==` incl. the console's own live tree. **Honest layering:** the gate proves shape-conformance, NOT tree-ness — `tree_of()` is the vocabulary's own decode check (index range, reached-exactly-once, no cycles/orphans, known spellings, ranges incl. contract_version-as-u32, depth ≤ 256, and per-kind **child arity** — Region wraps exactly one, List/Log/Text/Field none: child structure is where the two renderers would silently DIVERGE, so it is refused; unused scalar fields stay lenient), refusing with a reason; hostile shape-conforming frames (detached cycle, two-parent child, root back-edge, arity violations, 300-deep chain, unknown spellings, out-of-range integers) are **gate-admitted then tree_of-refused, pinned per class through the real gate** (the unsayable-attack discipline — honest constructors can't express them); the TUI weight `split()` hardened to 64-bit sums alongside (a wire-legal wide+heavy stack could wrap 32-bit). **Contracts make slots checkable:** a component declares `(contract_name, contract_version)`; `check_bindings` verifies every binding against the contract's actual schema (Text/Field ↔ scalars, List/Log ↔ List fields with scalar ELEMENTS — rows are text, containers bind nothing, slot types known, slots named uniquely — filled by name later, and route_to requires activatable — a route that can never fire is dead). **View/presenter split as separable data:** a component never names what feeds it; `zen.ui.Presenter{view, source_role}` binds a source (by ROLE, persistable) to a view (by name) — pinned at the schema level (no source field on the component, no tree on the presenter); the crash-graceful payoff arrives with the presenter runtime. **Stress placeholders are the DEFAULT:** `stress_text()` (long + an unbroken 64-char word) / `stress_number()` (`-9223372036854775808`) / `stress_rows()` (empty) / `stress_nested()` (deep ladder); the design-time constructors (`bound_*`, `open_slot`) have no happy-path preview to pick — a schematic self-stress-tests its layout. Suite `component`: 15 cases / 193 assertions, green Debug + ASan under the scope + Windows MinGW (18 portable suites now). Successors: SDL2 renderer (Phase B — also pulls lifting the vocabulary out of the console target), Builder panels (Phase C), live binding (Inspector), routing/presenter runtimes, fill-out example data, contract content-id pinning (identity/migration). | built |

| **UI-Builder Phase B** — the SDL2 projection (`zen-ui` + `zen-ui-pixel` + gated `zen-ui-sdl`; §2.1) | **the agnosticism test made real: a second, radically different renderer consuming the IDENTICAL tree — and it needed ZERO additions to the node vocabulary** (the headline; the litmus held: no SDL-only node field exists). The one shared addition is INPUT-side: `Action::SelectAt` + `InputEvent::index` — the pointer names a row where keys walk (intent, not medium; a mouse-reporting terminal could emit it; `ConsoleUi` gives it real semantics under the same single-writer clamp as SelectDown). **The vocabulary-target lift** (the seam Phase A named, pulled by its trigger): tree + component vocabulary now in `zen-ui` (`include/zen/ui/tree.hpp`+`component.hpp`), depending only on core; the console keeps only what is the console's (`UiState`/`Focus`/`guidance_for`/`emit_ui_tree`/`ConsoleUi` in `console/ui.hpp`); `zen-tui` links `zen-ui` NOT `zen-console` — boring, behavior-identical (identical suite counts as the safety proof). **The projection split mirrors the TUI's:** `zen-ui-pixel` = the layout brain (tree → paint-ordered draw commands `Fill/Text/PushClip/PopClip` with semantic `PxRole`s + interactive `PxTarget`s; metrics INJECTED so the logic is deterministic and SDL-free — pinned in the ordinary suite on every platform incl. Windows-without-SDL); `zen-ui-sdl` (gated) = the thin skin (window, TTF metrics, command executor — the Zengine-borrowed surface→texture→copy with its leaks fixed — and `sdl_map_event`: raw SDL events → the SAME semantic `InputEvent`s `tui_map_key` produces; the controller never sees an SDL_Event). **Abstract→medium in the renderer, never the tree:** activatable → click-selects (`SelectAt`)/double-click-`Activate`s; editable → `SDL_TEXTINPUT` as byte-`Edit`s (UTF-8 reassembles exactly, pinned); keys mirror the TUI's bindings. **Overflow made real** (the hint the TUI ignores, honored here): greedy word-wrap hard-breaking over-wide words only at CODEPOINT boundaries (never mid-UTF-8-sequence, pinned on the new `stress_text_unicode()` gauntlet — CJK/emoji/combining/RTL/unbroken-mixed-word; DEFAULTS stay ASCII so every projection is stressed); `Truncate` ellipsizes at a codepoint boundary; outline-equality re-pins overflow as a hint. **Gating:** SDL2 is a dependency of the renderer target ONLY; WSL/Linux-first (`ZEN_SDL` ON there / OFF on Windows — the flagged platform decision); system SDL2 preferred, else PINNED release tarballs by sha256 (static SDL2 2.30.11 + SDL2_ttf 2.22.0 vendored FreeType), fetched to the WSL-native FS from `/mnt` checkouts (drvfs cannot hold SDL's symlinks; keyed per build dir). **Headless-provable:** suite `pixel` (SDL-free draw-command pins: wrap/truncate boundary safety, paint order, focus/selection fills, targets, hit-testing, weight parity, determinism) runs EVERYWHERE; suite `sdl` (dummy-driver: full pipeline with real TTF, synthesized-event input mapping) runs where the SDL target builds; on-screen pixels are Josh's visual verify (the established division). The SDL console frontend (ConsoleUi over the engine with this renderer) falls out nearly free — a named seam, deliberately not built (render, don't operate). | built |

| **The great re-homing** — Loom / Zengine / playground (repository topology; landed 2026-07-18) | **organizational; zero behavior change.** The three-tier social contract — *the Loom is everyone's, Zengine is the default set, your weaves are yours* — enacted in **topology** instead of doctrine, because the filesystem had been contradicting it. This repo re-homed to `Zen/Loom/` as a **whole-repo move, `.git` and all** (history, all 14 branches and `origin` intact; repo-relative paths unchanged, so **no history rewrite** — commit hashes are the same objects), and **Zengine became a separate repository that consumes the Loom the way a stranger does**: `find_package(loom)` by default, `-DZEN_LOOM_DEV=ON` for the sibling-source override. The arrow is now structurally un-invertible — **the Loom's build cannot see Zengine** — so every rough edge in the public surface hits the house before it hits a guest; grammar-not-answers enforced by placement rather than vigilance. The Loom gained its first **exported consumer surface**: an installable CMake package exporting `loom::core` + `loom::switchboard` and their header closure, **deliberately smaller than the build tree** (the UI trio, console, TUI, bridge and SDL skin are Zengine-destined and move in their own port phases; kernel + isolation are the Loom's but Linux-only and unused by any consumer, joining when a hosting consumer appears). `EXPORT_NAME` makes the installed targets carry the same spellings as the in-tree `ALIAS`es, so the dev override is a genuine drop-in and the two paths cannot silently diverge. **The safety proof is the identical-green boring diff**, quoted before vs after the move: WSL Debug 25/25 and ASan/UBSan 25/25 (254 cases / 80524 assertions each), MinGW portable 19/19 (194 cases / 80203 assertions), enforcement tallies 15 + 26 — identical on both sides. Exporting for the first time surfaced one real edge in the public surface, exactly as the split intends: `zen/weave/weave.hpp` reaches for the header-only `zen/kernel/schema_codec.hpp`, so the package ships that one header without the kernel library it is filed under. `reference/` in Zengine holds the V1 engine as a **read-only quarry** (plain working-tree import; its history stays in the old repo). | landed (organizational) |

| **The Weave Manager (1a)** — the lifecycle steward (`kernel/manager.hpp` + the answering `kernel/control.hpp`) | **operating the system became the same gesture as using it.** Lifecycle was host-only C++ API and the kernel door **discarded every outcome** — the most dangerous surface in the system was the only one that answered nothing, so a reload's state-schema mismatch (a real, correct, well-shaped refusal) died as an unread return value. Now the door answers with the standard shapes (`Result{id}`/`Ack`/`Refused{why}`, declared in `Emit<...>`, to reply_to-else-stamped-sender, correlation echoed), and **`WeaveManager` is an ordinary `WeaveBase` participant** whose whole state is `RelayState` — poke-inspectable, driven by the EXISTING console with **zero console code**. **No privilege:** its kernel authority is exactly `load_capability(control)`, target-scoped, assembled by the host at mount (`manager_capability`, deliberately not `mount<>`'s emit-default, which would grant `allow_to_any(LoadLibrary)`); a second granted participant drives the door with no Manager in the path (pinned), and the host keeps the pen. **The door executes primitives; the orchestrator composes** — `SwapWeave` is a Manager composite, NOT a kernel op, because a `SwapRole` primitive would make swap policy unreplaceable. **Two ops, deliberately:** `ReloadWeave` is reload-IN-PLACE (same WeaveId, state transplanted; a differently-shaped library refused, incumbent runs on) and `SwapWeave` REPLACES THE ROLE HOLDER (fresh state; a differently-shaped successor is the normal case) — the same `.so` pair proves both halves. **Addressing is role-first** (`Kernel::load` gained an optional `role`; registration is the only moment a role can be bound); `LoadLibrary` bumped to **v2** to pay the immutable-published-schema rule honestly. **The swap window, pinned three ways:** queued inbound traffic still reaches the incumbent (the swap's messages go to the queue TAIL); **the incumbent's in-flight REPLIES die with it** — a gated message is authorized by looking its sender up at *delivery* time, so an unregistered sender's queued answers are refused `CapabilityDenied` (fail-closed, correct, and a property of unregistering ANY live weave, not of swap — the concrete thing an atomic rebind would have to solve); and a failed swap leaves the role unheld, the asker hearing why, the empty slot refusing cleanly (`NoSuchTarget`) per the optional-participation floor. **One request, one answer:** the unload half is fire-and-forget (correlation 0, unmatchable by relay sequences that start at 1) because its outcome is subsumed by the load's, and being **role-addressed** it cannot destroy a weave the asker did not name — both pinned. **Homing applies the `schema_codec` lesson prospectively:** the Manager lives inside the already-unexported `include/zen/kernel/`, so no exported header reaches an unexported one — zero new export edges. Suite `manager` 16 cases / 171 assertions; WSL Debug 26/26 + ASan/UBSan 26/26 (270 cases / 80695 assertions each, +16/+171 exactly), MinGW portable count-identical at 19/19 (194 / 80203). Successors: `PrepareShutdown` + the cooperative handoff letter (1b, Loomstd-homed), snapshot configurability, invisible/atomic rebind, multi-multiplicity roles, the triage brain. | built |

| **The Weave Manager (1b)** — the letter, cooperative handoff (`weave/lifecycle.hpp` + the Manager's chain) | **continuity across succession**: a differently-shaped successor inheriting what its predecessor knew. Reload transplants state across the SAME shape; the letter **converses** across a different one. Vocabulary is **Loomstd-tier** (`weave/lifecycle.hpp`, beside standard shapes — NOT kernel-homed; the Manager is *a* consumer, not its owner): `zen.PrepareShutdown{}` / `zen.Bequest{role, items}` / `zen.ClaimBequest{role}`. **Two walls, both 1a's own pins rather than guesses. W1 — the letter dies with its sender**: a gated message is authorized by sender-lookup at DELIVERY time, so fire-and-forget would post the letter into the void (`CapabilityDenied`); the graceful path is therefore **two-stage by construction**, and the suite reads the ordering off the bus's tape (`PrepareShutdown` → `Bequest` → **then** `UnloadRole`, zero Bequest refusals). **W2 — the steward cannot push**: arbitrary domain shapes would need grants unknowable at mount and `allow_any` on a broker is refused, so delivery is **PULL — the heir claims**, reaching the steward by its well-known role `zen.manager` because a weave that just woke knows nothing else that outlives a swap. Pull is also gap-agnostic *by construction* — **the letter must not know the gap** is law; nothing assumes immediacy, wall-clock, or that the predecessor's WeaveId still means anything. **Messages only, no state blob** (Josh-ratified): a blob would be a shadow transplant with none of reload's shape agreement. **Items are `List<Bytes>` and the gate stays sole admitter** — investigation refuted `List<Message>` (a List's element is ONE TypeRef; the gate pins nested Messages to one schema by content_id), so each item is serialized by the predecessor and **re-admitted through the real gate by the heir on read** (`claim_item`). **Participation is checked BEFORE asking** (`QueryRole` → `RoleInfo{holder, converses}`) from data the kernel already holds — **no Switchboard API added**; `holder == 0` honestly conflates unheld-vs-native (the kernel cannot see a native weave's accepts; neither is a participant). **Floor both ends**: never-declared → hard swap automatically; never-claims → fresh start; declared-then-silent wedges **its own** swap only, escaped by an ordinary non-graceful `SwapWeave` — **no timeout machinery, on doctrine**. Store is bounded, latest-per-role, answered once, successor-authorized, **poke-inspectable**; a failed load **discards** the letter (no successor can authorize a claim; unclaimable mail is a leak) — the honest extension of 1a's failed-swap friction. `graceful` is a **field** on `SwapWeave` (**v2**), not a sibling op: same machine, one extra stage. `loom::forward_for` generalizes `forward` for multi-stage chains (`poke` suite count-identical at 157 — the boring-diff proof). Suite `manager` 27 cases / 447 assertions; the Loomstd round-trip + gate-refusal pin lives in the **portable** `weave` suite on purpose (a header claiming portability while only compiling behind `if(NOT WIN32)` is a claim nothing checks — Windows rose 194→**195** cases proving it). WSL Debug 26/26 + ASan/UBSan 26/26, **282 cases / 80977 assertions** each; MinGW portable 19/19, 195 / 80209. **Satellite deferred to 1c by the prompt's own cut-order**: pricing the snapshot opt-out found that `OutOfProcessWeave::snapshot()` serves the host-owned cache, so under `Never` it would silently serve the *handshake* snapshot as current state — a vacuous-green needing a real honesty-lattice decision plus a `containment()` attestation, not a flag. | built |

| **R2A-1 — the activation fact** (`weave/lifecycle.hpp` + the control door + reload's whole-contract check) | **the smallest general lifecycle fact the substrate was missing**: a newly installed code incarnation may opt in to receiving ONE explicit, ordinary, inspectable message after the operation that installed it has successfully committed. `zen.Activated v1 {sequence}` is Loomstd-tier and its power is how little it claims — **not** health, readiness, role ownership, state preservation, a predecessor, gracefulness, resource availability, "start a loop", "repeat prior work", or system readiness; all ten non-meanings are written into the header so a later phase must delete a comment to smuggle one in, and the shape's **one-field minimality is itself pinned** in the portable `weave` suite. **No role field** (a loaded weave may hold none, and payload metadata must never compete with the bus and the live role map); **no cause field** (load/reload/swap/recovery is vocabulary no proven consumer needs yet). **THE OWNERSHIP IS THE ARCHITECTURE.** It lives at the **control door**, not the Manager: `LoadWeave`/`SwapWeave`/`ReloadWeave` are Manager composites, but `LoadLibrary`/`ReloadLibrary` are the primitives that call the Kernel, and a participant holding `load_capability` drives those with **no Manager in the path** — so a Manager-emitted lifecycle fact would be *false architecture*, two callers producing identical kernel changes with only one producing the lifecycle result. Pinned by a direct-door case that bypasses the Manager entirely; the rejected alternative was **built as a mutation and shown red** (relocating emission into `WeaveManager::on(Result)` leaves that case with zero activations). Not the Kernel either — it only *answers* a query (`Kernel::accepts`, reading the Switchboard's published accept-set, no second cache) and never enqueues anything, so no privileged non-message backchannel exists. **Swap has no activation code anywhere**: a successor is activated because it was loaded through the same primitive — the hard and graceful paths both prove it, and `manager.hpp` is **byte-identical to its pre-phase self**. **Participation is declared, never attempted**: opt in by listing the shape in your accepted schemas; the door asks first, and a non-participant costs nothing — no message, **no refusal manufactured to discover non-participation**, no sequence spent. Identity is the pair **(bus-stamped sender, sequence)**, monotonic within a revived lineage and explicitly not globally unique; the sequence lives in `ControlState` (honest **v2** bump — v1 meant `{ops}` forever) and is deliberately NOT `ops`, so it snapshots and revives with the door rather than restarting. **The adjacent repair, required not optional: reload now enforces the WHOLE contract.** `rebind()` swaps the ABI behind the incumbent's adapter and the Switchboard keeps routing by the accept-set from the incumbent's *registration* — so a same-state candidate could change its doors and be routed to by the old contract (false composition truth, and it made activation participation undecidable after a reload). Reload now also requires an **order-independent exact set match** on accepted `(name, version, content_id)`, refused **before rebind** with `accepted schema contract mismatch; reload refused`, incumbent's instance/library/state/WeaveId/role untouched and still serving. **Every new pin was shown red by a product mutation** (10 of them: accepts-always-true; state-only compare; a version bump; drop the load activation; drop the preflight and send blindly; Manager-owned emission; drop the reload activation; allocate the sequence without persisting it; announce before checking the result; reset the sequence on revival). Counts, all accounted: `kernel` 10→12 / 84→113, `manager` 27→35 / 447→598, `weave` 9→10 / 35→44 — **+11 cases / +189 assertions, and no other suite moved**. Green from **fresh** build trees (build outputs are not source truth — the R1 hand-off proved an archived binary can retain a red mutation after the source is restored): WSL Debug 26/26 and ASan/UBSan 26/26, **300 cases / 81232 assertions each, 0 skipped**, enforcement tallies 15 + 11 executed under the delegated scope with the strict gate on; MinGW portable 19/19 at 199 / 80247 (+1/+9 — the Loomstd-tier claim compiled and RUN on Windows, not asserted) and the Windows-kernel opt-in lane 22/22 with `kernel` 12/115 and `manager` 35/598 on real DLLs. **Zengine untouched** (`f4f7da1`, clean) and green through the stranger's path and SDL-off: all six suites, counts unchanged, the Trust Gate probes still pinning today's swap-death / double-wind / latecomer-deafness. **Nothing here fixes swap liveness** — no Zengine weave declares `zen.Activated` yet, so every one of them is a clean non-participant; the Timer authoring a chain from an activation is R2A-2. Still open and named in §3: native-mount activation, cause vocabulary, readiness/health, **reload revive-failure rollback** (validate-then-commit is not transactional — a post-rebind `revive()` failure still leaves the weave unavailable), prepared-candidate/atomic replacement. | built |

The spine holds across every boundary: one gate everywhere, untrusted-until-proven,
immutable published schemas, the kernel holds grammar not answers. The content-id
fast-path is deliberately *untaken* so "one gate, every delivery" stays literally true.
The grant is now projected onto **real** boundaries — the message boundary (B1), the
process boundary (B2), and the syscall/kernel boundary for Network (B3), the filesystem
(B4), and resources (B5). The mechanism ladder is **complete for the threat model**; only
syscalls (seccomp) remain — a **deliberate later decision**, not an assumed phase.
**The guarantee is real but conditional, and the condition is load-bearing:** the OS-enforced
rungs (B3–B5) require an unprivileged user namespace **plus** a delegated cgroup-v2 subtree.
Where the host has them, containment is real and positively confirmed; where it does not, the
host **fails safe — it refuses to mount an untrusted mod** (outside dev-mode, which proceeds
visibly uncontained). B1's in-process boundary is a **cooperative** one, not a cage. "Complete
for the threat model" names the *mechanism* set, not an unconditional promise on every host.

| **Audit response — the honesty-and-containment pass** (2026-07-20 cold-eyes audit; `docs/audits/2026-07-20/`) | **restoring the two promises the project rests on: containment holds, and the system never claims a guarantee it did not deliver.** Closes the confirmed load-bearing findings from an independent cold read; the auditor's own repros became regression gold. **Spine (never cut): F-19** — the sign-off blocker. A manifest's type-token stream is *flat* (bounded by the list cap, NOT the value-depth cap), so a field typed `List<List<…>>` ~100k deep passed the gate then drove `decode_type`'s per-`List` recursion to a **host stack overflow at mount time, before the mod runs** — a SIGSEGV no `try/catch` catches, reachable at three `decode_schema` call sites (out-of-process mount, in-process load, bridge peer; grep-confirmed complete). Fixed with a `kMaxTypeDepth` cap (mirrors `kMaxBinaryDepth=64` — a type nested deeper could only describe values the gate already rejects) **refused on the way down**, so a hostile descriptor is an ordinary `Refused` at every site. The green-is-not-correct lesson, earned again: the existing `test_fuzz` "deep nesting" case built via in-process `make_schema` and **never called `decode_type`** — a vacuous green that named itself as covering schema depth; the new pin exercises the REAL gate→`decode_schema` path and re-runs the auditor's **N=100000** ceiling (was exit 139, now a clean refusal at the exact `kMaxTypeDepth` boundary: 64 decodes, 65 refuses). **F-20** — the honesty-lattice breach: on a `pids`-but-not-`memory` host, `detect_enforcement` reported Resources enforceable and the containment note printed `memory<=…MiB` while `cgroup_create_leaf` **skipped** `memory.max` (controller undelegated) — enforcement claimed, never imposed, the one thing the lattice forbids. Fixed by routing the note through a pure `resource_note(caps, cgroup_memory_available())` that mirrors what a leaf *actually* writes (memory positively named UNCAPPED where undelegated), the same shape the cpu clause already had. **Real cracks: F-6** — `Switchboard::journal_` retained one `DeliveryOutcome` per message **ever enqueued** (a linear leak in lifetime throughput — a bus runs for weeks); now a bounded seq-tagged ring (`kJournalCapacity=1024`), the recent window intact, ancient tickets evicted-to-`Pending` (every consumer reads in the same `submit`→`pump`→`outcome` breath — verified across all consumers, only the console reads the journal in production). **F-8** — grant-record `persist()`'s comment claimed crash-durability the code didn't deliver (`rename` without `fsync`); now a POSIX durable write (temp `fsync`'d before the atomic `rename`, dir `fsync`'d after). **F-1** — the above-floor grant key was FNV-1a (~2^32 collision resistance, keying a security-relevant identity); replaced with an **in-tree SHA-256/128** (`src/detail/sha256.hpp`, no external crypto dep — the dependency question investigated and answered face-up, pinned to NIST vectors incl. the 2-block path), content-addressed AND collision-resistant, a *signed* author identity still the identity phase's job. **F-2/F-7 truth-in-labeling:** "complete for the threat model" qualified with its load-bearing condition (userns + delegated cgroup-v2, else fail-safe-refuse) across README/ledger/DESIGN; the stale `run-under-scope.sh` "WARN-skip" comment corrected to the fail-HARD reality + the ctest delegated-scope precondition documented. Backlog (F-3/4/5/21/22 + the F-2 startup-probe follow-on) recorded so nothing evaporates; **F-23 banked as a positive regression pin** (the bridge verifier did not leak root authority). **Green both lanes:** WSL Debug 26/26 + ASan/UBSan 26/26; MinGW portable re-proven (the `kMaxTypeDepth` cap and journal ring run everywhere — schema_codec 6/25, switchboard 12/41 on Windows; the isolation pins stay Linux-gated), `-Werror` clean throughout. | built |
| **Honesty-completion pass — the pids mirror** (2026-07-21; closes residuals a *memoryless* adversarial re-test raised against the audit-response fixes — `scratchpad/zen-fix-retest-2026-07-20.md`) | **the F-20 fix corrected the memory over-claim and *disclosed* a pids residual — but under-stated its reach; this pass closes the mirror and pins BOTH directions, so the lattice rule can never again be watched in only one.** **Spine (never cut): N-1** — on a **memory-only-delegated** host the substrate made an *absolute* enforcement attestation (`"pids.max ALWAYS bounds a fork-bomb"`, `"confirmed: … limits read back"`, `containment()` → `contained`) while `cgroup_create_leaf` **skipped** `pids.max` (controller undelegated) and a fork bomb ran unbounded — the exact F-20 sin, mirrored onto pids and stated *more* absolutely than the memory clause the fix corrected. Root cause named so it can't recur: **the F-20 pin exercised only the pids-only posture, so the memory-only mirror was never watched.** Fixed by mirroring the memory gate at every site — `resource_note` gains `pids_enforceable` (its `pids<=` clause goes UNCAPPED where `!cgroup_pids_available()`, the same three-way shape memory already had), and a new **pure `resource_attestation()`** helper delegation-qualifies the fork-bomb-stop attestation (retiring the absolute; the confirmed clause no longer implies a pids readback the host never performed), leaving `describe_resolution` one-line wiring. **The pins are the deliverable:** the full **pids×memory posture matrix** (both / pids-only / memory-only / neither) is pinned as pure-function tests — every posture that can reach a resource attestation is now watched to tell the truth or fail, no direction unpinned. **Mirror-symmetry (§2) confirmed:** memory reaches the note via `resource_note(…, cgroup_memory_available(), cgroup_pids_available())` at one call site; the attestation carries memory's truth in the note (as before) and pids' fork-bomb-stop truth in the helper's conditional — both delegation-qualified. The one honest asymmetry (the *fork-bomb* claim is a pids-specific semantic beyond `pids<=N`, so it lives in the attestation, not the note) is recorded, not smoothed. **N-2 (TCB data):** `write_file_synced` opened the grant-ledger temp `0600`, but `open(O_CREAT,0600)` ignores the mode on a pre-existing file and `rename` preserves it — a pre-planted `0666` `.tmp` leaked world-writable into the ledger (re-test observed `666`); now `fchmod(fd,0600)` after open, pinned. **N-4 (comment-truth):** the delivery-journal window-safety comment claimed its guarantee unconditionally, but the ring can roll *within one pump* past `kJournalCapacity`; the comment now names the sufficiency condition (no code change — no consumer at risk today: relay tracks its own pending, console reads one-shot). **N-3 recorded** — `Registry::register_schema` COW is O(N²) on a huge manifest (informational seam, bounded, not memory-unsafety). **Green all three lanes:** WSL Debug 287/81022 + ASan/UBSan 287/81022 (0 failed, 0 skipped, under the delegated scope); MinGW portable 197/80220 (isolation/grant Linux-gated — only a portable switchboard comment is shared); `-Werror` clean. | built |
| **The first hosting consumer — kernel export + the self-contained manifest** (2026-07-22; pulled by Zengine Stage 2, the snake vertical slice — the Loom's first consumer that HOSTS weaves) | **two named triggers fired by one consumer, each answered with the smallest true change.** **(1) `loom::kernel` joins the export.** The export set's own comment had pre-decided this: kernel and isolation "join the export when a hosting consumer appears." Snake hosts `.so` weaves, so zen-kernel rides `loomTargets` (Linux-only — a consumer gates on `if(TARGET loom::kernel)`, which is also the honest question "can this install host weaves?"); the kernel headers ship on every platform (`*.h` matching added for `abi.h` — the C ABI header is deliberately not `.hpp`), and the schema_codec special-case install dissolves into the general rule. Isolation stays home: no out-of-process consumer yet. **(2) `zen.Manifest` v3 — the manifest made self-contained.** A **green-is-not-correct find on the stranger's path, in prose form**: DESIGN.md and schema_codec.hpp's own header both said "the manifest lists referenced schemas first" — and no encoder section ever carried them. Every fixture that had crossed the ABI was FLAT, so the gap was invisible until the first real contract arrived: snake's state nests `Pos` (as `List<Message>` AND a message field), and its load refused with `unresolved nested schema 'Pos'` — the doc over-promised the code, caught by the first consumer that leaned on the promise. v3 adds the optional `referenced` list: the encoder walks accepted+state transitively and emits every nested component in **post-order, deduplicated** (dependencies first — one forward decode pass suffices); `decode_referenced` registers them into the dependency registry before accepted/state, so the **cross-library agreement wall applies to components exactly as to doors** (identical re-registration no-op; conflict = clean load refusal). Flat manifests emit no section — the floor stays lean, and old tests pass byte-comparably. The kernel's reconstruct consumes it; **the isolation host's manifest path deliberately does not yet** (fenced this phase, no nested out-of-process consumer exists; its refusal is clean and the helper is waiting — a named trigger). Pinned in test_schema_codec: the snake-shaped manifest resolves into an EMPTY registry; **the pre-v3 sequence is kept as the negative** (skipping `referenced` reproduces the exact original refusal — the section is load-bearing, not decorative); flat-manifest leanness pinned. Version bumped v2→v3, never mutated (the requests-bump precedent, paid again). **(3) The `-fno-gnu-unique` weave-library law** — the same first consumer found a second latent trap, this one in the toolchain: the maker path (`ZEN_SHAPE`+`WeaveBase` in a `.so`) instantiates loom's inline-template statics (`schema_of<T>()::s`) as `STB_GNU_UNIQUE` symbols, and glibc's program-wide unique table **ignores `RTLD_LOCAL` and can outlive a `dlclose`** — the second library sharing a vocabulary header aliased the first's *destroyed* statics (ASan: use-after-free in `describe()`, freed by the unloaded library's static destructors; without ASan, a SIGSEGV or garbage manifest bytes). The fixtures never saw it in years of swap tests **by accident of discipline**: hand-built schemas in anonymous namespaces have internal linkage, which is never unique — the first REAL maker-style `.so` pair hit it on its first drawer swap. Law recorded in DESIGN (weave libraries compile `-fno-gnu-unique`); pinned by Zengine's three load-unload-load linkage cases; a Loom-native `WeaveBase`-fixture pin is a named follow-on. Green both WSL lanes after the change; the consumer's own green (Zengine's suite driving real nested `.so` weaves end-to-end) is recorded in the Zengine repo — per-repo green discipline. | built |
| **The Windows kernel backend** (2026-07-22; opt-in, development/demo only — the accessibility seam the snake slice pulled: makers on Windows can run the three moments) | **the hook the platform gate always appreciated, pulled by its real trigger — and built to be impossible to mistake for the contained path.** `LOOM_ENABLE_WINDOWS_KERNEL` (default **OFF**; every default build unchanged, pinned by re-running the default MinGW lane) builds `zen-kernel` on Windows over the loader that was dual from birth (`LoadLibrary`/`GetProcAddress`/`FreeLibrary` — the surviving cross-platform wrapper; the phase's code delta is small on purpose: honest `GetLastError` detail in refusals, `__declspec(dllexport)` for the ABI symbol on PE — which also kills MinGW's export-everything auto-export, shrinking a weave DLL's dynamic surface to exactly `zen_weave_abi`, the RTLD_LOCAL spirit — and `Kernel::containment_note()`). **Honesty is structural:** the note is platform-truthful — Windows: `"unisolated; process-level only; no sandbox (Windows development/demo backend — isolation and the OS sandbox are Linux-only)"`; Linux in-process: `"no OS sandbox"` (in-process was never the contained mode — saying so everywhere ends a latent over-read) — **pinned per platform in the kernel suite with negative terms** (the note may never contain "contained"/"sandboxed"), plus a configure-time banner. Isolation, namespaces, cgroups, seccomp, the lattice's enforced rungs: **Linux-only, untouched, and stated as such at every surface** (CMake comment, DESIGN, README). **Test gating moved from platform to capability:** kernel/manager/capabilities suites + the portable fixture set (weave/b/v2/badsnap/badmsg/bequeaths/heir/wedged/badabi) ride `if(TARGET zen-kernel)` and run on the backend as-is — load→swap→reload→graceful-letter all proven on Windows DLLs; probe/bomb fixtures, policy mods, child host, scoped suites stay `NOT WIN32`. The fixtures also now carry the `-fno-gnu-unique` law explicitly (ELF-only genex; PE needs none — no gnu-unique mechanism exists there, a DESIGN-recorded asymmetry). **Named, not papered:** `LoadLibraryA` is ANSI (LoadLibraryW follow-on); MinGW weave DLLs need their runtime DLLs resolvable (toolchain bin on PATH — the 0xc0000139 lesson); the export set now includes the kernel on any platform that built it (`if(TARGET zen-kernel)` install gate). README's stale pre-snake export paragraph trued up in passing. Green: WSL Debug + ASan **289/81043** (the +1 case is the containment pin, running on Linux too); MinGW default-OFF **19 suites, 198/80238 — bit-identical to before, `ninja: no work to do`, no kernel target**; MinGW **ON: 22 suites** (19 + kernel/manager/capabilities) green with the fixtures as DLLs — load→swap→reload and the full graceful-letter ceremony proven on Windows. | built |

---

## 2. The three pillars (design record — partly built)

This section is the **design record** for the three pillars. Where a pillar (or a phase of
one) has since been built, its **Status** line says so and §1 carries the shipped summary;
the prose here is the settled design it was built from. Pillar 1 is **built (B1)**; Pillar 2
is **built (B2 + B3)** — process isolation and the network sandbox both ship, with the
remaining OS-capability primitives (seccomp, cgroups, filesystem) still to come; Pillar 3
is **still wholly design**.
Anything marked *designed, not built* is design the codebase allows — not a line of it written.

### 2.1 The console (the first human-facing Weave)

A Weave woven entirely in the low-ceremony layer plus a thin frontend; it mostly
*spends* what the substrate already banked.

- **Discovery-first.** Browse Weaves → view a Weave's accepted message shapes → fill
  fields → send. Discovery is not a beginner's crutch; it is the single source the
  whole interaction derives from (`list_weaves` → `accepted_schemas` → walk the shape →
  `send`). Knowing the command path *is* knowing the system, because the path names the
  Weave it talks to and the shape it sends.
- **Speed-runnable guided, one path two speeds.** There is one canonical command path
  (weave → message → fields). Guided = walk it slowly with the bus answering each step;
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
  while committed history scrolls up — the readline technique). The weave column shows a
  red "no such weave" the instant you typo, because the engine resolves the partial path
  against the live registry each keystroke. GUI (SDL3 / ImGui) is a *later, separate*
  phase and merely *another frontend* on the same engine.
- **Observer + injector, not reply-receiver.** The console watches the tap (delivered
  payloads and every refusal, legible because routing reasons are separated from gate
  reasons) and injects via `send`/`publish`. It deliberately does **not** receive typed
  replies — a console accepting arbitrary shapes is the first thing that wants a hole in
  the silhouette, which is a capability question, deferred. You inject and watch the
  consequences ripple on the tap.

**Status: Stage 3 built** (the engine + a plain terminal; the dataflow layer; and the UI-as-data
TUI on top — see the Stage 3 paragraph below). The
engine/frontend boundary (above) is realized: `zen-console`'s `ConsoleEngine` is frontend-agnostic
and fully testable with no terminal, returning domain data a throwaway terminal formats as text.
Discovery-first / host-don't-presume hold (it drives shapes it has never seen, via
`weaves`/`describe` off the registry). **The "observer + injector, *not* reply-receiver" bullet above
is now superseded:** that deferred capability question ("a console accepting arbitrary shapes") is
answered — the console *does* receive replies, into an indexed buffer (`m1`,`m2`,…), via
**wildcard-accept** (`AcceptMode::AnyRegistered`): accept-any-*registered*-shape, **gated against the
registry-resolved schema**, an explicit opt-in capability (an unregistered shape still reaches no
one). The console is the **most-granted participant, not an exception** — broad send via the gated
`send_as`, the tap, discovery — each a grant, not a bypass.

**Stage 2 (built) realizes the "one path, two speeds" and the dataflow vision.** The
**speed-runnable assumption ladder** (named → positional → type-directed → prompt-on-ambiguous;
positional fails *as a whole* and falls through; a still-open required field prompts) is now engine
logic, each rung pinned by a frontend-free test; `NeedsInput` is **structured data** (open fields +
unplaced args), not a printed string. **References** (`$mN.field`) are built: a reference is a
**wire** — it reads a scalar `Cell` off an immutable buffered `Value` and routes it into a new
message (output→input by typing), the text-mode prototype of the flowchart crown's wiring; resolution
is in the engine (`resolve_ref`, standalone-tested), only the `$label.field` lexing is in the
terminal. The gate stays the **backstop that lets the ladder guess fearlessly** — a wrong-typed value
is caught (at compose, as the engine knows both types; the gate is the unconditional floor beneath),
never a silent mis-send. Coercion is narrow: a numeric literal widens Int→Float, a reference matches
its resolved type exactly.

**Stage 3 (built) is the capstone: UI-as-data.** The console's OWN interface is now **data** — the
engine library emits a renderer-agnostic **semantic widget tree** (intent + relationship), and the
*same tree* a terminal renderer resolves to box-characters a GUI later resolves to pixels. This
makes "the GUI inherits the engine" structurally supported: the bet is **no geometry member on
`Widget`** (a closed, geometry-free member set — no `x`/`y`/`w`/`h` field to write) **plus a
name-based compile-time tripwire** on ~10 coordinate spellings (`int x` fails to build; `int px`
compiles clean — **defense in depth, not unrepresentability**), so layout happens **only in a
renderer**. The vocabulary is small and general (`VStack`/`HStack`/`Region`; `List`/`Log`/`Text`/
`Field`; an **overflow policy** and a **focus marker**; a **weight** *relative* grow-hint — never an
*absolute* size) but used here only for the console. `Widget` is a single value type with a defaulted
`==`, so the tree is **one value** — headlessly assertable and diffable by region. **One real
renderer + a renderer-agnosticism proof**: the full-screen **termios TUI** (`console_tui.cpp`,
hand-rolled ANSI, no ncurses, no new dep) is the only production renderer and the *only* place cells
exist; a ~50-line test-only `render_outline` walk consumes the same tree to prove it carries no
medium (it ignores `weight`; the TUI resolves `weight` as a relative size in cells).
**Guidance is engine-produced** (`guidance_for(partial)` advances empty→weave→shapes→fields, carried
in the `Field`'s hint). **Message-driven dirty**: the single bus observer (`record_tap`) sets
per-region flags (`buffer` on a reply to the console, `weaves` on death/revival, `tap` on any event),
drained by `take_dirty()` — the retained-mode / Zengine idea, the change-signal being bus messages.
**Symmetric input seam**: renderer-agnostic semantic actions (`FocusNext`/`Activate`/`Edit`/`Submit`/
…) the `ConsoleUi` controller applies; the raw-key→action map is the TUI's only terminal-coupled
code, so a GUI inherits input too. The proofs pass frontend-free (tree structure; no geometry member
via the fence + a structural-`==` test; one-tree-two-consumers; guidance advances; a bus message
drives the buffer pane; a scripted-action TUI smoke incl. the NeedsInput prompt).
Named successors: a **general Weave-emitted-UI protocol** (any Weave emits its own UI tree — the
vocabulary is built general *for* it, validated by the console first), the **GUI renderer**,
**geometric/canvas UIs** (bridge at the sandboxed-Weave fabric level, not semantic rendering), and
the **result-graph buffer** (Stage 2's flat-`mN` seam).

### 2.2 Pillar 1 — Capabilities / grants (the silhouette, made enforceable)

The kernel grants **capabilities, not permissions**, and the default grant is nearly
empty. A Weave's reach into the world is its silhouette: which message shapes it may
send, to which targets, plus the dangerous OS-relevant grants (ask the kernel to load
more code, touch the filesystem, reach the network).

- The bitcoin-miner reframe: arbitrary code is not the danger — *ambient authority* is.
  Capabilities make most bad behavior **unsayable** (a Weave with no network grant cannot
  reach the network over the bus; the only things it can *do* are send granted messages).
- This answers "should the kernel accept messages": **yes** — the kernel exposes a
  control surface as a Weave-like participant (`LoadLibrary` / `ReloadLibrary` /
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
(`CapabilityDenied` before the gate), the trusted-`WeaveBus`-vs-root-`Switchboard`
split, and the kernel's gated load-door all ship and are tested. What is *not* yet
enforced: the grant's **OS-capability flags** — they are recorded on the grant and
shaped for the sandbox, but inert until B3 makes them absolute at the syscall boundary.
So B1 makes the grant real at the message boundary exactly as this design intended,
with its OS-relevant parts deferred — not weakened — to the isolation phases.

### 2.3 Pillar 2 — Isolation, then the sandbox (two phases: **B2 built**, **B3 built**)

> **Refinement settled this stretch.** The original framing (the old heading: "crash-isolation
> and capability-enforcement are *one phase*") folded both into a single out-of-process move.
> Building it clarified they are **two phases**, and splitting them is the honest move: process
> isolation ships real containment *now* (B2), and the OS sandbox that makes "no network"
> absolute is separable, additive work (B3). Conflating them would have forced B2 either to
> over-claim (call a process boundary a sandbox) or to wait on the fiddly syscall work before
> delivering any containment at all.

The instruction-layer gap (a Weave with an empty grant can statically link or `dlopen`
a networking library and call `connect()` directly, outside the kernel's knowledge) has a
**two-step** answer.

**Built as B2 — process isolation (containment, not a sandbox).** Out-of-process, the OS
boundary means a child **cannot touch host memory** and a crash **cannot take the host
down**: the host detects the death, contains it, reloads from a host-owned snapshot bounded
by `max_reloads`, then quarantines. Hosting is a per-Weave mount choice; the bus cannot tell
a hosted Weave from a native one; the grant is still enforced at the **message** boundary
(child output is gated host-side, sender stamped from the connection). What B2 deliberately
does **not** do is enforce the grant's OS-capability flags — it reports its containment
honestly as *"isolated, not sandboxed,"* and the flags stay inert. That honesty is
load-bearing: a process boundary stops a *crash*, not a `connect()`.

**B3 — the OS sandbox (built: the Network primitive + the honesty lattice; filesystem
followed in B4; seccomp / cgroups deferred to B5+).** B3 projects the grant's OS-capability flags onto a real
syscall-level profile applied to the child *before* it loads the `.so`, turning "isolated"
into "isolated **and** sandboxed." The seam was already in place (hosting is out-of-process,
the grant carries the flags, the child is the one place a profile installs), so B3 was
additive — no rework to the gate, the bus, the wire format, or B2's supervisor. It enforces
**Network** first (binary and coarse — no gradient to muddy the lattice) and builds the
permanent **detect → apply → know → refuse-or-proceed** structure the rest plug into:

- Out-of-process, the OS gives instruction-level enforcement the bus can't: a **network
  namespace with no interface** makes `connect()` fail *regardless of what's linked or
  loaded*; seccomp-bpf filters the syscall set; cgroups cap CPU/memory; dropped capabilities
  close privileged ops.
- **The unification:** the grant is one source of truth projected onto whatever
  boundaries the hosting mode provides. In-process it's bus-enforced (partial, advisory
  below the bus). Out-of-process it's *also* syscall-enforced (absolute). So
  crash-isolation (survive a Weave segfault) and capability-*enforcement* (make "no
  network" absolute) are the **same out-of-process move viewed twice**.
- **Mode maps onto trust, and the grant decides the mode.** A first-party Weave you
  compiled → in-process (fast, bus-enforced, vetting is the tool). An untrusted
  mod-woven Weave → out-of-process under an OS sandbox scoped to its grant (slower,
  OS-enforced; the linked-libcurl trick fails because the process has no network).
  Process isolation makes the transitive-vetting problem **moot** for OS-relevant
  capabilities — the sandbox contains the whole process whatever it loads.
- A **full-trust, not-sandboxed, in-process** mode is explicitly wanted: trusted
  first-party native-speed code, bus-enforced only. It is one end of this spectrum — and
  it ships (B1: in-process, the grant enforced at the message boundary). The spectrum now
  has **all three points built**: in-process/bus-enforced (B1),
  out-of-process/isolated-but-not-sandboxed (B2), and out-of-process/OS-sandboxed (B3 — for
  the Network flag now; seccomp/cgroups/filesystem extend this third point). B2 made the
  middle point — isolate for *crash* containment without paying for a full sandbox — a real,
  distinct choice rather than an all-or-nothing jump.
- **Honest limits:** sandbox config is real work (seccomp is fiddly; namespaces are
  robust because coarse). Not every capability has an OS shadow (bus-only grants like "may
  send DamageEvent" stay bus-enforced). Out-of-process costs IPC + serialize-at-the-boundary
  — which is *why* bytes-as-currency was built, and why isolation is per-Weave, not the
  default. And the grant *decision* is irreducible trust: a granted network capability is
  real power. The honest sentence is *"Zen makes a Weave's power minimal-by-default,
  explicit, observable, and — out-of-process — OS-enforced; the trust in a granted
  capability is yours to give, and giving it is real."* Not a sandbox in-process; not
  "arbitrary code made harmless" even out-of-process.

**Status: B2 built; B3 (Network), B4 (Filesystem), B5 (Resources) built — the mechanism
ladder is complete for the threat model** (as a *mechanism* set; the OS-enforced rungs are
environment-conditional — real containment needs userns + delegated cgroup-v2, else the host
fails safe and refuses, or dev-mode proceeds visibly uncontained). B2 (out-of-process hosting +
crash supervision),
B3 (the detection lattice; native `fork`+`unshare` no-interface network namespace; generated
honest `containment()`; default-strict dev-mode override; parent-writes-maps handshake), B4
(the graduated `FsAccess` filesystem capability: a private mount-namespace allow-list view,
confirmed via `/proc/<pid>/ns/mnt`), and B5 (the quantitative `ResourceLimits` capability: a
per-Weave cgroup-v2 leaf with memory/pids caps applied at the sync point, confirmed via
`/proc/<pid>/cgroup` + read-back) all ship and are tested in Debug and under ASan/UBSan — a
child without the Network grant gets `ENETUNREACH` from a real `connect()`, a restricted child
gets `ENOENT`/`EROFS`/`EACCES` on out-of-scope files, a memory bomb is OOM-killed within its
cgroup while a granted one survives the same allocation, all detection branches are covered, and
no path ever claims containment it did not impose. Remaining: **seccomp-bpf** syscall filtering
(a deliberate later decision — kernel-exploit-escape defense, a higher adversary tier), and
macOS/Windows backends.

### 2.4 Pillar 3 — Gated continuous access (the safe shape of shared state)

Driving case: a health display wants current/max HP continuously accurate without
polling the bus every frame.

- **First, the poll model is the problem, not bus speed.** HP changes on damage, not per
  frame — so **push, not pull**: combat publishes `HPChanged{current,max}` when it
  changes, the display caches it and renders the cache at native speed, and most frames
  carry zero bus traffic. Flipping pull → push makes most of the per-frame cost evaporate.
- **For genuinely high-frequency cross-Weave reads** (a transform a dozen systems read
  every frame): a Weave publishes a value into a **typed, read-only slot the runtime
  mediates**, and readers hold a **handle, not a pointer**. The handle yields the current
  value with no per-read message (a deref, not a round-trip); it is schema'd (no
  reinterpreting bytes); and — the property a raw pointer can never have — its lifetime is
  mediated, so when the owner dies or hot-reloads the handle goes **safely invalid** (reads
  return "unavailable") instead of dangling. *(Which component owns the slot — kernel vs
  switchboard — is an unsettled detail; what matters is mediation by something that tracks
  lifetime.)*
- `VarStorage` (the old prototype) is the right *shape* — a handle yielding a current
  value while abstracting the backing. The cross-Weave evolution changes only the backing:
  "gated published slot" instead of a raw pointer or arbitrary function. In-process *within
  one Weave*, `VarStorage` as-is is fine — it's C++, use pointers freely.
- **Decision: no raw pointers across Weaves.** A raw-pointer Weave can't be isolated
  (pointers don't cross a process boundary → forces in-process, forecloses the sandbox),
  can't be safely hot-reloaded (dangles on swap), is invisible to the tap (loses the
  observability that made the miner detectable), and is write-capable unless you're
  perfect. Blessing it would *contract* potential, not expand it — and once a raw-pointer
  mode is marketed as "fast," it becomes the default reach and the ecosystem loses the Zen
  properties through convenience. **Not forbidden**, though: two trusted first-party
  in-process Weaves may share a pointer by mutual agreement like any two objects in one
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

- The **grant** (Pillar 1) is the single source of truth for what a Weave may do.
- The **hosting mode** is decided by trust (Pillar 2): full-trust in-process → fast,
  bus-enforced, vetting is the tool; isolated out-of-process → crash-contained now (B2),
  OS-enforced and scoped to the grant once sandboxed (B3).
- The mode determines which **enforcement boundaries** apply (bus-only vs bus + syscall)
  *and* which **access mechanisms** are even available (Pillar 3): holding a slot handle is
  itself a grant, and an isolated Weave **cannot** hold one (no shared address space) — so
  it gets push-messages instead. Which is a second reason push is the right default and
  slots are the trusted-in-process optimization.

### 2.6 The powerbox — policy that produces the grant (policy phases P1–P2)

The B-series answered *what a grant enforces*; this phase answers *where a grant comes from*,
in the **powerbox / object-capability** shape: a privileged capability is held by a small,
hard-rooted **broker** Weave, and an untrusted mod gets only a send-rule to *talk to* the
broker, never the raw capability. The spine:

- **Advice, not authority.** A mod may *ask* (the manifest's `requests`); the host alone
  decides. A declaration is never a grant — Pillar 1 applied to policy, kept absolute.
- **Floor by default; power is a granted send-rule to a broker.** Unknown → the floor (no
  network, `FsAccess::None`, bounded resources, a send-rule to the broker role). Deltas above
  it come only from a host-written, per-install **grant-record** keyed by `.so` content-hash.
- **Role-addressing.** A send-rule grants `shape → role`; the role resolves to its singleton
  holder at delivery, reload-stable; an unheld role degrades to `NoSuchTarget`, not a hole.
- **Scope by the stamped sender (P1).** The broker namespaces each mod's data by the
  unspoofable sender the host stamps — the whole mod-vs-mod isolation model, resting on the
  existing stamping unchanged.
- **Generalizes (P2).** The same hooks carry a second broker on a different capability: the
  NetworkBroker holds `os_cap::Network` (there is no OS-scoped network — no-interface or the
  whole net), scopes per-destination by its **own software allow-list** (a higher-trust posture,
  honestly reported, not OS-enforced), and a mod reaches it only via a recorded `net` role delta
  — never the floor, and the delta grants the role send-rule, **never `os_cap::Network`** (the
  mod stays OS-network-denied and reaches the net solely through the broker).

**Status: built (P1 + P2).** **P1 (StorageBroker)** — the ask, the floor-factory + grant-record +
host grant-API, role-addressing, the `Op::EmitRole`/`ZenHostApi::send_to_role` seam (sender
stamped host-side, never on the wire), persistent-`WriteScoped` (TCB-only; mods stay `None`), and
the out-of-process StorageBroker (role `"storage"`, keyspace scoped by the stamped sender;
session-scoped, honestly stated). **P2 (NetworkBroker)** — proves the powerbox **general** (a
second broker on a different capability): `GrantDelta` gains a `roles` field; the floor grants
storage to all but **net to none**; the out-of-process NetworkBroker holds full host network,
scopes per-destination by a **software allow-list** (NOT OS), and does raw TCP. Proven end to end
with negative controls — storage: scoping, floor-without-disk, ask-is-not-a-grant,
reload-keeps-state; network: mediation + negative control (a net-denied mod reaches the allowed
listener *only* via the broker — its own `connect()` → ENETUNREACH), allow-list scoping,
floor-denies-net — plus broker-down → `NoSuchTarget`. Named successors: **OS-scoping the net
broker** + **per-mod network policy**; **first-class persistent identity** (the save-file phase);
**authority-transfer**; the **version resolver**. P1/P2 ship reload-in-place, singleton roles only.

---

## 3. Deferred seams (unchanged, recorded so they don't evaporate)

Still deferred-with-intent, per the Level 0 seam-readiness review: migration transform
registry; emit *enforcement* (chokepoint reserved at `Mail`); full schema-as-value beyond
the manifest precursor; multi-threaded dispatch; request/response await; content-id
fast-path (kept untaken on purpose); cross-language libraries; behavioral contracts;
static-struct half of the codegen marriage. The three pillars above now subsume or
sharpen several of these — and four have since **shipped** as B1/B2/B3: capability-gating,
cross-process delivery, crash isolation, and OS-level network sandboxing.

**New seams the remote-operator bridge surfaced (hooked, not built):** *connect-authority B/C* —
the `authorize_connection` chokepoint + the `allow_any()`/`AnyRegistered` registration that consumes it
return full grant today; *B* is a bearer capability token (possession-is-authorization, no identity),
*C* is per-connection graduated/differential grants, **where "Weaver" is born** (the first relation
needing differential persistent authority = the first place authorization needs *authentication* = the
identity phase). **B's forge-trigger, on record:** when model B lands, the *unauthorized-connection*
forge becomes sayable — a connect that fails the token must be proven to register **no** proxy and
process **no** frame. (Under model A, operator proxies are hidden from discovery but *addressable* by
their small-int ids — same-principal, harmless; a cross-principal channel the identity phase must treat
as surface.) *Location-transparent addressing* — contract-addressing pointed at *location* (the same
contract-address resolving to a different host); the one refinement **relocation** pulls so senders
resolve a relocated weave's new location directly. **Relocation's step (d) costs one honest new
runtime primitive, not zero** (read-and-answer finding): no Switchboard operation binds a *replacement
participant behind an existing `WeaveId`* — register mints a fresh id; reload/swap preserve the id but
`admit` against the unchanged state-schema (a divergent shape is gate-refused, never bound) — so a
relocated weave keeping its identity needs either a `WeaveId`-rebind primitive or the addressing seam
above. *The socket-frame `TerminalBackend`* (host-rendered output over the wire) — hooked by the new
`write`/`flush` seam, no consumer yet. *Discovery as a literal bus weave* — the deeper
discovery-as-messages purification (a discovery participant the operator queries by an ordinary gated
message; today host-side discovery is the bridge's own granted direct reads), pulled by nothing yet. *A
neutral `wire.hpp`* sharing the two protocols' framing primitives. **Multi-threaded dispatch stays
explicitly deferred** — the bridge's multiplexer is the *client's* readiness, never the bus's
concurrency; conflating them would be a real error.

**Open after R2A-1 (the activation fact), stated as open rather than implied-done.** Activation is
built for the control door's dynamic load/reload only, and everything below is deliberately NOT built:
*Native-mount activation* — `mount<T>()` weaves are not activated at all (the door never sees them);
the seam is a host-side decision about whether a native mount is even the same kind of event, pulled
by the first native consumer that wants the fact. *Cause vocabulary* — no enum distinguishes load /
reload / swap / recovery; the only proven fact is "a new incarnation committed", and a cause field
waits for a real consumer that cannot act without it. *Readiness / health* — `zen.Activated` is not
`Ready`, `Healthy`, `Started`, or `Running`, and nothing yet says when a weave is fit for domain
traffic. *Reload revive-failure rollback (R2B)* — reload is validate-then-commit, not transactional:
the incumbent's instance is destroyed and the adapter rebound *before* revival is known to have
succeeded, so an identical-manifest candidate whose `revive()` fails still leaves the weave
unavailable. Both pre-commit refusals (state contract, accepted contract) DO preserve the incumbent
and are pinned; this one is the honest remainder. *Prepared-candidate / atomic replacement (R2B)* —
no successor is staged before the incumbent is unloaded, so the swap window and its failed-swap
friction are unchanged. *Accepted-contract evolution* — reload now requires exact door equality, so
changing a weave's accepted messages is replacement's business (or a later explicit
manifest-migration design); reload deliberately does not republish an accept-set. *Sequence
uniqueness* — an activation's identity is the PAIR (bus-stamped sender, sequence); the number is
monotonic within a revived lineage and is not claimed unique across control weaves, hosts, or
histories. A revived control weave continues its lineage; a *fresh equivalent* one necessarily
carries a different stamped sender, because no operation binds a replacement native participant
behind an existing `WeaveId` — the same addressing seam relocation pulls, met from a second
direction. *The first consumer* — **Zengine has not consumed activation** (R2A-2 is where its Timer
authors a beat chain from an activation instead of a one-shot host wind); until then the trust-gate
probes still pin today's swap-death, double-wind, and latecomer-deafness, and **nothing here fixes
swap liveness.**

---

## 4. Resolved: build order → **B** (capability + isolation first)

**Decided.** Route **B** was chosen, and the phases built since are named for it: **B1**
(capabilities), **B2** (isolation), **B3** (the OS sandbox — Network primitive + the honesty
lattice), **B4** (the filesystem primitive — graduated `FsAccess`, mount-namespace allow-list
view), **B5** (the resource primitive — quantitative `ResourceLimits`, cgroup-v2 leaves). The
mechanism ladder is now complete for the threat model (conditionally: the OS-enforced rungs
need userns + delegated cgroup-v2, else the host fails safe and refuses — the guarantee is real
but names its condition); the **policy phases** are underway —
**P1 (the StorageBroker) and P2 (the NetworkBroker) both ship** (§2.6), proving the powerbox
general across two capabilities — and **seccomp** remains a deliberate later decision. The
named successors are first-class persistent identity (save-files), authority-transfer, and the
version resolver.
The demand-loading use
case (gameplay code loading and unloading as a *hot path* — the game's own spawn logic
sending the kernel `LoadLibrary` when an ogre appears) settled it: it makes
kernel-control-over-the-bus the hot path, which forces capability-gating before that surface
is safe to expose. The two options, kept for the record:

- **Option A — console first.** Ship the human instrument sooner; the console manages the
  kernel via a direct, trusted C++ call (both are floor-level), and drives loaded Weaves
  over the bus. Capability + isolation comes after.
- **Option B — capability + isolation layer first.** Build the kernel's message door plus
  the bus's "may you send this to them" gate (and the in-process/out-of-process hosting
  split), with demand-loading as the proving ground; the console is then born *on top* as
  the first grant-holder, in the real model.

**Resolved: B** — the maker's call, made. The console's *value* (discover and drive a
live system) doesn't strictly need kernel-control-as-messages, but the demand-loading case
showed the capability layer is the real spine of everything multi-Weave, and being born
holding a real grant beats retrofitting one. The counter-argument for A was real (the
console feels half-real until "operate the kernel like everything else" is true, and the
human instrument in hand sooner accelerates everything) — but B won, and B1 + B2 + B3 now
ship, so the console (§2.1) will be born *on top*, as the first grant-holder, in the real
model. **B3's first sandbox primitive (Network) now ships**; the remaining isolation work
(seccomp, cgroups, filesystem — §2.3) extends the proven lattice, and the console is the
natural next phase.
