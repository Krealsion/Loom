// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_IDLE_NAP_HPP
#define ZEN_HOST_IDLE_NAP_HPP

// The serving host's wait while its bus is idle and its console closed. On Windows a short sleep
// lasts a scheduler tick (about 15.6 ms) whatever it asks, and under MinGW-w64
// `std::this_thread::sleep_for` does even where the process raised its timer resolution, so the
// nap waits on a high-resolution waitable timer and lasts the period it names.

#include <cstdint>

namespace loom::host {

class IdleNap {
public:
    IdleNap();
    ~IdleNap();
    IdleNap(const IdleNap&) = delete;
    IdleNap& operator=(const IdleNap&) = delete;

    /// Block for `ms` milliseconds; zero or less returns at once.
    void nap(int ms);

#ifdef _WIN32
    /// What the last nap asked of Windows: the relative due time, in 100 ns units, and whether it
    /// waited on a high-resolution timer. A nap on `Sleep`, where Windows refused that timer, is
    /// kept to the scheduler tick whatever it asks.
    struct Asked {
        std::int64_t due_100ns = 0;
        bool high_resolution = false;
    };
    Asked last() const noexcept { return asked_; }

private:
    void* timer_ = nullptr;
    Asked asked_{};
#endif
};

} // namespace loom::host

#endif
