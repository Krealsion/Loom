# Kernel — reference

Dynamic weaves: loading native libraries as bus participants across a true C
ABI. Laws: [KERN-01..05](../laws/kernel-laws.md). ABI detail:
[dynamic-abi](dynamic-abi.md). Guide:
[dynamic-weaves](../guides/dynamic-weaves.md).

## Loading

`Kernel::load(name, path[, role])` — open the library, fetch and version-check
the descriptor, construct the instance, reconstruct its manifest (`zen.Manifest`
v5: the nested components it `referenced`, its accept-set, its state schema, its
optional `requests`, `claims` and `emits`, all crossing as gated values and all
claimed through the agreement wall — a divergent or self-contradictory
declaration is a refused load, with the shape named), register a **host
adapter** on the bus (optionally bound to a role). The adapter *is* a `Weave`;
on the bus a loaded weave is indistinguishable from a native one, declaring the
same four lists a native one declares. Grants: the admission policy's verdict,
or the grant a four-argument `load` named at its call site
([admitting a loaded artifact](capabilities.md#admitting-a-loaded-artifact));
nothing in the manifest becomes authority.

`load_candidate(name, path, coordinator)` — the ordinary load **then the
seal**: every artifact-level refusal happens before the live world is touched,
and the artifact that prepared is the artifact that goes live
([PR-01](../laws/replacement-laws.md)).

## Unloading and lifetime

`unload(name)` / `unload_role(role)` (selects the **live** holder) /
destruction of the Kernel unload everything it still holds. The lifetime chain
is exact-once by construction ([KERN-02](../laws/kernel-laws.md)): the record
and the adapter share the `LoadedLibrary`; the adapter's destructor destroys
the instance and releases its share — so a candidate discarded deep inside a
transaction releases its artifact with no Kernel call. An adapter a host keeps
after `unregister_weave` is detached (`ArtifactStatus::Unregistered`), reaps
nothing, holds its library mapped.

`ArtifactStatus`: `NotLoaded / Live / Sealed / Dead / Unregistered` —
aliveness outranks the seal (a dead sealed candidate reports `Dead`).

## Reload

`reload_from(name, new_path)` — validate-then-commit hot reload behind the
stable id: snapshot host-side, open and reconstruct the candidate, require an
**exact accepted-contract match** (order-independent `(name, version,
content_id)` set) and state-schema compatibility, refuse before touching the
incumbent otherwise, then rebind and `swap_state`. Evolving a contract is
replacement's business, never reload's.

## Role truth

`role_of` / `query_role` / `weave_id` derive from the Switchboard's live
tables ([KERN-03](../laws/kernel-laws.md)) — there is no kernel-side role
cache to drift, so a role moved by admission (no Kernel call anywhere) is
reported correctly at once, including truthfully-unchanged during
`AdmissionPending`.

## Platforms

Reference platform: Linux, including under WSL (`dlopen`). `LOOM_ENABLE_WINDOWS_KERNEL` is an opt-in
**development/demo** `LoadLibrary` backend with no isolation, truth-pinned at
every surface (`containment_note()`); never a default.

### Opening a weave on Windows

The Windows backend opens a weave by its full path: the path it is given, read in the
program's code page (UTF-8 in every program Loom builds, so a path in any script opens
there; [tools](../guides/tools.md#platform-differences-that-will-actually-bite-you)), and a
relative one completed against the current folder, as the admission policy's read of the
same file completes it. The libraries the weave needs are looked for **beside the weave**,
in the host program's folder, in the system folder and in any folder the host itself added
(`AddDllDirectory`); **never in the current folder and never on `PATH`**. A weave's own
libraries therefore go beside it. A library the process has already loaded is used by its
name, wherever it came from. Loom sets nothing process-wide to arrange this; each load asks
for it.

When Windows refuses a load, the refusal says what it can, after `open failed: `: that no
file is at the path; which library the weave needs is in none of those folders, through
which of its own libraries when it is a library's library; that the file is not a Windows
library, or is one built for another machine; and otherwise Windows' error code with its
own words. The explanation is worked out only after the refusal, from the files the load
looked at; nothing is read before a load or after one that succeeds.

Whether a refused image may show the system's error dialog is the host's to decide, not
Loom's. Nothing in Loom sets the process's error mode or a thread's, so a load runs under
the modes the host gave its process and the thread that calls `LoadLibraryExW`. With
`SEM_FAILCRITICALERRORS` set the system shows no dialog for the error and hands it straight
back to the load; without it, Windows may show one, and the load waits until somebody
closes it. The mode decides whether a person is asked before the load fails, not whether it
fails: either way the load returns the same failure, and the refusal above is worked out
from it. A host with nobody to answer a dialog adds the flag before its first load and
keeps the flags it already has: process-wide with `SetErrorMode(GetErrorMode() |
SEM_FAILCRITICALERRORS)`, or for one thread with `SetThreadErrorMode(GetThreadErrorMode() |
SEM_FAILCRITICALERRORS, nullptr)`, on every thread that loads. Loom's own test program sets
it process-wide when it starts, as a host ([loading a
weave](../guides/dynamic-weaves.md#load-it)).

## A weave built as a WebAssembly module

`LOOM_ENABLE_WASM` (off by default; `LOOM_WASMTIME_DIR` names the Wasmtime C API it links,
pinned in [tools](../guides/tools.md#the-webassembly-backends-two-pins)) builds a third kind of
image beside a shared library: a weave compiled as a wasm32 module, run by Wasmtime inside the
host's process (`src/kernel/wasm_box.hpp`). `load`, `load_candidate` and `reload_from` open a
file whose first bytes are WebAssembly's magic as a module and anything else as a library;
the module is driven through the same `ZenWeaveAbi` a library hands the kernel, so the host
adapter, admission (the content id is the `.wasm`'s own), the manifest, `LifecycleAdoption`
and reload are the ones above. A kernel built without the option refuses a module at `open`,
saying it is a WebAssembly module and that `LOOM_ENABLE_WASM` is off; it never hands one to
the platform's loader.

**Building one.** `loom_wasm_weave(<target> OUTPUT <file.wasm> SOURCES ...)`
(`cmake/loom-wasm.cmake`, with the wasi-sdk `LOOM_WASI_SDK` names) compiles the weave's own
sources unchanged, Loom's core and switchboard, and the module glue
(`cmake/loom-wasm-glue.cpp`), which turns the weave's descriptor into exports and the host
table into imports. One module file serves every platform's kernel.

**What a module may import, and what it is answered.** One `zen_host` import per `ZenHostApi`
slot, and the host's byte sink: each checks every span and name the module hands it against
the module's own memory before a byte is read, and traps the instance on one that does not fit.
Outside a delivery every door refuses. The WASI calls wasi-libc makes are answered without
reaching the world: an empty environment and argument list, no clock (`ENOSYS`), no descriptor
and no preopened directory (`EBADF`), standard output and error kept to `kBoxLogBytes` an
entry and said as one line on the host's standard error, and `proc_exit` a trap. **Every other
import is refused at `open`, named, before anything of the module is instantiated**; no WASI
implementation is linked.

**The version, before the weave is made.** Opening a module makes one throwaway instance —
its static initialisation runs, as a library's does when it is opened — to read the ABI
version its weave was built against; another version is refused at `open`, naming both, before
any slot of the weave runs (KERN-04).

**The grant, at the mount.** A box carries the grant's send and observe rules as any image
does, and its memory from `ResourceLimits`: `memory_bytes`, the backend's default
(`kBoxDefaultMemoryBytes`) when it is 0, and wasm32's own ceiling under `unlimited_memory`. A
box reaches nothing but the host's doors, so a grant giving an OS capability, a filesystem
level other than `None`, or a cpu weight is **refused at the mount, in words naming what it
gave, and never narrowed**; a `pids` ceiling holds, since a box starts no process. The mount
runs after the `Speak` question and before registration; a reload mounts its new image under
the incumbent's baseline grant, which the reload keeps, so a box that cannot carry it refuses
with the incumbent untouched.

**Bounds and supervision.** Each entry into a module may take `kBoxStepsPerEntry` steps
(Wasmtime's fuel); reaching it traps the instance. A trap during a delivery ends that delivery
as `HandlerFailed`; the box then makes a fresh instance and revives it from the state it held
after the last entry that completed, while the weave's own `LifecyclePolicy::max_reloads`
lasts, and quarantines it after: every slot then fails, and `snapshot` still says the last good
state, so a reload can carry the weave out. A trapped instance's memory is never read again.
Wasmtime runs with explicit checks rather than signal handlers, so the backend installs no
handler in the host's process.

**Lifetime.** One engine and one linker serve the process and are never torn down. A compiled
module is shared by every image opened from the same bytes (their SHA-256), and the last
`kBoxModulesKept` stay compiled after their images close, so unloading a module and loading it
again compiles nothing. Each instance owns one store. `kernel_lifetime_counts()` counts the
modules compiled and the stores opened and closed; a module's open and close count as a
library's.

The bounds above are named, not copied: `src/kernel/wasm_box.hpp` owns each value.
`containment_note()` speaks for native images only and says nothing about a box.

**The demo.** `zen-box-reload` (`tests/box_reload/`, the `box_reload` entry) reloads one live
counter from its native build into its module and back while asks keep arriving, by
`zen.ReloadWeave` as `loom-host` asks it, and offers the two reloads a box refuses; its
transcript says which build gave each answer and how long each reload held the bus:

```text
zen-box-reload <build>/tests/libzen_box_counter.so <build>/wasm/box_counter.wasm \
               <build>/wasm/box_counter_reaching.wasm [--turns <n>] [--asks <n>] [--check]
```

## The weave build contract

`loom_weave_build_contract(<target>)` ships with the package
(`lib/cmake/loom/loom-weave.cmake`, included by `loomConfig.cmake`) and is what
keeps `dlclose` real and, on Windows, a weave's C++ runtime its own
([KERN-05](../laws/kernel-laws.md)). It applies the platform's requirement to
exactly the target handed to it, records the verdict on that target's
`LOOM_WEAVE_BUILD_CONTRACT` property, and refuses a target type that is never
`dlopen`'ed. ELF/GNU is the combination with unique symbol binding; PE-COFF and
Mach-O have none, and the function says so rather than injecting an option a
compiler merely tolerates. It is present in kernel-less packages too — what you
can *author* is not gated on what an install can *host*, which stays
`if(TARGET loom::kernel)`. See [guides/dynamic-weaves](../guides/dynamic-weaves.md)
for the authoring shape.

On Windows the weave carries its own C++ runtime, so whichever runtime the
machine or the host holds never decides which one it runs with, and it loads on
a machine that holds only Windows:

- **MinGW-w64** (GCC, or Clang targeting it): a `SHARED` or `MODULE` weave links
  with `-static`, so libgcc, libstdc++ and the thread library are inside the
  image; the C runtime stays Windows' own. A static library has nothing to
  apply: its runtime is chosen when the image that links it is linked. Every
  library the weave links by `-l` name is taken from its static archive where one
  exists, and one that has only an import library is not found. The function
  first checks that the toolchain can link its runtime statically, and refuses
  the target otherwise.

  A copy of the runtime in the image takes from the process what it never gives
  back: winpthreads, under GCC, a Windows TLS index for its thread record the
  first time the image's code reaches it (a guarded static's first
  initialization, `std::call_once`, `std::thread`, and under emulated TLS a
  throw or a `thread_local`); libc++, under Clang, an FLS index for
  `std::thread`; and libstdc++ its emergency exception pool, about 80 KB. A
  process has 1,088 TLS indexes, so a process that loaded such a weave about a
  thousand times would abort. The weave therefore also links
  `loom-weave-tls-return.cpp`, installed beside `loom-weave.cmake`, with the
  import slots of `TlsAlloc`, `TlsFree`, `FlsAlloc` and `FlsFree` wrapped
  (`--wrap=__imp_...`): it records each index the image takes, and the image's
  first-registered `atexit` handler, which the C runtime runs after the image's
  other handlers, static and `thread_local` destructors, returns every index
  still held and the pool when the image unloads. A process may then load and
  free a weave any number of times; under GCC each reload leaves about 2 KB.

  Only a toolchain a weave's reload has been measured on is accepted: GCC from
  <!-- value LOOM_WEAVE_MINGW_GCC_LEAST -->12.2<!-- /value --> and Clang from
  <!-- value LOOM_WEAVE_MINGW_CLANG_LEAST -->18.1<!-- /value -->, whose linker can
  wrap an import slot (checked by linking the file above). A libstdc++ built
  without the C runtime's `__cxa_thread_atexit` (`_GLIBCXX_HAVE___CXA_THREAD_ATEXIT`
  undefined) registers a `thread_local`'s destructor itself and keeps the image
  loaded while the thread that used it lives, so an unload that reports success
  leaves it in place; it is refused, as is an older compiler, in words naming
  the reason.
- **The MSVC ABI** (MSVC, or a Clang that simulates it): every object is compiled
  for the static runtime (`MultiThreaded`, `MultiThreadedDebug` in Debug). The
  runtime is chosen in each object, and every object of one image must agree, so
  Loom's libraries are built for it and `loom-weave.cmake` makes it the default
  `CMAKE_MSVC_RUNTIME_LIBRARY` of the project that includes it; a project that
  names another runtime is refused at configure, and one whose flags choose a
  runtime by hand (`/MD`, or policy CMP0091 old) is refused by the function.

  The static runtime includes the C runtime, so a weave's C stdio is its own: it
  writes to the Windows standard handle it loaded with, and a host's redirect
  through its own C runtime (`_dup2` onto descriptor 1, `freopen`) does not reach
  it once it has loaded. A host that wants a weave's output points the standard
  handle before the weave loads -- `_dup2` onto descriptor 1 does that too, while
  it lasts -- and keeps that handle open until the weave has unloaded, since a
  weave writing to a closed handle writes nowhere, or to whatever reuses it.
- Any other Windows compiler is unclassified: the function warns, applies
  nothing, and the verdict says so.

Each image then has its runtime's state to itself: its own heap where the
runtime keeps one, its own exception machinery. The seam never let an exception
or a C++ object cross ([dynamic ABI](dynamic-abi.md)), and with a runtime in each
image that is a requirement, not a convention.

Why it matters: a weave's shapes instantiate Loom's inline templates, such as
`schema_of<T>()`'s function-local static, with vague linkage. On ELF, GCC built
with `--enable-gnu-unique-object` emits those as `STB_GNU_UNIQUE`, and glibc
resolves such symbols through a program-wide table that ignores `RTLD_LOCAL`,
marks the defining image `NODELETE` and outlives `dlclose`. Unload then reports
success (`dlclose()` returns 0, `unload()` returns true) while the image stays
resident, and the next load of a different library sharing the same vocabulary
binds to the old image's statics. Forgetting the contract is no build error;
this is what it produces. It is a function rather than an interface target
because an interface target's reach is the consumer's to choose (`PUBLIC` would
spread the option downstream), cannot refuse a target type, and cannot fail on a
compiler that cannot express the contract.

Inside **this repo** the call is not optional and not remembered: the
`weave_population` entry derives which of Loom's own artifacts must carry it from
the build graph and names any that left the roll
([POP-05](../laws/population-laws.md)). That is a house rule about Loom's tree —
a consumer's build system is neither enumerated nor required to adopt it.

## Tests

Suite `kernel` (load/unload/reload/candidate/admission lifetimes), `capabilities` (the message door), Zengine's lanes as the
stranger-consumer proof. Suite `box` and the `box_reload` entry (the `box` gate,
`LOOM_ENABLE_WASM`): the WebAssembly backend through real modules.
