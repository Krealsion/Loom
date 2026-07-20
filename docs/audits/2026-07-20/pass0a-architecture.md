# Pass 0a — architecture as understood from CODE ALONE (no .md opened)

Date: 2026-07-20. This is my picture of Loom before reading any prose doc. Pass 0b will diff it against the docs.

## Naming (surprises)
- Repo dir = `Loom`. C++ namespace = `loom`. Public include prefix = `zen/`. Umbrella comment calls it "loom: the self-describing-value-and-gate foundation." So one component wears three names: **Loom** (repo), **loom::** (namespace), **zen/** (include path + the wider "Zen" project). Deliberate-looking but a genuine first-contact snag.
- Phase vocabulary appears bare in code comments: **B1..B5** (message / process / syscall / filesystem / resource boundaries) and **P2**, **Part A/B**, "1a/1b". These are milestone names with no in-code glossary; a stranger cannot decode them from the code.

## Layered model (bottom → top), from include/zen + src
1. **loom core** (the value+gate foundation)
   - `kind.hpp` — 7 primitive Kinds (Int/Float/Text/Bool/Bytes/Message/List), frozen, append-only.
   - `schema.hpp` — `Schema` (name, version, fields, `ContentId` = 64-bit structural hash). `SchemaBuilder`. `same_identity()` = name+version+content_id.
   - `value.hpp` — `Cell` (variant over the 7 kinds; Message/List behind indirection), `Value` (schema ptr + slots). Construction never validates. `construct_blind()` for runtime-discovered schemas.
   - `admission.hpp` — `Error`/`ErrorKind`, `Admission` (accept XOR reject).
   - `gate.hpp` — **THE gate**: `admit(Value, Schema, Report)`. `diagnose()`. `gate_invocations()` counter exists to *prove* one validator serves all boundaries.
   - `serialize.hpp` — native canonical binary + `Unverified` (parsed-but-not-validated; exposes only the *claim*). `parse()` noexcept. `admit(Unverified, door/registry)` = persistence gate = same validator. `compat::` = JSON debug codec through the same gate. Strict-reject unknown fields.
   - `registry.hpp` — copy-on-write schema store keyed by (name, version). `SchemaConflict` on same-key/different-shape.
2. **switchboard** (in-process message bus)
   - `message.hpp` — `WeaveId` (u64, 0=null), `Message` (payload Value + sender + reply_to + correlation).
   - `bus.hpp` — abstract `Bus` (send / publish / send_to_role). The surface a Weave sees.
   - `grant.hpp` — **`Grant`** capability: send-rules (shape→target / →role / any), `os_cap` (Network/SpawnProcess, hard), `FsAccess` (graduated None→WriteAnywhere), `ResourceLimits`. Default = empty. `permits()`/`permits_role()`.
   - `weave_contract.hpp` — `Weave` (5 virtuals: accepted_schemas/handle/snapshot/policy/revive). Never sees an unvalidated message.
   - `switchboard.hpp` — `Switchboard : Bus`. FIFO single-threaded, non-reentrant `pump()`. **Trust boundary = the inner `WeaveBus`**: a Weave holds only a `Bus&` that stamps its own identity and routes through the *gated* path (`send_as`). `Switchboard::send/publish/send_to_role` are the *ungated root authority* (public, but "held only by the host"). Roles = singleton named slots. Lifecycle: kill/reload(budgeted)/swap_state(unbudgeted). Observers/taps.
3. **weave authoring sugar**
   - `weave/shape.hpp` — `ZEN_SHAPE`/`ZEN_FIELD` macros derive a Schema from a plain struct (byte-identical to hand-built). `to_value`/`from_value`. Access model `ZEN_EXPOSE`/`ZEN_HIDE` (value access only; existence/name/type/tag always visible — "no secret state" floor).
   - `weave/weave.hpp` — `WeaveBase` CRTP: derive accept-set/snapshot/revive/dispatch from types; `Mail` typed send context (sole maker outbound path; reserved emit-gate seam). `mount()` (trusted, emit-default grant) / `mount_granted()` (explicit grant). Built-in **poke** doors (Describe/Read/Write/ResetState) answered by the substrate, `final` so a maker can't lie about structure.
   - `weave/lifecycle.hpp`, `weave/relay.hpp`, `weave/standard_shapes.hpp`, `weave/poke*.hpp`, `weave/standard_shapes.hpp` — std protocol vocabulary (Result/Ack/Refused, PrepareShutdown/Bequest, relay).
4. **kernel** (dynamic library loading)
   - `kernel/abi.h` — the **C ABI** (`ZenWeaveAbi`): create/destroy/describe/snapshot/policy/revive/handle. Only C crosses; every Value crosses as bytes and is re-admitted host-side. `ZEN_ABI_VERSION=1`.
   - `kernel/export.hpp` — `ZEN_EXPORT_WEAVE` generates the ABI + thunks; catches every exception at the seam.
   - `kernel/schema_codec.hpp` — Schema-as-Value meta-schema so schemas cross the ABI as bytes; `CapabilityAsk` (advice, not authority).
   - `kernel/kernel.hpp` — `Kernel`: load/reload_from/unload/unload_role from `.so`. Re-admits everything.
   - `kernel/control.hpp` — `ControlWeave`: operate the kernel BY MESSAGE. `load_capability(control)` = the canonical dangerous grant (target-scoped).
   - `kernel/manager.hpp` — `WeaveManager`: an ordinary weave that orchestrates load/swap/reload/list; graceful-swap "letter/bequest" handoff. NO privilege beyond load_capability. Extremely self-documented about its own security edges.
5. **isolation** (out-of-process containment — the REAL boundary for hostile code)
   - `isolation/sandbox.hpp` — B3/B4/B5 native enforcement: `detect_enforcement()` PROBES what the host can enforce (runs the real unprivileged op in a throwaway child). user+net+mount namespaces, `unshare_isolation`, id-maps, `run_mount_plan` (pivot_root), `child_netns_is_isolated`/`child_mountns_is_isolated` (positive confirmation via distinct /proc ns inode), cgroup-v2 leaves. **"NEVER report enforcement we did not impose."** Fail-safe: unenforceable → refuse unless dev-mode.
   - `isolation/protocol.hpp` — length-prefixed parent↔child wire; bounds-checked `Cursor`; 64MB frame cap.
   - `isolation/channel.hpp` — non-blocking bounded unix-socket channel; over-cap/backlog → failed; EOF → child death.
   - `isolation/grant_record.hpp` — floor + grant deltas. **Identity = `so_content_hash` = FNV-1a 64-bit** ("identity, not authentication"). Storage role floor-wired; net role never floored. `GrantDelta` (network/filesystem/roles).
   - `isolation/host.hpp` — `IsolationHost`: spawn `zen-weave-host` child per weave, proxy that IS a weave, supervise (crash→bounded reload→quarantine). `containment()` reports per-capability truth. Test seams `override_enforcement_for_test` + `force_entry_failure_for_test`.
6. **bridge** (network/remote console) + **console/ui** (TUI/SDL) — consumer surface, less security-central.

## The security thesis (as I read it from code)
- ONE gate guards every boundary (bus, persistence, DLL, IPC). Values are self-describing and re-admitted at every crossing.
- Capability = `Grant`, default empty, host is the only source (no in-band widening). Sender identity is *stamped by the connection/bus*, never read from payload — `send_as`, WeaveBus, EmitRole (no sender on wire).
- In-process (B1) the boundary is COOPERATIVE: a native `.so` loaded in-process can reach around `Grant` (it's just memory). Real containment for hostile code is OUT-OF-PROCESS (B2 process, B3 netns, B4 mount, B5 cgroup). The honesty lattice refuses to *claim* containment it didn't impose.

## Candidate findings already visible (to route to Wave 1)
- **[SEC] FNV-1a grant-record key is forgeable.** `so_content_hash` (grant_record.hpp:79-83) is FNV-1a 64-bit and explicitly "identity, not authentication." A malicious mod can be crafted to collide with a benign, highly-granted mod's content hash and inherit its recorded `GrantDelta` (network/fs/roles). Exploitable in the stated threat model (hosting hostile third-party mods). VERIFY.
- **[SEC/HONESTY] Isolation enforcement may be untested-here / seam-only.** Must confirm the isolation tests positively confirm a real distinct netns inode on a spawned child (not just `override_enforcement_for_test`). And whether B3/B4/B5 run at all under WSL2.
- **[SEC] `same_identity` vs bare `content_id ==`.** Gate/wire/registry use bare `content_id ==` inline (schema.hpp:85 comment). A 64-bit content-id collision where name+version already fixed upstream — is there any door-selection path that compares content_id alone with attacker-controlled (name,version)? Check gate.cpp + serialize.cpp.
- **[QUALITY] In-process `send_as`/`send_to_role`/`publish_as` are public.** Security relies on discipline (Weaves get only WeaveBus). Confirm nothing hands a Weave the concrete Switchboard.
- **[DEBT] Committed working-tree cruft:** `tests/.test_bridge.cpp.swp` present (not git-tracked). Minor.

## Environment facts
- WSL2 Ubuntu-22.04, g++ 11.4.0, cmake 3.22.1, 24 cores. No `bwrap`. No `ninja` in WSL (Windows-side only). Existing `build`/`build-san`/`build-win` present. `tests/run-under-scope.sh` presumably wraps `systemd-run --user --scope` for cgroup delegation.
