// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_BOX_TWIN_HPP
#define ZEN_TESTS_BOX_TWIN_HPP

// A door proved for a boxed image by the case that proves it for a native one: the case's body is
// a function of the image it loads, called once with the native fixture in its own suite and, in
// a kernel built with the WebAssembly backend, once with the same source built as a module, as a
// case of suite `box`. One body, so the two can never be asked different questions.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include <doctest.h>

#if defined(LOOM_ENABLE_WASM)
/// The boxed twin of a case whose body is `fn(const char* image)`: a case named `name`, in suite
/// `box`, run on the module at `boxed`.
#define ZEN_BOXED_TWIN(name, fn, boxed)                                                            \
    TEST_CASE(name * doctest::test_suite("box")) { fn(boxed); }
#else
#define ZEN_BOXED_TWIN(name, fn, boxed)
#endif

#endif // ZEN_TESTS_BOX_TWIN_HPP
