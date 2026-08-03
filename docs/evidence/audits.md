# Audits — the dated records

Audits are point-in-time examinations; their findings that changed the system
are ratified in commits and reflected in current reference/laws. The artifacts
stay where they were made.

## 2026-07-20 — the architecture audit (in-repo)

[`docs/audits/2026-07-20/`](../audits/2026-07-20/): finder briefing, lead
hand-verification, doc-diff pass, legibility log, and runnable repros. Its
verified claims fed the reference pages; its style (hand-verify the
security-critical paths, never trust the report) is standing doctrine.

## 2026-07-26 — the trust gate (operator workspace, out-of-repo)

A full-repo trust audit whose report and backups live **outside the
repositories** in the operator workspace (`Zen/zen-trust-gate-report.md`,
`Zen/audit-bundles/*.bundle` — verified, test-restored). Recorded here because
a fresh clone will not contain them. What it changed *is* in-tree: the R1
repairs are ratified on Zengine `main`, the timer-survival over-claim it
measured false is corrected everywhere current, and its two process rules —
push at phase end; state which repo's green was proven — are now house
discipline.

## 2026-08-03 — STF-0: the OOM witness that stopped witnessing

**Found by R2E-0a's verification pass; repaired by STF-0.** The isolation suite's
memory-bomb fixture allocated 200 MiB and `memset` it into a pointer never read
and never freed. That is dead code: at `-O2` GCC deletes the `malloc` and the
`memset` outright. So the **Release** child never grew, never crossed its 64 MiB
cgroup cap, was never OOM-killed and was never quarantined — the case had stopped
testing anything while still failing loudly, which is the only reason it was
noticed at all.

**Debug passing did not prove Release containment.** Debug keeps the dead store,
so the same source tested a real property on one lane and nothing on the other.
A green Debug lane was never evidence about the optimized build.

The repair is test-only. The pressure is now made observable: stores go through a
`volatile` pointer, and there is **one store per page** (`sysconf(_SC_PAGESIZE)`
stride) so the whole range is faulted in — a single volatile write is equally
un-removable and equally useless. Both bomb sites were repaired, `handle()` and
`revive()`, because quarantine is reached by dying repeatedly until `max_reloads`
runs out.

Confirmed rather than assumed, in three independent ways:

- **the optimized binary** still calls `malloc` and `sysconf`, and the page loop
  survives `-O2` in all four inlined copies — `movb $0x1,(%rcx)` per iteration,
  bound `cmp $0xc7fffff` (= 200 MiB − 1, the full range);
- **the kernel's own books**, inside a delegated scope: `memory.events` `oom_kill`
  goes **0 → 4**, and 4 is exactly the initial death plus the three revives the
  reload budget allows before quarantine;
- **the mutation**: restoring the old unused `malloc`+`memset` turns Release red
  and leaves Debug green — reproducing the original asymmetry — and drops
  `sysconf@plt` from 5 to 0 in the optimized artifact.

**No production isolation mechanism changed.** Release returned to 30/30, with
`isolation` and the aggregate `all` both moving red → green.

## Mutation campaigns

Every R2-arc phase ran a mutation campaign against its own tests (canary
hand-proven first; whole binary; masked-vs-hole reported). Their summaries
live in the phase records ([history](../history/README.md)) and the marathon's
in `marathon/FINAL-REPORT.md` at [Night Lab](night-lab.md).
