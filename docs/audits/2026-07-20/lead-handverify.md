# Lead's own hand-verifications (what I verified myself, and how)

## HV-1: FNV-1a grant-record key — CONFIRMED weak, but exploitability CORRECTED DOWN
- **Path confirmed (static read):** `IsolationHost::mount_mod` (src/isolation/host.cpp:559-607) computes
  `hash = so_content_hash(so_path)` and `delta = grant_record_.lookup(hash)`; `delta.network` →
  `with_os_capabilities(os_cap::Network)`, `delta.filesystem` → `with_filesystem(level)`, roles incl. "net"
  → `allow_to_role(kNetRequest,…,kNetRole)`. So the FNV-1a hash is the SOLE key deciding elevation.
  No signature; the actual bytes are not re-vetted at mount against the vetted build.
- **Hash confirmed (static read):** `so_content_hash` (src/isolation/grant_record.cpp:54-76) is textbook
  FNV-1a-64 over the whole file. The code comment itself says: "This is identity, not authentication:
  it names a build, it does not vouch for it."
- **My PoC attempt + SELF-CORRECTION:** I tried to build a second-preimage collision via GF(2) linear
  algebra. That was WRONG: FNV-1a's `*PRIME mod 2^64` mixes bits through integer carries, so FNV-1a is
  NOT GF(2)-affine and there is no closed-form linear solve. Corrected exploitability: FNV-1a-64 offers
  only ~32-bit birthday collision resistance; a *second*-preimage against a specific target hash costs
  ~2^32 (meet-in-the-middle, large memory) to ~2^64 (naive) — cheap for a determined attacker but NOT a
  5-minute closed form. I did NOT complete a live colliding binary this session (would need ~2^32
  memory/time) — an honest limit. The finding stands on: (1) non-cryptographic hash as the sole
  security-relevant identity key, (2) confirmed sole-key usage in mount_mod, (3) the design's own
  "not authentication" admission. Severity tempered by: work factor (not instant), the exploit also
  needs the forged .so mounted where a colliding hash already carries a delta, and the consent-UX that
  would populate deltas is deferred. Net: **material**, latent, bites when a mod marketplace + hash-keyed
  grants combine (which the vision explicitly promises). Recommendation: swap FNV-1a for a truncated
  cryptographic digest (e.g. SHA-256/128) before any hash-keyed grant persistence ships.

## HV-2: "One gate, every boundary" — CONFIRMED TRUE (static trace)
- Live: `admit(Value, Schema)` (gate.cpp:223) → `detail::validate_into` (gate.cpp:198).
- Persistence: `admit(Unverified, …)` (serialize.cpp:838/848) → `admit_against` → `finish`
  (serialize.cpp:672) → the SAME `detail::validate_into` (serialize.cpp:678). Declared in
  gate_internal.hpp ("no second validator"); bumps `g_gate_calls` so `gate_invocations()` lets a test
  prove both paths hit one function. DLL/IPC re-serialize + re-admit, inheriting the same gate.

## HV-3: "No UB on hostile input" — WELL-SUPPORTED (ran build-san + read fuzz)
- test_fuzz.cpp: ~58k parse+admit+diagnose probes — 4000 random-soup ×5 doors, 4000 valid-header+random
  body ×5, 6000 bit-flip/truncate ×3 seeds — PLUS targeted cases: length-beyond-input (over-read),
  list-count-beyond-cap (over-alloc), 200-deep nesting (stack/depth cap). Green in build-san (ASAN+UBSAN,
  -fno-sanitize-recover). Property checked both ways: admitted ⇒ diagnose() empty; rejected ⇒ errors
  non-empty; parse() noexcept. Caveat: deterministic-seeded (regression corpus, not continuous fuzzing) —
  it re-runs the same inputs, so it guards against regressions but won't surface NEW inputs run-to-run.

## HV-4: Isolation enforcement honesty — CONFIRMED REAL (ran isolation suite here)
- enforcement_gate.hpp converts the classic fail-open (WARN+return) into hard FAIL by default + a >=12
  positive tally; the netns proof is non-vacuous (a real child's connect() → ENETUNREACH withheld vs
  ECONNREFUSED granted, test_isolation.cpp:258-300). This host CAN enforce (userns+delegated cgroup),
  so the guarded cases really executed. The UNCONTAINED warnings are the deliberately dev-mode-forced
  branch tests. Honest caveat for the report: on a host WITHOUT userns/cgroup delegation (hardened CI,
  many containers, the Windows lane) the enforcement is UNAVAILABLE and the design fails-safe-refuses
  (or dev-mode proceeds uncontained). The security guarantee is ENVIRONMENT-CONDITIONAL.

## HV-5: Negative control — DONE (harness genuinely reports red)
- Scratch doctest against the real gate: planted CHECK(garbage admitted) → RED, exit 1, "FAILURE!";
  reverted to CHECK_FALSE → GREEN, exit 0. The repo greens are earned evidence.

## HV-6: Sender stamped by connection, not payload (anti-forgery core) — CONFIRMED NON-VACUOUS
- forge_client.cpp is a REAL malicious out-of-process mod that bypasses the safe `Mail::send_to_role`
  (which zeros reply_to), reaches `mail.bus()`, and forges a wire reply_to pointing at a victim weave.
- test_policy.cpp:347-381 mounts real broker + real attacker + an in-process victim that WOULD accept the
  redirected StorageValue. Asserts: reply lands on the STAMPED sender (attacker.id, from link.id/the
  connection), NOT the forged wire reply_to; victim.handled_names is EMPTY. Guarded by
  ZEN_REQUIRE_ENFORCEABLE(ZEN_FLOOR_CAPS) so it runs under real enforcement. The attack is genuinely
  EXPRESSED and genuinely DEFEATED — the "unsayable attack" discipline is real here. Strong support for
  the whole trust model's "identity is stamped by the connection, never read from the payload" invariant.

## HV-7: SEC-1 (unbounded decode_type recursion) — INDEPENDENTLY REPRODUCED by the lead
- Wrote sec1_repro.cpp from the headers (NOT mirroring the verifier's file): built a schema descriptor
  whose one field's type-token stream is a FLAT list of N List tokens + 1 Int token, serialized, admitted
  against schema_desc_schema(), then decode_schema() (host.cpp reconstruct_and_cache's path).
- Result at default 8MiB stack: N=25000 (50KB) -> GATE ADMITTED, decode_schema returns OK; N=100000
  (200KB, << 64MiB frame cap) -> GATE ADMITTED, then exit 139 (SIGSEGV) in decode_schema. Stack
  sensitivity at N=40000: ulimit -s 2048 -> SIGSEGV, 8192/65536 -> OK. Linear threshold => genuine stack
  overflow, not a logic bug. The gate admits the payload BEFORE the crash (printed), so the crash is in
  host-side reconstruction, unreachable by the try/catch(std::exception&) at host.cpp:790 (SIGSEGV is not
  an exception). CONFIRMED sign-off-blocking: an untrusted mod's manifest crashes the trusted host at
  MOUNT time, before the mod runs a message — inverting "an out-of-process weave is crash-contained".
  Same unguarded decode at kernel.cpp:302,306 (in-process load) and remote_console.cpp:133 (bridge peer).
