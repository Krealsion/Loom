// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The reload witness's weave (`weave_reload`, tests/reload_witness.cpp). One call reaches its C++
// runtime by every road that takes something from the process on first use -- a throw, a guarded
// static, std::call_once, a thread_local with a destructor, a thread -- and writes through
// std::cout, whose streams the runtime sets up as the image starts (libstdc++ from 13 before any
// constructor of the image's own) and tears down as it unloads. Free of every loom header, so what
// a load takes is this image's runtime alone.

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

const std::string& guarded() {
    static const std::string value(40, 'g');
    return value;
}

std::once_flag once;
int once_value = 0;

struct PerThread {
    int value = 0;
    ~PerThread() { value = -1; }
};
thread_local PerThread per_thread;

} // namespace

// Answers `n` when every road was taken, -1 otherwise.
extern "C" __declspec(dllexport) int zen_reload_runtime(int n) {
    int roads = 0;
    try {
        throw std::runtime_error("thrown and caught inside the image");
    } catch (const std::runtime_error&) {
        ++roads;
    }
    if (guarded().size() == 40) {
        ++roads;
    }
    std::call_once(once, [] { once_value = 1; });
    roads += once_value;
    per_thread.value = n;
    if (per_thread.value == n) {
        ++roads;
    }
    int from_thread = 0;
    std::thread worker([&from_thread] { from_thread = 1; });
    worker.join();
    roads += from_thread;
    if (std::cout << '.' << std::flush) {
        ++roads;
    }
    return roads == 6 ? n : -1;
}
