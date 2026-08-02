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

## Mutation campaigns

Every R2-arc phase ran a mutation campaign against its own tests (canary
hand-proven first; whole binary; masked-vs-hole reported). Their summaries
live in the phase records ([history](../history/README.md)) and the marathon's
in `marathon/FINAL-REPORT.md` at [Night Lab](night-lab.md).
