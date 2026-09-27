# Audits — what a cold read found

An audit reads the substrate as an adversary would, and hand-verifies the security-critical paths
rather than trusting a report. What it confirmed and what changed because of it is below; each
repair is pinned by a test, so the current fact is in the suite named, not on this page. The
audit's working papers and its reproduction programs are in Git (`git log -- docs/audits`).

## The architecture audit, 2026-07-20

| what it found | now | pinned by |
|---|---|---|
| A flat type-token stream nesting `List<List<…>>` deeply enough passed the gate, then overflowed the host's stack in `decode_schema`. | refused at a nesting ceiling | suite `schema_codec` |
| On a cgroup base that delegates `pids` but not `memory`, a weave's containment note claimed a memory cap while memory ran uncapped. | the note names only a cap it can impose | suite `isolation` |
| The delivery journal grew without bound. | a bounded ring: recent outcomes survive, older ones are evicted | suite `switchboard` |
| The grant key was an FNV-1a hash, and collisions could be worked for. | a SHA-256 digest truncated to 128 bits, checked against NIST vectors | suite `policy` |
| `cgroup_confirm` recognises a leaf by substring, so `zen-weave-1` matches a process in `zen-weave-10`. | open | [known seams](../reference/known-seams.md#a-cgroup-leaf-is-confirmed-by-substring) |

## An isolation witness that stopped witnessing in Release

The isolation suite's memory-bomb fixture allocated 200 MiB and wrote it through a pointer that
was never read. At `-O2` GCC removes that allocation and its writes, so the Release child never
crossed its 64 MiB cgroup cap and was never killed or quarantined; the case still failed, which
is how it was noticed. Debug keeps the dead store, so the same source tested a real property in
one build and nothing in the other: **a green Debug lane is not evidence about the optimized
build.** The fixture now writes one byte per page through a `volatile` pointer, so the whole
range is faulted in (`tests/weavelib/test_weave.cpp`). Three things confirmed it: the optimized
binary still calls `malloc` and `sysconf` and keeps the page loop; inside a delegated scope,
`memory.events` counts four OOM kills, the first death and the three revives the reload budget
allows; and restoring the old unused write turns Release red while Debug stays green. No
production isolation mechanism changed.
