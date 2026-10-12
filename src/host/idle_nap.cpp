// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "idle_nap.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <chrono>
#include <thread>
#endif

namespace loom::host {

#ifdef _WIN32

IdleNap::IdleNap()
    : timer_(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS)) {}

IdleNap::~IdleNap() {
    if (timer_ != nullptr) {
        ::CloseHandle(timer_);
    }
}

void IdleNap::nap(int ms) {
    if (ms <= 0) {
        return;
    }
    const std::int64_t due_100ns = static_cast<std::int64_t>(ms) * 10000;
    LARGE_INTEGER due{};
    due.QuadPart = -due_100ns; // relative
    if (timer_ != nullptr &&
        ::SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE &&
        ::WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) {
        asked_ = Asked{due_100ns, true};
        return;
    }
    ::Sleep(static_cast<DWORD>(ms));
    asked_ = Asked{due_100ns, false};
}

#else

IdleNap::IdleNap() = default;
IdleNap::~IdleNap() = default;

void IdleNap::nap(int ms) {
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif

} // namespace loom::host
