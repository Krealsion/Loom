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
looked at; nothing is read before a load or after one that succeeds. Whether a refused
image may show the system's error dialog is the host's to decide (`SetErrorMode`), not
Loom's.

## The reloadable-weave build contract

`loom_weave_build_contract(<target>)` ships with the package
(`lib/cmake/loom/loom-weave.cmake`, included by `loomConfig.cmake`) and is what
keeps `dlclose` real ([KERN-05](../laws/kernel-laws.md)). It applies the
platform's requirement to exactly the target handed to it, records the verdict
on that target's `LOOM_WEAVE_BUILD_CONTRACT` property, and refuses a target type
that is never `dlopen`'ed. ELF/GNU is the affected combination; PE-COFF and
Mach-O have no unique symbol binding, and the function says so rather than
injecting an option a compiler merely tolerates. It is present in kernel-less
packages too — what you can *author* is not gated on what an install can *host*,
which stays `if(TARGET loom::kernel)`. See
[guides/dynamic-weaves](../guides/dynamic-weaves.md) for the authoring shape.

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
stranger-consumer proof.
