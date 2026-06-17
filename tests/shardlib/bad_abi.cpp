// A library that exports a descriptor with an unsupported abi_version. The
// kernel must reject it cleanly, before calling any of its function pointers
// (which are therefore left null).

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

extern "C" ZEN_KERNEL_EXPORT const ZenShardAbi* zen_shard_abi(void) {
    static const ZenShardAbi abi = {
        ZEN_ABI_VERSION + 1000u, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &abi;
}
