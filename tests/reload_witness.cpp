// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The `weave_reload` entry (KERN-05): weavelib/reload_runtime.cpp's contracted image loaded, called
// down every road that takes a TLS or FLS index, and freed, <count> times, past the process's TLS
// ceiling. Each free must end the image, the indexes free after the first load (the process takes
// some once) must be free after the last, none holding a value, and each load past the first
// hundred must leave less than kLeftPerLoad. Exit 0, 1 naming the failure, or the runtime's own.
//   zen-reload-witness <image> <count>      (docs/reference/kernel.md#the-weave-build-contract)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// A tenth of one libstdc++ emergency pool: what is left a load once the image gives it back is
// about 2 KB, and without that about 84 KB.
constexpr SIZE_T kLeftPerLoad = 8 * 1024;
constexpr int kWarmLoads = 100;

struct IndexCount {
    int tls = 0;
    int stale = 0;
    int fls = 0;
};

IndexCount count_free_indexes() {
    IndexCount c;
    std::vector<DWORD> taken;
    taken.reserve(4096);
    for (;;) {
        const DWORD index = TlsAlloc();
        if (index == TLS_OUT_OF_INDEXES) {
            break;
        }
        if (TlsGetValue(index) != nullptr) {
            ++c.stale;
        }
        taken.push_back(index);
    }
    for (const DWORD index : taken) {
        TlsFree(index);
    }
    c.tls = static_cast<int>(taken.size());
    taken.clear();
    for (;;) {
        const DWORD index = FlsAlloc(nullptr);
        if (index == FLS_OUT_OF_INDEXES) {
            break;
        }
        taken.push_back(index);
    }
    for (const DWORD index : taken) {
        FlsFree(index);
    }
    c.fls = static_cast<int>(taken.size());
    return c;
}

SIZE_T private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX mem{};
    mem.cb = sizeof mem;
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),
                            sizeof mem);
    return mem.PrivateUsage;
}

int fail(const std::string& why) {
    std::fprintf(stderr, "weave_reload FAILED: %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 3) {
        std::fprintf(stderr, "usage: zen-reload-witness <image> <count>\n");
        return 2;
    }
    const std::string image = argv[1];
    const int count = std::atoi(argv[2]);
    if (count <= kWarmLoads) {
        return fail("the count must exceed " + std::to_string(kWarmLoads));
    }
    using Entry = int (*)(int);
    IndexCount before;
    SIZE_T warm = 0;
    for (int i = 1; i <= count; ++i) {
        HMODULE h = LoadLibraryA(image.c_str());
        if (h == nullptr) {
            return fail("load " + std::to_string(i) + " was refused, error " +
                        std::to_string(GetLastError()));
        }
        const auto entry = reinterpret_cast<Entry>(
            reinterpret_cast<void (*)()>(GetProcAddress(h, "zen_reload_runtime")));
        if (entry == nullptr) {
            return fail("the image exports no zen_reload_runtime");
        }
        if (entry(i) != i) {
            return fail("call " + std::to_string(i) + " did not take every road");
        }
        if (!FreeLibrary(h)) {
            return fail("free " + std::to_string(i) + " failed, error " +
                        std::to_string(GetLastError()));
        }
        if (GetModuleHandleA(image.c_str()) != nullptr) {
            return fail("free " + std::to_string(i) + " left the image in the process, so the "
                        "next load would not be a new copy of its runtime");
        }
        if (i == 1) {
            before = count_free_indexes();
            std::printf("weave_reload: %s, %d TLS and %d FLS indexes free after the first load\n",
                        image.c_str(), before.tls, before.fls);
            std::fflush(stdout);
        }
        if (i == kWarmLoads) {
            warm = private_bytes();
        }
        if (i % 200 == 0) {
            const IndexCount now = count_free_indexes();
            std::printf("weave_reload: after %d loads, %d TLS and %d FLS indexes free\n", i,
                        now.tls, now.fls);
            std::fflush(stdout);
        }
    }
    const IndexCount after = count_free_indexes();
    const SIZE_T end = private_bytes();
    const SIZE_T left = end > warm ? (end - warm) / static_cast<SIZE_T>(count - kWarmLoads) : 0;
    if (after.tls != before.tls || after.fls != before.fls) {
        return fail(std::to_string(before.tls) + " TLS and " + std::to_string(before.fls) +
                    " FLS indexes were free after the first load and " +
                    std::to_string(after.tls) + " and " + std::to_string(after.fls) + " after " +
                    std::to_string(count) + " loads: the image's runtime kept what it took");
    }
    if (after.stale != 0) {
        return fail(std::to_string(after.stale) + " free TLS indexes still hold a value");
    }
    if (left >= kLeftPerLoad) {
        return fail("each load left " + std::to_string(left) + " bytes past the first " +
                    std::to_string(kWarmLoads) + ", at least " + std::to_string(kLeftPerLoad));
    }
    std::printf("weave_reload: %d loads, each a new image; %d TLS and %d FLS indexes free after "
                "the first and the last, no TLS index holding a value; %s bytes left a load past "
                "the first %d\n",
                count, after.tls, after.fls, std::to_string(left).c_str(), kWarmLoads);
    return 0;
}
