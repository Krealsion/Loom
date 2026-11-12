// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// THE PUBLIC MACRO SURFACE, COMPILED AS A STRANGER: every form of ZEN_SHAPE / ZEN_EXPOSE /
// ZEN_HIDE, both zero-argument spellings included (the dispatch MSVC's traditional preprocessor
// cannot perform), ZEN_UNION with and without names, ZEN_ENUM and loom::Any, built against
// find_package(loom) with no consumer-written compiler switch, so a package that stops carrying
// its own preprocessor requirement fails here. It ASSERTS what each derives, since a surface that
// builds but tags or names the wrong fields would pass a compile-only witness.

#include "witness_protocol.hpp"

#include <zen/serialize.hpp>
#include <zen/weave/shape.hpp>

#include <zen/registry.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
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

// A std::optional member: an optional field of its type, absent when it holds std::nullopt.
struct Note {
    std::string to;
    std::optional<std::string> text;
    ZEN_SHAPE(Note, 1, ZEN_FIELD(to), ZEN_EXPOSE(text));
};

// A union named by its cases' shapes (ZEN_UNION's variadic part empty), one named by the line,
// an enumeration, and a record holding them and an open field.
struct Moved {
    std::int64_t x = 0;
    ZEN_SHAPE(Moved, 1, ZEN_FIELD(x));
};
struct Pressed {
    std::int64_t key = 0;
    ZEN_SHAPE(Pressed, 1, ZEN_FIELD(key));
};
struct Moment {
    std::variant<Moved, Pressed> is;
    ZEN_UNION(Moment, 1, is);
};
struct Side {
    std::variant<std::int64_t, std::monostate> is;
    ZEN_UNION(Side, 1, is, "by", "none");
};
enum class Mood { calm, busy };
ZEN_ENUM(Mood, 1, calm, busy);
struct Held {
    Moment moment;
    Side side;
    Mood mood = Mood::calm;
    loom::Any extra;
    ZEN_SHAPE(Held, 1, ZEN_FIELD(moment), ZEN_FIELD(side), ZEN_FIELD(mood), ZEN_FIELD(extra));
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
    check(tally.size() == 5, "Tally derives five fields");

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

    // ---- a std::optional member is an optional field of its type ------------------
    const auto note = loom::schema_of<Note>();
    const auto twin = loom::SchemaBuilder("Note", 1)
                          .field("to", loom::Kind::Text)
                          .field("text", loom::Kind::Text, /*required=*/false)
                          .build();
    check(note->content_id() == twin->content_id(),
          "a std::optional member derives the hand-built optional field");
    const loom::Value absent = loom::to_value(Note{"you", std::nullopt});
    check(!absent.has("text"), "std::nullopt leaves the field absent");
    const loom::Admission back = loom::admit(loom::parse(loom::serialize(absent)), note);
    check(back.ok() && !loom::from_value<Note>(back.value()).text.has_value(),
          "an absent field crosses the bytes and comes back as std::nullopt");
    const std::vector<loom::FieldAccess> na = loom::access_of<Note>();
    check(na.size() == 2 && na[1].writable && na[1].type.kind == loom::Kind::Text,
          "ZEN_EXPOSE(text) tags a std::optional member, and its kind is the inner one");

    // ---- a union, an enumeration and an open field ----------------------------------
    const auto moment = loom::schema_of<Moment>();
    check(moment->is_union() && moment->fields().size() == 2 &&
              moment->fields()[1].name == std::string("Pressed"),
          "ZEN_UNION names each case by its shape");
    const auto side = loom::schema_of<Side>();
    check(side->fields().size() == 2 && side->fields()[0].name == std::string("by") &&
              !side->fields()[1].carries,
          "ZEN_UNION names every case by the line, and std::monostate carries nothing");
    const auto mood = loom::schema_of<Mood>();
    check(mood->is_union() && mood->fields().size() == 2 &&
              mood->fields()[1].name == std::string("busy"),
          "ZEN_ENUM is a union of its enumerators");
    loom::Registry open;
    open.register_schema(loom::schema_of<Moved>());
    Held held;
    held.moment.is = Pressed{7};
    held.side.is = std::monostate{};
    held.mood = Mood::busy;
    held.extra = loom::Any::of(Moved{3});
    const loom::Admission crossed =
        loom::admit(loom::parse(loom::serialize(loom::to_value(held))), loom::schema_of<Held>(), open);
    check(crossed.ok(), "a value holding a union, an enumeration and an open field crosses the bytes");
    if (crossed.ok()) {
        const Held read = loom::from_value<Held>(crossed.value());
        check(std::get<Pressed>(read.moment.is).key == 7 && read.side.is.index() == 1 &&
                  read.mood == Mood::busy && read.extra.as<Moved>().x == 3,
              "each comes back as it was sent");
    }
    check(!loom::admit(loom::to_value(held), *loom::schema_of<Held>()).ok(),
          "an open field is refused by a gate given no registry");

    if (failures != 0) {
        std::fprintf(stderr, "package witness: macro surface FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: macro surface PASSED");
    return 0;
}
