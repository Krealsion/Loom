# Cold-eyes audit — 2026-07-20

An independent cold read of the Loom. This directory preserves the audit's durable
artifacts; the **fix pass** that closed the confirmed findings is recorded in the ledger
(`zen-design-ledger.md`, the "Audit response — the honesty-and-containment pass" row) and
in DESIGN.md.

## What's here

- `lead-handverify.md` — the lead's own hand-verifications (HV-1…HV-7), including the
  independent reproduction of the F-19 (SEC-1) `decode_type` stack overflow and the
  corrected-down exploitability of the F-1 FNV-1a grant key.
- `finder-briefing.md`, `legibility-log.md`, `pass0a-architecture.md`, `pass0b-docdiff.md`
  — the audit's working notes.
- `repros/` — the confirmed reproductions, kept as regression gold:
  - `sec1_repro.cpp`, `deep_manifest.cpp`, `deep_type.cpp` — **F-19**: a flat token
    stream typing `List<List<…>>` N deep passes the gate then overflows the host stack in
    `decode_schema`. Now fixed; the N=100000 ceiling is a committed regression in
    `tests/test_schema_codec.cpp` ("decode_schema refuses a pathologically deep
    type-token stream…").
  - `sec2_probe.cpp` — **F-20**: on a pids-without-memory cgroup base, the containment
    note claimed `memory<=…` while memory ran uncapped. Now fixed; the honesty is pinned
    in `tests/test_isolation.cpp` ("resource note honesty…"). This probe needs a live
    pids-only base to run; the fix's pin is a pure-function test that needs none.
  - `journal_probe.cpp`, `verify_journal.cpp` — **F-6**: the unbounded delivery journal.
    Now a bounded ring; pinned in `tests/test_switchboard.cpp` ("the delivery journal is a
    bounded ring…").
  - `sec5_repro.cpp` — **F-22** (backlog): `cgroup_confirm`'s prefix-ambiguous substring
    match (`zen-weave-1` matches `zen-weave-10`). Latent; kept as a repro for when it's
    anchored to a path-component boundary.
  - `fnv_forge.cpp` — **F-1**: FNV-1a collision work. The key is now SHA-256/128; pinned
    to NIST vectors in `tests/test_policy.cpp` ("so_content_hash is a truncated SHA-256…").

## Building a repro

The repros link against the built core. From WSL, in a single shell:

```sh
cd /mnt/g/programming/cpp/Zen/Loom
g++ -std=c++20 -Iinclude docs/audits/2026-07-20/repros/sec1_repro.cpp build/libloom.a -o /tmp/repro
/tmp/repro 100000   # post-fix: gate ADMITTED, decode_schema refuses cleanly (no SIGSEGV)
```
