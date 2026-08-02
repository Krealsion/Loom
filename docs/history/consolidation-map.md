# R2C-0 consolidation map

The classification of every substantial pre-consolidation documentation surface,
made **before** any rewriting (the phase rule). Statuses: CURRENT GUIDE /
CURRENT REFERENCE / CURRENT LAW / DECISION / HISTORICAL / EVIDENCE / DUPLICATE /
STALE. Every source listed here is preserved unabridged in
[`pre-r2c/`](pre-r2c/) regardless of classification.

## Loom `DESIGN.md` (3,890 lines, frozen at `78d64ea`)

| Section | Status | Current destination | Verified against |
|---|---|---|---|
| The spine (operational invariants) | CURRENT LAW | laws/ (GATE, MSG) + README spine | source + gate/switchboard suites |
| Public surface | CURRENT REFERENCE | reference/values-and-admission.md | headers |
| Schema and content identity | CURRENT REFERENCE | reference/values-and-admission.md | schema suite |
| Value model / ownership | CURRENT REFERENCE | reference/values-and-admission.md | value suite |
| The gate | CURRENT LAW + REFERENCE | laws/admission-laws.md, reference/values-and-admission.md | gate suite |
| Registry threading/immutability | CURRENT REFERENCE | reference/values-and-admission.md | registry suite |
| Serialization | CURRENT REFERENCE | reference/values-and-admission.md (wire) | serialize suite |
| The Switchboard | CURRENT REFERENCE + LAW | reference/messaging.md, laws/messaging-laws.md | switchboard suite |
| The Kernel | CURRENT REFERENCE | reference/kernel.md, reference/dynamic-abi.md | kernel suite |
| The weaving layer | CURRENT GUIDE + REFERENCE | guides/writing-a-weave.md | weave suites, examples/ |
| Capabilities (B1) | CURRENT REFERENCE + LAW | reference/capabilities.md | capabilities suite |
| Isolation B2–B5, powerbox P1–P2 | CURRENT REFERENCE (condensed) + HISTORICAL detail | reference/capabilities.md (summary); full text history-only | isolation/policy suites |
| Console stages 1–3, terminal seam, bridge | HISTORICAL detail; short REFERENCE presence | reference/known-seams.md notes; full text history-only | console/bridge suites |
| Poke weave | HISTORICAL detail + short reference note | history (full); mentioned in reference/messaging.md (poke doors) | poke suite |
| UI-Builder A/B | HISTORICAL | history only | component/pixel suites |
| Weave Manager 1a/1b | CURRENT REFERENCE (legacy note) | reference/lifecycle.md (graceful swap) | manager suite |
| Lifecycle conversation (R2B-1..3d-1, incl. prepared replacement + admission-at-dispatch + first-breath) | CURRENT LAW + REFERENCE + DECISION | laws/{lifecycle,answer-authority,replacement}-laws.md, reference/{lifecycle,prepared-replacement}.md, decisions/ | provenance/kernel suites |
| Future seams | CURRENT REFERENCE | reference/known-seams.md | — |
| Level 0 hardening | HISTORICAL | history only | — |
| Smaller decisions on record | DECISION | decisions/ (absorbed where still live) | — |
| Building | DUPLICATE (of README) | README | — |

## Loom `zen-design-ledger.md` (509 lines, frozen at `78d64ea`)

| Part | Status | Destination |
|---|---|---|
| "proven means a test asserts it" rule | CURRENT LAW (doctrine) | laws/README.md |
| Built table | CURRENT REFERENCE (summary) | reference pages own their subsystems; table itself HISTORICAL |
| Design-record pillars (§2) | HISTORICAL / DECISION | history; live triggers noted in reference/known-seams.md |

## Loom `README.md` (250 lines) — STALE as entry surface

Dense spine + pointers into `DESIGN.md`. Rewritten last as an entry surface;
original frozen.

## Loom `docs/replacing-a-service-safely.md`

CURRENT GUIDE — moved to `guides/replacing-a-service.md` unchanged in content.

## Zengine `README.md` (653 lines, frozen at `f6a4c69`)

| Section | Status | Destination |
|---|---|---|
| Tiers / consuming the Loom / build | CURRENT (entry) | README (rewritten, shorter) |
| `reference/` quarry note | CURRENT (entry) | README |
| Test discipline | CURRENT | README (condensed) |
| `timer/` package (~415 lines) | CURRENT GUIDE + REFERENCE + LAW | docs/guides/{timers,timed-weaves}.md, docs/reference/timer-*.md, docs/laws/timer-laws.md |
| `input/`, `surface/`, `snake/` | CURRENT REFERENCE (kept concise) | README sections retained (own their truth; smaller) |

## Contradictions found (source-of-truth order applied)

1. **"activation is the candidate's first delivery"** (R2B-3 era shorthand,
   multiple places) — NARROWED: preparation conversation reaches the sealed
   candidate first. Current wording: *no production delivery before committed
   activation; activation is the first delivery as a live participant* (PR-08).
2. **`AdmitResult.ok` prose** in pre-R2C text — SUPERSEDED by `scheduled`
   (R2B-3d); frozen text left as-is, current reference states the split.
3. **Ledger pillars written as future design** — several since built (B1–B5,
   P1–P2) with Status lines already correcting them; classified historical,
   no meaning edits.
4. **"restoration allocates nothing"** — already corrected in source (R2B-3d
   errand); frozen texts predating the correction keep their words, reference
   states capacity-not-allocation.
