# Kernel — reference

Dynamic weaves: loading native libraries as bus participants across a true C
ABI. Laws: [KERN-01..04](../laws/kernel-laws.md). ABI detail:
[dynamic-abi](dynamic-abi.md). Guide:
[dynamic-weaves](../guides/dynamic-weaves.md).

## Loading

`Kernel::load(name, path[, role])` — open the library, fetch and version-check
the descriptor, construct the instance, reconstruct its manifest (accept-set +
state schema, crossing as gated values), register a **host adapter** on the
bus (optionally bound to a role). The adapter *is* a `Weave`; on the bus a
loaded weave is indistinguishable from a native one. Grants: loaded weaves
currently receive permissive bus-send authority (B1's demonstrated door is the
kernel's own gated `LoadLibrary` capability).

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

Canonical: Linux/WSL (`dlopen`). `LOOM_ENABLE_WINDOWS_KERNEL` is an opt-in
**development/demo** `LoadLibrary` backend with no isolation, truth-pinned at
every surface (`containment_note()`); never a default.

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

Inside **this repo** the call is not optional and not remembered: the
`weave_population` entry derives which of Loom's own artifacts must carry it from
the build graph and names any that left the roll
([POP-05](../laws/population-laws.md)). That is a house rule about Loom's tree —
a consumer's build system is neither enumerated nor required to adopt it.

## Tests

Suite `kernel` (446+ assertions across load/unload/reload/candidate/admission
lifetimes), `capabilities` (the message door), Zengine's lanes as the
stranger-consumer proof.
