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
naming both versions. The current version is `ZEN_ABI_VERSION`
in `include/zen/kernel/abi.h`; that header owns the version, rather than a
second version number maintained in this law.

MEANS
- the stale-artifact fixture always declares `ZEN_ABI_VERSION - 1`, so the pin
  means "the previous ABI refuses" after every future bump;
- out-of-process children get null capability doors and fail closed rather
  than pretending.

DOES NOT MEAN
- that a version number is observable *within* one self-consistent build — it
  protects mixed artifacts.

PROVEN BY — descriptor version gate; suite `kernel` (stale-ABI case).

## KERN-05 — A weave's statics live and die with it, and its C++ runtime is its own

LAW — A weave built through Loom's supported path receives, automatically, the
platform's requirement for `dlclose` to actually end that image's static
lifetime, and, on Windows, its own C++ runtime, whichever Windows compiler built
it: what runs it is the image itself, never whichever runtime the machine or the
process happens to hold. Both are Loom's to state and apply, never the author's
to remember.

MEANS
- `loom_weave_build_contract(<target>)` is exported with the package and is the
  whole supported path; a consumer never spells a compiler or linker option for
  this;
- it covers a **compilation**, not a file — the installed `loom::core` and
  `loom::switchboard` are built under it too, because one unique symbol
  anywhere in the image marks the whole image `NODELETE`, and on the MSVC ABI
  every object of one image is compiled for one runtime;
- it reaches exactly the target handed to it: a host executable, or any other
  consumer that merely links Loom, is untouched;
- the platform predicate is semantic. ELF/GNU needs the unique-binding option;
  PE-COFF and Mach-O have no such binding and never receive it, notwithstanding
  that MinGW GCC accepts its spelling. On PE-COFF the image carries its C++
  runtime instead: under MinGW-w64 (GCC, or Clang targeting it) a `SHARED` or
  `MODULE` weave links it in (`-static`), and on the MSVC ABI every object of
  the image is compiled for the static runtime (`MSVC_RUNTIME_LIBRARY`);
- on the MSVC ABI that runtime is chosen in each object as it is compiled, so
  Loom's libraries are built for the static runtime, and `loom-weave.cmake` makes
  it the default of the project that includes it (Loom's own build, a project's
  `find_package(loom)`, or the project Loom is built inside) and refuses in
  words a project that names another. That is the package's requirement of an
  image that links its libraries, as its build configuration is, not the contract
  reaching a host;
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
  symbols or imports — this is a build contract, and only a build contract;
- that a program carries its runtime: under MinGW-w64 a host executable keeps
  linking its toolchain's runtime library, and finds it beside itself or on
  `PATH`;
- that anything C++ may cross the seam. Nothing ever could (`abi.h`), and with a
  runtime in each image, an exception or an object crossing it would meet a
  different runtime on the other side.

PROVEN BY — `cmake/loom-weave.cmake`; CTest entry `weave_contract` (reads the
built artifacts, the symbol tables with `nm` on ELF and each Windows image's
imports with objdump or the MSVC linker, over a roll of contracted targets
derived from the function itself, with a deliberately uncontracted twin as the
control); the installed-package witness (a weave built against the installed
package imports no C++ runtime library on Windows); CTest
entry `weave_population` (that the artifacts which must be on that roll are on
it — a separate, build-graph-derived expectation, [POP-05](population-laws.md);
without it, a fixture that stopped calling the function would simply leave the roll
and nothing would say so); suite `kernel` (a contracted image releases and
each load gets a fresh lifetime; the uncontracted one stays resident while
`dlclose` reports success — the loader's bookkeeping, which is *not* what
notices a fixture losing the contract).
