// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// A descriptor needing what this host lacks, one field wrong in each build, so its refusal names
// that one thing (docs/reference/dynamic-abi.md#the-number-and-the-sizes): NEEDS_NEXT_VERSION the
// next ABI version, NEEDS_LARGER_HOST a larger host table, NEEDS_UNREAD_MANIFEST a manifest
// version this host does not read, NEEDS_SHORT_DESCRIPTOR a size not reaching `handle`. Every
// slot is null: a host that called one before refusing would crash, failing loudly.

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT and the manifest version

#include <cstddef>
#include <cstdint>

namespace {

#if defined(NEEDS_NEXT_VERSION)
constexpr std::uint32_t kVersion = ZEN_ABI_VERSION + 1u;
#else
constexpr std::uint32_t kVersion = ZEN_ABI_VERSION;
#endif

#if defined(NEEDS_LARGER_HOST)
constexpr std::uint32_t kHostSize = static_cast<std::uint32_t>(sizeof(ZenHostApi) + sizeof(void*));
#else
constexpr std::uint32_t kHostSize = static_cast<std::uint32_t>(sizeof(ZenHostApi));
#endif

#if defined(NEEDS_UNREAD_MANIFEST)
constexpr std::uint32_t kManifest = loom::kManifestVersion - 1u;
#else
constexpr std::uint32_t kManifest = loom::kManifestVersion;
#endif

#if defined(NEEDS_SHORT_DESCRIPTOR)
constexpr std::uint32_t kSize = static_cast<std::uint32_t>(offsetof(ZenWeaveAbi, handle));
#else
constexpr std::uint32_t kSize = static_cast<std::uint32_t>(sizeof(ZenWeaveAbi));
#endif

} // namespace

// By name, in declaration order (KERN-04).
extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {.abi_version = kVersion,
                                    .size = kSize,
                                    .host_size = kHostSize,
                                    .manifest_version = kManifest,
                                    .create = nullptr,
                                    .destroy = nullptr,
                                    .describe = nullptr,
                                    .snapshot = nullptr,
                                    .policy = nullptr,
                                    .revive = nullptr,
                                    .handle = nullptr,
                                    .claim_published = nullptr,
        .describe_creation = nullptr,
        .create_with = nullptr};
    return &abi;
}
