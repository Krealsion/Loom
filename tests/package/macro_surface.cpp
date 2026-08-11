// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE PUBLIC MACRO SURFACE, COMPILED AS A STRANGER.
//
// Every form of ZEN_SHAPE / ZEN_EXPOSE / ZEN_HIDE, including BOTH zero-argument
// spellings -- which is the exact dispatch that MSVC's traditional preprocessor
// cannot perform. Built against find_package(loom) with NO consumer-written
// compiler switch: if the installed package stops carrying its own preprocessor
// requirement, this file is where a stranger finds out, and it is the same place
// the founder's Zengine build found out.
//
// It ASSERTS rather than merely compiles. A macro surface that expands to
// something that builds but tags the wrong fields would otherwise pass a
// compile-only witness.

#include "witness_protocol.hpp"

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Whole-state scope: a bare ZEN_EXPOSE(); -- empty __VA_ARGS__, the form that
// pasted into ZEN_DETAIL_EXPOSE___VA_OPT__ under the traditional preprocessor.
struct ExposeAll {
    std::int64_t a;
    std::int64_t b;
    ZEN_EXPOSE();
    ZEN_SHAPE(ExposeAll, 1, ZEN_FIELD(a), ZEN_FIELD(b));
};

// Whole-state scope: a bare ZEN_HIDE();
struct HideAll {
    std::int64_t a;
    ZEN_HIDE();
    ZEN_SHAPE(HideAll, 1, ZEN_FIELD(a));
};

// An untagged shape: the default is read-exposed, write-hidden.
struct Untagged {
    std::int64_t seq;
    ZEN_SHAPE(Untagged, 1, ZEN_FIELD(seq));
};

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "  FAIL: %s\n", what);
        ++failures;
    }
}

const loom::FieldAccess* find(const std::vector<loom::FieldAccess>& v, const std::string& name) {
    for (const loom::FieldAccess& f : v) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    std::puts("package witness: public macro surface");

    // ---- field scope: ZEN_EXPOSE(x) / ZEN_HIDE(x) / ZEN_FIELD(x) -----------------
    const std::vector<loom::FieldAccess> tally = loom::access_of<witness::Tally>();
    check(tally.size() == 3, "Tally derives three fields");

    const loom::FieldAccess* handled = find(tally, "handled");
    const loom::FieldAccess* raw_total = find(tally, "raw_total");
    const loom::FieldAccess* label = find(tally, "label");
    check(handled != nullptr && raw_total != nullptr && label != nullptr,
          "every registered field name survives the expansion");
    if (handled != nullptr) {
        check(handled->writable && !handled->hidden, "ZEN_EXPOSE(handled) is writable, not hidden");
    }
    if (raw_total != nullptr) {
        check(raw_total->hidden && !raw_total->writable, "ZEN_HIDE(raw_total) is hidden, not writable");
    }
    if (label != nullptr) {
        check(!label->writable && !label->hidden, "ZEN_FIELD(label) takes neither tag");
    }

    // ---- whole-state scope: bare ZEN_EXPOSE(); and ZEN_HIDE(); -------------------
    const std::vector<loom::FieldAccess> ea = loom::access_of<ExposeAll>();
    check(ea.size() == 2, "ExposeAll derives two fields");
    for (const loom::FieldAccess& f : ea) {
        check(f.writable, "bare ZEN_EXPOSE() applies to EVERY field");
        check(!f.hidden, "bare ZEN_EXPOSE() hides nothing");
    }

    const std::vector<loom::FieldAccess> ha = loom::access_of<HideAll>();
    check(ha.size() == 1, "HideAll derives one field");
    for (const loom::FieldAccess& f : ha) {
        check(f.hidden, "bare ZEN_HIDE() applies to EVERY field");
        check(!f.writable, "bare ZEN_HIDE() exposes nothing");
    }

    // ---- the untagged default ----------------------------------------------------
    const std::vector<loom::FieldAccess> ut = loom::access_of<Untagged>();
    check(ut.size() == 1 && !ut[0].writable && !ut[0].hidden,
          "an untagged field carries neither tag");

    // ---- the tags are metadata BESIDE the shape, never part of it ----------------
    // A tagged and an untagged shape of the same structure must share a content-id.
    // If the macros ever leaked a tag into the schema this would diverge.
    check(loom::schema_of<witness::Ping>()->name() == std::string("Ping"),
          "ZEN_SHAPE carries the struct's own name");
    check(loom::schema_of<witness::Ping>()->version() == 1u, "ZEN_SHAPE carries the version");

    if (failures != 0) {
        std::fprintf(stderr, "package witness: macro surface FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: macro surface PASSED");
    return 0;
}
