# AGENTS.md — Loom

Machine context router: **`docs/CONTEXT.md`** (topics → reference → laws →
tests). Human docs: `docs/README.md`. Normative truth is `docs/reference/` +
`docs/laws/` only; `docs/history/` is frozen record; `docs/evidence/` is not
the API contract.

## Build / test (canonical: WSL Ubuntu, GCC ≥ 11.4, cmake ≥ 3.22)

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)" && ctest --test-dir build
# sanitizer lane: same with -B build-san -DZEN_SANITIZE=ON
```

- `-Werror` is on; C++20; avoid GCC-12+-only features.
- The isolation/policy/all suites need a delegated cgroup scope: run the whole
  binary via `tests/run-under-scope.sh ./build/tests/zen-tests` — a bare run
  fails ~33 enforcement cases **by design** (fail-hard beats silent skip).
- Windows/MinGW builds the portable subset only; kernel lanes are opt-in.

## Ownership

This repo owns substrate truth. `../Zengine` owns package truth (Timer etc.)
and consumes Loom as an installed package (`find_package(loom)`). Night Lab
(github.com/Krealsion/zen-night-lab) is read-only evidence.

## Do not assume

- A transaction id, a correlation, or a payload field is ever authority.
- `Committed` at commit-call time — commit *schedules*; `AdmissionPending` is
  real (PR-07).
- Prepared replacement preserves incumbent state — it does not (PR-09).
- `send_to_role` says anything about the sender's office (MSG-04) — office
  speech is its own explicit, verified act (`mail.as_role(...)`, MSG-07), and
  merely holding a role attaches nothing.
- A committed activation is answerable (LIFE-05).
- Green means correct — "proven" means a regression test asserts it; check
  case *counts*, not just pass/fail.
- Docs claims over source: when they disagree, current source + tests win;
  record the contradiction rather than silently fixing prose.
