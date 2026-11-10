// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The weave build contract's sentinel (KERN-05). Two artifacts are built from this source, one
// through loom_weave_build_contract() and one DELIBERATELY without it: under GCC the bypass twin
// must still come out STB_GNU_UNIQUE on ELF, and on PE it must still import its C++ runtime, or
// the sentinel can no longer express the failure. Clang on ELF emits no such binding, so the
// twin carrying none is the record, and the contracted half is not passing for lack of a hazard.

// Free of every loom header, so the bypass artifact, loaded on purpose in that state, aliases
// nothing else in the process. The symbol copies the real hazard's shape: loom::schema_of<T>()
// (include/zen/weave/shape.hpp) is a function template holding a function-local static, which
// GCC emits with vague linkage and, on ELF, as STB_GNU_UNIQUE. On GCC 11.4, inline function
// statics, function template statics, class template static data members and inline variables
// get the binding; a plain external function's local static does not.

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

// The C++ runtime used out of line, by an exception thrown and caught here, so an image built from
// this source either carries that runtime or imports it.
extern "C" int zen_contract_runtime(int n) {
    try {
        if (n > 0) {
            throw n;
        }
    } catch (const int caught) {
        return caught;
    }
    return 0;
}
