// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The libraries the Windows loader's weave needs (weavelib/loader_weave.cpp), two of them, so a
// case can leave out a library's own library: the leaf, and under ZEN_LOADER_MIDDLE the middle,
// which needs the leaf. Plain C entry points and nothing of Loom.

#if defined(ZEN_LOADER_MIDDLE)
extern "C" int zen_loader_leaf_value();

extern "C" __declspec(dllexport) int zen_loader_middle_value() {
    return zen_loader_leaf_value() + 1;
}
#else
extern "C" __declspec(dllexport) int zen_loader_leaf_value() {
    return 41;
}
#endif
