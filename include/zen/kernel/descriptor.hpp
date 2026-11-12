// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_DESCRIPTOR_HPP
#define ZEN_KERNEL_DESCRIPTOR_HPP

// What a host judges of an image's descriptor before calling into it, and the manifest version
// this build writes and reads. Header-only, so the kernel and the isolated host's child, which
// links nothing of Loom, judge an image alike. docs/reference/dynamic-abi.md

#include <zen/kernel/abi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace loom {

/// The `zen.Manifest` version this build's `describe` writes and its host reads
/// (zen/kernel/schema_codec.hpp). It moves without the ABI's version: a descriptor says which it
/// writes, and a host refuses by name one it does not read.
inline constexpr std::uint32_t kManifestVersion = 6;

namespace detail {

/// Where a descriptor's every image must reach: the end of its `handle` slot.
inline constexpr std::size_t kDescriptorReachesHandle =
    offsetof(ZenWeaveAbi, handle) + sizeof(ZenWeaveAbi::handle);

/// The versions this host loads, as a refusal names them.
inline std::string loaded_versions() {
    return ZEN_ABI_OLDEST == ZEN_ABI_VERSION
               ? std::to_string(ZEN_ABI_VERSION)
               : std::to_string(ZEN_ABI_OLDEST) + " to " + std::to_string(ZEN_ABI_VERSION);
}

/// Read `abi` as this host reads it: true with `out` holding the slots its size reaches and
/// NULL past them; false with `why` naming what the image needs and what this host has. Reads
/// the descriptor's bytes and calls nothing of the image's.
inline bool read_descriptor(const ZenWeaveAbi* abi, ZenWeaveAbi& out, std::string& why) {
    out = ZenWeaveAbi{};
    if (abi->abi_version < ZEN_ABI_OLDEST) {
        why = "unsupported abi_version " + std::to_string(abi->abi_version) +
              ", from before the tables said their size (host supports " + loaded_versions() +
              ")";
        return false;
    }
    if (abi->abi_version > ZEN_ABI_VERSION) {
        why = "the image needs abi_version " + std::to_string(abi->abi_version) +
              ", which this host lacks (host supports " + loaded_versions() + ")";
        return false;
    }
    if (abi->size < kDescriptorReachesHandle) {
        why = "the image's descriptor is " + std::to_string(abi->size) +
              " bytes, which does not reach its handle slot (" +
              std::to_string(kDescriptorReachesHandle) + " bytes)";
        return false;
    }
    if (abi->host_size > sizeof(ZenHostApi)) {
        why = "the image needs a host table of " + std::to_string(abi->host_size) +
              " bytes, which this host lacks (its table is " + std::to_string(sizeof(ZenHostApi)) +
              " bytes)";
        return false;
    }
    if (abi->manifest_version != kManifestVersion) {
        why = "the image writes zen.Manifest version " + std::to_string(abi->manifest_version) +
              ", which this host does not read (it reads version " +
              std::to_string(kManifestVersion) + ")";
        return false;
    }
    const std::size_t reach = abi->size < sizeof(ZenWeaveAbi) ? abi->size : sizeof(ZenWeaveAbi);
    std::memcpy(&out, abi, reach);
    out.size = static_cast<std::uint32_t>(reach);
    return true;
}

} // namespace detail
} // namespace loom

#endif // ZEN_KERNEL_DESCRIPTOR_HPP
