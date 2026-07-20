# Legibility log — first contact (Pass 0a, docs quarantined)

Cold audit, 2026-07-20. Two-pass mode ON: this log records confusions/surprises from the first minute, code+tests+build files only. Doc diff comes in Pass 0b.

## Format
- [L#] timestamped-ish sequential entries. Confusions, surprises, wrong paths — even ones later resolved.

## Entries

- [L1] Entry point: the workspace is `Zen/` containing `Loom/` and `Zengine/`. Session opened in `Loom/`. Before reading a single file I know from the brief (not the repo) that Loom is "a C++20 capability-secure message-passing substrate" and Zengine a "seeded consumer". A stranger without the brief would have only directory names: "Loom" and "Zengine" — evocative but not descriptive.
- [L2] Three-way naming split: repo dir `Loom`, C++ namespace `loom::`, include prefix `zen/`, header guards `ZEN_*`. The umbrella `zen/zen.hpp` comment lowercases it as "loom: the ...". A competent stranger will stumble on why `#include <zen/gate.hpp>` yields `loom::admit`. Resolved eventually (Loom is the substrate lib inside the Zen project) but it costs a beat on every first include.
- [L3] Bare milestone vocabulary in code comments with no in-code glossary: `B1`..`B5`, `P1`/`P2`, `Part A/B/3`, `1a/1b`, "the floor", "the powerbox", "the letter", "poke". These are meaningful and consistent once learned, but a cold reader cannot decode them from code alone — they presume the ledger. (e.g. grant.hpp: "enforced out-of-process in B3"; sandbox.hpp: "B4 builds the vocabulary only".)
- [L4] `nucleus.cpp` at the Loom root (13KB, not in CMakeLists' library sources) — a top-level cpp with no obvious build role. Unclear what it is on first contact. FLAG to check whether it's dead/scratch/an entrypoint.
- [L5] Working-tree cruft: `tests/.test_bridge.cpp.swp` (Vim swap file) present in the tree though NOT git-tracked. Signals an editor was mid-file; harmless but untidy for an "acquire-ready" repo.

## Wave 0 results (code-only pass + build + run + negative control)
- Both lanes (`build` plain, `build-san` ASAN+UBSAN) were already built (today). Incremental state clean.
- `build` ctest: 26/26 pass incl. isolation/policy/all under `run-under-scope.sh`. `build-san` ctest: 26/26 pass under ASAN+UBSAN `-fno-sanitize-recover=all` (fuzz suite + sanitized out-of-process children clean).
- Environment is unusually capable: WSL2 kernel 6.6, systemd --user running, **delegated cgroup-v2 scope obtainable**, unprivileged userns allowed. So B3/B4/B5 enforcement ran FOR REAL here (not degraded).
- Isolation honesty mechanism is real and load-bearing: `tests/enforcement_gate.hpp` converts the classic fail-open (`if(!enforceable){WARN;return;}`) into a hard FAIL by default + a positive tally asserting >=12 OS-enforcement cases actually executed. The netns proof is non-vacuous: a real child calling connect() gets `ENETUNREACH` with Network withheld vs `ECONNREFUSED` when granted (test_isolation.cpp:258-300). fs proof uses a real /tmp secret + mount-ns view.
- NEGATIVE CONTROL done (scratch, not committed): a doctest exercising the real gate, with a planted false CHECK that garbage bytes are admitted → harness went RED (exit code 1, "Status: FAILURE!"); reverted to the true CHECK → GREEN (exit 0). The harness genuinely reports failure; the repo greens are evidence, not trust.
