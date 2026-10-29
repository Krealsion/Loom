// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef LOOM_SRC_KERNEL_WASM_BOX_HPP
#define LOOM_SRC_KERNEL_WASM_BOX_HPP

// The kernel's WebAssembly backend (LOOM_ENABLE_WASM): a weave built as a wasm32 module, run by
// Wasmtime inside the host's process and driven through the same `ZenWeaveAbi` a native image
// hands the kernel. The module's only reach is the host table's doors, as imports; every other
// import is refused when the module is opened, and no WASI implementation is linked.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include <zen/kernel/abi.h>
#include <zen/switchboard/grant.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace loom::detail {

/// One compiled module: shared by every image opened from the same bytes, and kept while a
/// recent open may still want it (`kBoxModulesKept`), so unloading and loading the same file
/// again compiles nothing.
class BoxModule;

/// The steps a module may take in one entry from the host (Wasmtime's fuel). Reaching it traps
/// the instance, so no handler can hold the host's one thread for longer.
inline constexpr std::uint64_t kBoxStepsPerEntry = 200'000'000;
/// The linear memory a box may grow to when its grant names no memory (`ResourceLimits`).
inline constexpr std::int64_t kBoxDefaultMemoryBytes = std::int64_t{64} << 20;
/// What a module may write to its standard output and error in one entry; the rest is counted.
inline constexpr std::size_t kBoxLogBytes = 4096;
/// The longest role or shape name a module may hand a door, terminator excluded.
inline constexpr std::size_t kBoxMaxNameBytes = 4096;
/// Compiled modules kept after their last image closed, most recently opened first.
inline constexpr std::size_t kBoxModulesKept = 8;

/// Open the module at `path`: compile it (or find it compiled), refuse every import outside the
/// box's set by name, then make one throwaway instance to read the ABI version it was built
/// against. Null with `error` set on any refusal; no instance of the weave exists either way.
std::shared_ptr<BoxModule> box_open(const std::string& path, std::string& error);

/// The descriptor every boxed instance is driven through. Its `create` slot refuses: a boxed
/// instance is made by `box_create`, which knows its module.
const ZenWeaveAbi* box_abi() noexcept;

/// Make one instance of the module's weave in a store of its own; null when the weave refused
/// its making or the module trapped, with its words in `refusal`.
void* box_create(const std::shared_ptr<BoxModule>& module, ZenByteSink refusal);

/// Project `grant` onto the instance's box, or refuse in `why` the parts it cannot carry: the
/// box gives memory from `ResourceLimits` and reaches nothing outside the host's doors, so an OS
/// capability, a filesystem level or a cpu share is refused, never narrowed.
bool box_mount(void* instance, const Grant& grant, std::string& why);

/// What `Kernel::containment` says of a boxed instance, from what the box imposes on it now: the
/// memory and the steps it is held to, the reach it is refused, its reloads after a trap or its
/// quarantine, and the tier's honest scope.
/// docs/reference/capabilities.md#the-box-a-weave-built-as-a-webassembly-module
std::string box_containment(const void* instance);

/// Process-wide counts of the backend's work, for `kernel_lifetime_counts()`.
struct BoxCounts {
    std::uint64_t modules_compiled = 0;
    std::uint64_t stores_opened = 0;
    std::uint64_t stores_closed = 0;
};
BoxCounts box_counts() noexcept;

} // namespace loom::detail

#endif // LOOM_SRC_KERNEL_WASM_BOX_HPP
