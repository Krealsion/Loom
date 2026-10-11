// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
//
// Linked into every MinGW-w64 weave by the weave build contract (KERN-05, cmake/loom-weave.cmake):
// the image's runtime takes Windows TLS and FLS indexes, and libstdc++ its exception pool, and
// returns none. Each call through the wrapped import slots of TlsAlloc, TlsFree, FlsAlloc and
// FlsFree (--wrap=__imp_...) is recorded here, and what is held goes back, with the pool, at unload
// (docs/reference/kernel.md#the-weave-build-contract).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

#if defined(__GLIBCXX__)
namespace __gnu_cxx {
// libstdc++'s own, declared in none of its headers. Named only where the runtime is libstdc++,
// which has it; libc++ keeps no such pool. Weak, so a libstdc++ without it links and has none to
// free.
__attribute__((weak)) void __freeres();
} // namespace __gnu_cxx
#endif

namespace {

// The function kernel32 exports under `name`, found when it is needed: a wrapped slot is renamed
// for every caller in the image, this file's own included, so the real one is reached by name,
// only when the runtime takes or returns an index, and at unload.
template <class Function>
Function loom_weave_kernel32(const char* name) {
    const FARPROC found = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(found));
}

using LoomWeaveTlsAlloc = DWORD(WINAPI*)();
using LoomWeaveFlsAlloc = DWORD(WINAPI*)(PFLS_CALLBACK_FUNCTION);
using LoomWeaveIndexFree = BOOL(WINAPI*)(DWORD);

// The indexes of one kind this image took and has not returned. Each is live at once, so the
// ledger is never full while it holds more slots than the process has indexes of that kind. Plain
// data with no initializer: a static one is zeroed before any code of the image runs, and zero is
// an empty ledger and an unlocked SRWLOCK, so an index taken by any constructor is recorded.
template <DWORD Slots>
struct LoomWeaveIndexLedger {
    DWORD held[Slots];
    DWORD count;
    SRWLOCK lock;

    void take(DWORD index) {
        AcquireSRWLockExclusive(&lock);
        if (count < Slots) {
            held[count++] = index;
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void give(DWORD index) {
        AcquireSRWLockExclusive(&lock);
        for (DWORD k = 0; k < count; ++k) {
            if (held[k] == index) {
                held[k] = held[--count];
                break;
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void give_all(const char* free_name) {
        const auto release = loom_weave_kernel32<LoomWeaveIndexFree>(free_name);
        while (count > 0) {
            release(held[--count]);
        }
    }
};

// TLS: TLS_MINIMUM_AVAILABLE and the 1,024 expansion slots. FLS: 4,080 on current Windows.
LoomWeaveIndexLedger<64 + 1024> loom_weave_tls;
LoomWeaveIndexLedger<4096> loom_weave_fls;

DWORD WINAPI loom_weave_tls_alloc() {
    const DWORD index = loom_weave_kernel32<LoomWeaveTlsAlloc>("TlsAlloc")();
    if (index != TLS_OUT_OF_INDEXES) {
        loom_weave_tls.take(index);
    }
    return index;
}

BOOL WINAPI loom_weave_tls_free(DWORD index) {
    loom_weave_tls.give(index);
    return loom_weave_kernel32<LoomWeaveIndexFree>("TlsFree")(index);
}

DWORD WINAPI loom_weave_fls_alloc(PFLS_CALLBACK_FUNCTION callback) {
    const DWORD index = loom_weave_kernel32<LoomWeaveFlsAlloc>("FlsAlloc")(callback);
    if (index != FLS_OUT_OF_INDEXES) {
        loom_weave_fls.take(index);
    }
    return index;
}

BOOL WINAPI loom_weave_fls_free(DWORD index) {
    loom_weave_fls.give(index);
    return loom_weave_kernel32<LoomWeaveIndexFree>("FlsFree")(index);
}

// At an unload the image's TLS callbacks, DllMain, thread_local destructors, atexit handlers and
// static destructors may each still use an index, in the CRT's order. The image's first atexit
// handler runs after all of them: the first constructor (priority 101) registers it, and the CRT
// runs a DLL's handlers in reverse. It returns every index still held -- an FLS index's callback,
// still in the image, runs for each thread holding a value -- and then the pool.
void loom_weave_give_back() {
    loom_weave_fls.give_all("FlsFree");
    loom_weave_tls.give_all("TlsFree");
#if defined(__GLIBCXX__)
    if (__gnu_cxx::__freeres != nullptr) {
        __gnu_cxx::__freeres();
    }
#endif
}

__attribute__((constructor(101))) void loom_weave_arm() {
    std::atexit(loom_weave_give_back);
}

} // namespace

extern "C" {
// The wrapped import slots: what every call in the image through them reaches.
DWORD(WINAPI* __wrap___imp_TlsAlloc)() = loom_weave_tls_alloc;
BOOL(WINAPI* __wrap___imp_TlsFree)(DWORD) = loom_weave_tls_free;
DWORD(WINAPI* __wrap___imp_FlsAlloc)(PFLS_CALLBACK_FUNCTION) = loom_weave_fls_alloc;
BOOL(WINAPI* __wrap___imp_FlsFree)(DWORD) = loom_weave_fls_free;
}
