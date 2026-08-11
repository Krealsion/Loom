// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The reloadable-weave build contract's sentinel (KERN-05).
//
// Two artifacts are built from this one source: one through
// loom_weave_build_contract(), one DELIBERATELY without it. The pair is the whole
// proof. The contracted artifact alone would be a test that cannot fail -- most of
// this tree's fixtures hide their shapes in anonymous namespaces, so they carry no
// unique symbols with or without the contract, and an assertion over them would be
// green for the most boring reason there is. The bypass twin is the positive control
// that keeps the question askable: it must STILL come out STB_GNU_UNIQUE, or the
// sentinel has stopped being able to express F-22 and the other half proves nothing.
//
// Deliberately free of every loom header. The symbol below is the one thing under
// test, and keeping it unique to this file means the bypass artifact -- which is
// loaded, on purpose, in the state F-22 describes -- cannot alias anything else in
// the process. Its shape is copied from the real hazard, not from loom's code:
// loom::schema_of<T>() (include/zen/weave/shape.hpp) is a function template holding a
// function-local static, which is exactly what GCC emits with vague linkage and
// therefore, on ELF, as STB_GNU_UNIQUE.
//
// Measured on GCC 11.4 (2026-08-04): four patterns get the binding -- inline function
// statics, function template statics, class template static data members, and inline
// variables -- while a plain external function's local static does NOT. Do not assume
// an arbitrary static local will do; this one is the shape that does.

namespace zen_contract_sentinel {

template <typename T>
int& reload_sentinel() {
    static int n = 0;
    return n;
}

struct Tag {};

} // namespace zen_contract_sentinel

// Each call increments, so a FRESH image is externally observable: the first call
// after a genuinely fresh load returns 1. A second load that returns anything else is
// reading the statics of the image it replaced.
extern "C" int zen_contract_touch() {
    return ++zen_contract_sentinel::reload_sentinel<zen_contract_sentinel::Tag>();
}
