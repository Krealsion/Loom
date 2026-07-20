# Pass 0b — docs admitted, diffed against the code-derived picture

The docs are unusually ACCURATE and self-aware. My Pass 0a architecture picture matched the docs on
essentially every structural point (layering, one-gate, capability model, isolation phases, trust
boundary). This is rare. The disagreements are small and mostly about emphasis/legibility, not lies.

## Doc claims I SPOT-VERIFIED against code/tests (agreement)
- README end-to-end example: ran `build/examples/quickstart` → output matches README BYTE-FOR-BYTE
  incl. `content_id 0xb1d69bad13ae83d6`, "34 bytes, magic 'ZN' v1", the exact corrupt-refusal message. TRUE.
- "One gate, every boundary": traced live + persistence admit paths to the same `detail::validate_into`
  (gate.cpp:198, serialize.cpp:678). TRUE (HV-2).
- "No UB on hostile input / never crashes": build-san (ASAN+UBSAN, -fno-sanitize-recover) 26/26 green
  incl. fuzz suite + sanitized out-of-process children. Strong support (pending finder look at fuzz depth).
- Ledger "proven means a test asserts it" + enforcement-gate hard-FAIL mechanism: verified real
  (enforcement_gate.hpp; netns ENETUNREACH proof non-vacuous). TRUE and admirable.
- B1-B5 "built": isolation/policy suites run REAL OS enforcement here and pass; the ledger's per-phase
  test-count claims are internally consistent. Believable (finders to spot-check specific pins).
- Exported CMake surface = loom::core + switchboard only: confirmed in CMakeLists (LOOM_INSTALL block).

## Doc/code disagreements & doc-honesty notes (candidate findings)
- [MINOR/doc] `.clang-tidy` is committed but DESIGN.md:2686 admits it is NEVER run (no clang-tidy in
  WSL). A committed lint config that CI does not enforce is aspirational; a stranger may assume it runs.
- [OBS/doc] The ledger + DESIGN say the mechanism ladder is "complete for the threat model; only seccomp
  remains, a deliberate later decision." But behavioral contracts (DESIGN 2524) and first-class persistent
  identity (P1 is session-scoped only) are ALSO real gaps for the "hostile mod" stake — the ladder is
  complete for RESOURCE/REACH containment, not for IDENTITY/BEHAVIOR. Not a lie (both are disclosed
  elsewhere), but the "complete" framing over-reaches if read in isolation.
- [OBS/doc] so_content_hash + schema ContentId BOTH use FNV (non-cryptographic). Ledger calls the schema
  content-id an "integrity/drift check" (mitigated by name+version pairing — verified in hardening review
  2604-2631). The MOD-identity FNV (so_content_hash) is the one with teeth (HV-1). Docs are internally
  honest ("identity, not authentication") but do not connect that admission to the grant-theft consequence.

## Legibility (the stranger's-eyes audit) — carried in legibility-log.md
- Naming (Loom/loom/zen), bare milestone vocab (B1..B5/P1/P2/poke/letter/floor/powerbox), nucleus.cpp.
- BUT: DESIGN.md + the ledger DO decode all of this for a patient reader. The gap is that the CODE alone
  doesn't; a stranger MUST read the ledger/DESIGN to parse the code comments. That is acceptable IF the
  docs are treated as required reading — which the repo does not say explicitly at the code-entry points.
- The vision doc (zen-vision.md) is a manifesto promising far more than exists (flowchart-to-program,
  marketplace, teaching layer, live mid-game editing). Inspiring, but a stranger could mistake vision for
  status. The ledger is the corrective (Built vs Designed vs Deferred) — but vision links to nothing.
