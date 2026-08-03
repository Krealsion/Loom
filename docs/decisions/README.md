# Decisions — why, on record

ADR-style records for exactly the choices a future maintainer might be tempted
to unmake without knowing what rejected them. Not every phase is a decision;
the full narrative lives in [history](../history/README.md).

| Record | One line |
|---|---|
| [one-gate-at-every-boundary](one-gate-at-every-boundary.md) | one validator, no fast path, authorization kept outside it |
| [lifecycle-authority-is-loom-owned](lifecycle-authority-is-loom-owned.md) | attestation is a host capability, board-relative; a grant is never authority |
| [readiness-is-authenticated-conversation](readiness-is-authenticated-conversation.md) | the candidate answers for itself; host assertion was removed |
| [admission-and-activation-share-one-boundary](admission-and-activation-share-one-boundary.md) | one envelope is both; commit schedules |
| [committed-activation-is-not-answerable](committed-activation-is-not-answerable.md) | first breath is a fact, not an ask |
| [no-rollback-after-committed-production](no-rollback-after-committed-production.md) | failure direction is refuse-before, never unwind-after |
| [office-authorship-is-deliberate](office-authorship-is-deliberate.md) | speaking as an office is an explicit verified act, never inferred from holding |
| [a-claim-is-not-a-message](a-claim-is-not-a-message.md) | Senses earn their category by carrying no causality, no history and no default reach |
| [migration-is-authored-not-inferred](migration-is-authored-not-inferred.md) | supersedes automatic gate migration: an authored transformation before admission, never coercion inside it |
| [Zengine: timer-continuity-carries-remaining-duration](../../../Zengine/docs/decisions/timer-continuity-carries-remaining-duration.md) | durations cross; due times cannot |

Shape of each: context → decision → alternatives considered → why rejected →
consequences → current laws supported → evidence.
