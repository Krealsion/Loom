// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE WINDOWS LOADER'S WEAVE: an ordinary loadable weave that needs a library of its own
// (weavelib/loader_libs.cpp, the middle library, which needs the leaf), for the cases where a
// weave's libraries are looked for and what a refusal names when one is missing. Its state holds
// the middle library's answer, so that library is a real import of this image.

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstdint>

extern "C" int zen_loader_middle_value();

namespace {

struct LoaderState {
    std::int64_t answer = 0;
    ZEN_SHAPE(LoaderState, 1, ZEN_FIELD(answer));
};

class LoaderWeave final : public loom::WeaveBase<LoaderWeave, LoaderState, loom::Accept<>> {
public:
    LoaderWeave() { state_.answer = zen_loader_middle_value(); }
};

} // namespace

ZEN_EXPORT_WEAVE(LoaderWeave)
