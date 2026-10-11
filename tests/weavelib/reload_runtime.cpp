// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The reload witness's weave (`weave_reload`, tests/reload_witness.cpp). One call reaches the C++
// runtime the image carries by every road on which a copy of that runtime takes something from the
// process the first time it is used: a throw, a function-local static's guarded initialization,
// std::call_once, a thread_local with a destructor, and a thread. Free of every loom header, so
// what a load takes is this image's runtime alone.

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
    return roads == 5 ? n : -1;
}
