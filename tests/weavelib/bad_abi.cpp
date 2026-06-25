// A library that exports a descriptor with an unsupported abi_version. The
// kernel must reject it cleanly, before calling any of its function pointers
// (which are therefore left null).

#include <zen/kernel/abi.h>
#include <zen/kernel/export.hpp> // for ZEN_KERNEL_EXPORT

extern "C" ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {
    static const ZenWeaveAbi abi = {
        ZEN_ABI_VERSION + 1000u, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &abi;
}
