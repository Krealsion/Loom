# Kernel laws (KERN)

Reference: [kernel](../reference/kernel.md) ·
[dynamic-abi](../reference/dynamic-abi.md).

## KERN-01 — Bytes are the boundary currency

LAW — Everything crossing the dynamic-library seam crosses as bytes and is
re-admitted through the one gate host-side before anything routes on it.

MEANS
- no host pointer into library memory, no cross-allocator free
  (`ZenByteSink` ownership);
- a loaded weave's emissions are gated exactly as a native weave's are;
- schemas themselves cross as gated values (the manifest).

DOES NOT MEAN
- that the library is trusted less *semantically* — once admitted, a loaded
  weave is indistinguishable on the bus.

PROVEN BY — `include/zen/kernel/abi.h`, `export.hpp`; suites `kernel`
(malformed-message/snapshot refusals), `capabilities`.

## KERN-02 — One lifetime chain per artifact

LAW — An artifact's instance and library are destroyed/closed exactly once,
whoever unloads in whatever order: the `HostAdapter`'s destructor is the
removal notification, and the shared library handle closes when its last
holder releases.

MEANS
- the Switchboard never calls the Kernel (the dependency is one-directional —
  literally: zero occurrences of "Kernel" in the Switchboard sources);
- a transaction discarding a candidate releases the artifact with no Kernel
  call and no hook;
- the invariant is one-directional: a record never outlives its adapter; an
  adapter a host keeps after unregistration is *detached* — it reaps nothing
  and holds its library mapped (`ArtifactStatus::Unregistered`).

DOES NOT MEAN
- "if and only if" — the converse direction is false and useful.

PROVEN BY — shared `LoadedLibrary`; suite `kernel` (lifetime-delta cases,
namesake-load non-reaping, shutdown exact-once).

## KERN-03 — Role truth is derived, never cached

LAW — Every Kernel role query reads the Switchboard's live table — the same
table routing resolves against. There is no second answer to drift.

MEANS
- a role moved by admission, with no Kernel call anywhere, is reported
  correctly and immediately;
- during a pending admission the Kernel reports the incumbent (truthfully),
  and the candidate only after dispatch.

DOES NOT MEAN
- that the Kernel has no books at all — artifacts (names, libraries,
  statuses) are its truth; *roles* are the bus's.

PROVEN BY — `role_of`/`query_role`/`unload_role` all derive; suite `kernel`
(direct-admission visibility, pending-window truth).

## KERN-04 — The ABI seam refuses loudly

LAW — An artifact built against a different ABI version is refused at load,
naming both versions. Current: **v4**.

MEANS
- the stale-artifact fixture always declares `ZEN_ABI_VERSION - 1`, so the pin
  means "the previous ABI refuses" after every future bump;
- out-of-process children get null capability doors and fail closed rather
  than pretending.

DOES NOT MEAN
- that a version number is observable *within* one self-consistent build — it
  protects mixed artifacts.

PROVEN BY — descriptor version gate; suite `kernel` (stale-ABI case).

## KERN-05 — A reloadable artifact's statics live and die with it

LAW — A weave built through Loom's supported path receives, automatically, the
platform's requirement for `dlclose` to actually end that image's static
lifetime. The requirement is Loom's to state and apply, never the author's to
remember.

MEANS
- `loom_weave_build_contract(<target>)` is exported with the package and is the
  whole supported path; a consumer never spells a compiler option for this;
- it covers a **compilation**, not a file — the installed `loom::core` and
  `loom::switchboard` are built under it too, because one unique symbol
  anywhere in the image marks the whole image `NODELETE`;
- it reaches exactly the target handed to it: a host executable, or any other
  consumer that merely links Loom, is untouched;
- the platform predicate is semantic. ELF/GNU is affected; PE-COFF and Mach-O
  have no such binding and receive nothing, notwithstanding that MinGW GCC
  accepts the ELF option's spelling;
- the verdict is recorded on the target (`LOOM_WEAVE_BUILD_CONTRACT`), and a
  compiler that cannot express the contract is refused, not assumed.

DOES NOT MEAN
- that a `dlopen`'ed weave is isolated, sandboxed, or trusted — the threat tier
  is unchanged and `RTLD_LOCAL` was never containment;
- that every shared library in the process is reload-safe;
- that every `dlclose` hazard is solved: a weave leaving a callback, an
  `atexit` handler, or a thread behind is still its author's problem;
- that Loom reaches a build system that never calls it. It defines a correct
  path; it is not a cage. The uncontracted twin in `tests/` exists to keep that
  boundary visible;
- that anything is checked at load time. Nothing inspects a loaded artifact's
  symbols — this is a build contract, and only a build contract.

PROVEN BY — `cmake/loom-weave.cmake`; CTest entry `weave_contract` (reads the
built artifacts with `nm`, over a roll of contracted targets derived from the
function itself, with a deliberately uncontracted twin as the control); suite
`kernel` (a contracted image releases and each load gets a fresh lifetime; the
uncontracted one stays resident while `dlclose` reports success).
