// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// What the serving host's idle nap asks of Windows (src/host/idle_nap.hpp), read from the nap
// itself rather than timed: a busy machine lengthens every wait, but cannot change what was asked.

#include <doctest.h>

#include "idle_nap.hpp"

using loom::host::IdleNap;

TEST_SUITE("host_nap") {

TEST_CASE("the idle nap asks Windows for the period it names, on a high-resolution timer") {
    IdleNap nap;
    nap.nap(5);
    CHECK(nap.last().high_resolution);
    CHECK(nap.last().due_100ns == 5 * 10000);
    nap.nap(1);
    CHECK(nap.last().high_resolution);
    CHECK(nap.last().due_100ns == 1 * 10000);
    nap.nap(0); // nothing to wait for: it returns at once and asks nothing
    CHECK(nap.last().due_100ns == 1 * 10000);
}

} // TEST_SUITE("host_nap")
