# History — why Zen became this way

Everything here is **frozen record**, not current documentation. It exists so that
current docs may be aggressive about clarity without destroying the reasoning —
the experiments, intermediate laws and rejected designs — that produced the
system. Nothing in this directory is normative unless a current law explicitly
cites it.

- **Current behavior** → [`../reference/`](../reference/)
- **Authoring** → [`../guides/`](../guides/)
- **Concise invariants** → [`../laws/`](../laws/)

## The frozen manuscripts

| File | What it is | Frozen at |
|---|---|---|
| [`pre-r2c/DESIGN.md`](pre-r2c/DESIGN.md) | the complete pre-consolidation design manuscript (~3,900 lines): every subsystem's rationale, alternatives, and phase-by-phase reasoning | Loom `78d64ea` |
| [`pre-r2c/zen-design-ledger.md`](pre-r2c/zen-design-ledger.md) | the built/designed/open ledger — what shipped vs. what was only permitted architecture | Loom `78d64ea` |
| [`pre-r2c/README.md`](pre-r2c/README.md) | the pre-consolidation repo README | Loom `78d64ea` |

`zen-vision.md` (the repo root) is **not** history — it is the project's soul
document, canonical and current, kept in the author's own words.

## Phase chronology (the R2 arc)

Phase names below are historical coordinates. Current laws deliberately do not
carry them; the law files cite the phases only as rationale.

| Phase | What it established | Commit(s) | Survives as |
|---|---|---|---|
| R2A-1 (+1a) | `zen.Activated v1{sequence}` — one narrow lifecycle fact, owned by the control door, participation declared never attempted, finite sequence refuses rather than wraps | Loom `b1148a2` | LIFE-01..03 |
| R2A-2/3, R2B-0 | Timer authors its beat chain from activation; death is universal, inheritance is authored; the handoff letter carries **remaining duration**, never due times | Zengine `58d662c` | TIMER-01..05 |
| R2B-1 (+1a,1b) | provenance is a **delivery fact** (no wire form); answer authority IS the delivery; lifecycle authority is minted host-side and is **board-relative** | Loom `ea8cdb1` | ANS-01, LIFE-04 |
| R2B-2 (+2a,2b,2c) | the deferred answer (bounded, a *conversion* of the immediate right); death ends conversations; a message belongs to a **life**; an answer belongs to the life that asked | Loom `10474e2`,`d521f02`,`fcf1ce0`,`50bf034` | ANS-02..05, MSG-03 |
| R2B-3a, 3b(1) | the candidate **seal** (no public standing; routing paths themselves refuse); admission seals the incumbent and puts activation ahead of candidate-reachable traffic | Loom `982f9a7`,`ab3bc04` | PR-01, PR-05 |
| R2B-3b-1a | admission verifies the exact sealed owner; dynamic `mail.answer()` parity (**ABI 3→4**) | Loom `98066f9` | PR-03, ANS-06 |
| R2B-3b-2 (+2a) | the bounded replacement transaction, in the Switchboard; exact-once terminalization by **ordering**; one candidate → one transaction | Loom `fe371b0` | PR-02, PR-06 |
| R2B-3b-3 | authenticated readiness: `mark_candidate_ready` withdrawn; the ask's own envelope identity decides, never the correlation | Loom `6683dc8`,`72f5072` | PR-04 |
| R2B-3b-3a | the Kernel's books follow reality: the adapter's destructor is the removal notification; role truth derived, cache deleted | Loom `338c686`..`27815f3` | KERN-02..03 |
| R2B-3c | a live Timer crosses a prepared replacement; the boundary IS the admission; found the commit-then-refuse-activation defect | Zengine `332f9e9`,`d78e7da` | TIMER-04..05 |
| R2B-3d (+3d-1) | admission and activation became **one envelope**; a committed activation is not a send and is **not answerable**; `AdmissionPending` | Loom `1447b4b`..`d42b4b7` | PR-07..09, LIFE-05 |
| R2B-4a | `loom::PreparedReplacement` — the host authoring handle, pure delegation | Loom `b4bbd39`,`78d64ea` | the [replacing-a-service guide](../guides/replacing-a-service.md) |
| Night Lab marathon | six applications as **evidence**: 178 facade ops / 0 raw; role-authored provenance (5 sightings, holding ≠ speaking-as); continuity expressed as non-mutating description *snapshots* (staging varied; never an atomic handoff) | night-lab `bf09f79` | [../evidence/night-lab.md](../evidence/night-lab.md) |
| R2D-0 | **role-authored provenance**: identity existed, role destination existed, role membership existed — office *authorship* did not. The weaker "sender held R" fact was rejected (the same holder speaks personally); the law became explicit intent + membership verified at the authorship moment + immutable delivery provenance, publications first-class, refusal (`RoleAuthorshipDenied`) never a downgrade. Not a grant, not a current-role lookup, not payload, not lifecycle authority, not an answer, not destination routing. **ABI v5** earned: v4 could neither carry the fact inbound nor request authorship outbound, and partial parity is the silent same-word-two-meanings failure v4 itself closed for `answer()`. Lobby / build-farm / download-manager replays closed the seam in the follow-up | Loom `6a23e3d`,`30eab0a` | [MSG-07](../laws/messaging-laws.md), [MSG-04/06 sharpened](../laws/messaging-laws.md), [the decision](../decisions/office-authorship-is-deliberate.md), [dynamic-abi v5](../reference/dynamic-abi.md) |

| R2E-0 | **what I know, what I become.** Three closures and one proof-of-no-API. (1) **Senses**: a deliberate immutable *latest claim*, visible at the claim call — the smallest settlement rule, and indistinguishable from handler-completion because dispatch is non-reentrant. By value (no alias into a claimant), two key spaces so personal ≠ office is structural, stale office claims *stamped* rather than withheld (returning nothing would collapse "never claimed" and "the predecessor's"), a new default-absent **observe rule** so no weave gained reach, `Claims<...>` registering at mount for discovery-before-first-claim, and a repository bounded by current keys. **ABI v6** earned: a host-only Sense is the feature not existing. (2) **Authored handoff**: proven to need **no Loom API** — prepared replacement, FIFO, `NotAccepted` and an ordinary weave already supply every piece; migration is an authored transformation *before* admission, never coercion inside the gate; the namespace witness runs carried and deliberately-not-carried. (3) **MSG-08**: the Night Lab III silent dynamic seam closed with one Loom-owned refusal fact and no delivery futures. (4) **MSG-09**: `pump_bounded` earned from the Rule Garden's fake yield message; `pump()` untouched | Loom `101ebb9`,`7e189a0`,`ae9c0e6`,`c281985` | [SENSE-01..05](../laws/sense-laws.md), [HANDOFF-01..03](../laws/handoff-laws.md), [MSG-08/09](../laws/messaging-laws.md), [a-claim-is-not-a-message](../decisions/a-claim-is-not-a-message.md), [migration-is-authored-not-inferred](../decisions/migration-is-authored-not-inferred.md), [dynamic-abi v6](../reference/dynamic-abi.md) |

Notable ideas **rejected or superseded** along the way live in
[`../decisions/`](../decisions/) — each records why, so it is not re-litigated
by accident.

## Older records

- [`../audits/2026-07-20/`](../audits/2026-07-20/) — the dated architecture
  audit (finder briefing, hand-verification, doc-diff, legibility log, repros).
- The 2026-07-26 trust-gate audit and its bundles live **outside the repos** in
  the operator workspace (`Zen/zen-trust-gate-report.md`, `Zen/audit-bundles/`);
  its repairs were ratified onto Zengine `main` (R1). Recorded here so the
  pointer survives even though the artifact is not in-tree.
