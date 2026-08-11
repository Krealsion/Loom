// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TERMINAL_VOCABULARY_HPP
#define ZEN_TERMINAL_VOCABULARY_HPP

// WHAT A TERMINAL PARTICIPANT KNOWS HOW TO SAY — and, separately, what it has
// declared it is willing to be told.
//
// THREE POWERS THE WORD "inspect" USED TO HIDE, kept apart here because they are
// different and only the first one lives in this file:
//
//   TYPE KNOWLEDGE       which shape schemas can be described and composed
//   SERVICE DISCOVERY    which weaves and roles currently exist
//   TRAFFIC OBSERVATION  which messages are flowing
//
// A vocabulary is TYPE KNOWLEDGE ALONE. Holding a schema is not permission to
// send it — that is the Kernel's answer, given at delivery against this
// participant's own effective authority — and it is not evidence that anybody is
// listening. A terminal can know `Work v1` perfectly, be refused when it sends
// one, and be refused again by a world where no service holds the office. Those
// are three separate facts and this type answers only the first.
//
// IT IS SUPPLIED, NOT DISCOVERED. The host names the shapes when it mounts the
// participant. That is deliberately narrower than the console's registry read:
// enumerating a live Switchboard's registry reveals which shapes some weave
// somewhere has declared, which is a fact about the running world rather than
// about this participant, and it needs a `Switchboard&` no ordinary weave holds.
// A terminal that could enumerate every shape in the process because it is a
// terminal would have been handed a power nothing granted it.
//
// A DOOR IS A SECOND, SEPARATE DECLARATION. Being able to COMPOSE `Work v1` and
// being willing to RECEIVE one are unrelated: an ordinary terminal composes
// requests it will never be sent and receives answers it will never author. So
// `knows()` adds type knowledge and `accepts()` adds type knowledge AND a door.
// The accept-set the bus is given is exactly the doors — never the catalog.

#include <zen/schema.hpp>
#include <zen/terminal/composer.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

/// One catalog entry, as a presentation lists it.
struct VocabularyEntry {
    std::string name;
    std::uint32_t version = 0;
    /// Is this also one of this participant's DOORS — a shape it declared it is
    /// willing to be delivered? Listed rather than inferred, because a catalog
    /// that showed only names would leave "why can this never arrive?"
    /// unanswerable at the surface a person is reading.
    bool accepted = false;
};

/// The shapes a terminal participant may describe and compose, and the subset it
/// declares as doors.
///
/// Not bounded, deliberately: every entry is placed by the HOST at mount, so
/// there is no traffic that can grow it and nothing to evict. (Everything in this
/// core that untrusted traffic can grow — the transcript, the received store — is
/// bounded, and says so.)
class TerminalVocabulary {
public:
    /// Add TYPE KNOWLEDGE: this shape may be described and composed. It does not
    /// become a door, and it confers no authority to send it.
    TerminalVocabulary& knows(std::shared_ptr<const loom::Schema> schema) {
        return add(std::move(schema), /*door=*/false);
    }

    /// Add type knowledge AND a door: this shape may be described, composed, and
    /// DELIVERED here. Still no authority — a door says what may arrive, never
    /// what this participant may say.
    TerminalVocabulary& accepts(std::shared_ptr<const loom::Schema> schema) {
        return add(std::move(schema), /*door=*/true);
    }

    /// The schema for (name, version) if this vocabulary carries it, else nullptr.
    std::shared_ptr<const loom::Schema> find(std::string_view name,
                                             std::uint32_t version) const {
        for (const Entry& e : entries_) {
            if (e.schema->name() == name && e.schema->version() == version) {
                return e.schema;
            }
        }
        return nullptr;
    }

    /// Everything known, in the order the host declared it.
    std::vector<VocabularyEntry> catalog() const {
        std::vector<VocabularyEntry> out;
        out.reserve(entries_.size());
        for (const Entry& e : entries_) {
            out.push_back(VocabularyEntry{e.schema->name(), e.schema->version(), e.door});
        }
        return out;
    }

    /// The accept-set to hand the bus: exactly the doors, never the catalog.
    std::vector<std::shared_ptr<const loom::Schema>> doors() const {
        std::vector<std::shared_ptr<const loom::Schema>> out;
        for (const Entry& e : entries_) {
            if (e.door) {
                out.push_back(e.schema);
            }
        }
        return out;
    }

    /// The full description of one known shape, or nullopt. Knowing a shape is
    /// never a claim that anything currently accepts it.
    std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const {
        const std::shared_ptr<const loom::Schema> s = find(name, version);
        if (!s) {
            return std::nullopt;
        }
        return describe_schema(*s);
    }

    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::shared_ptr<const loom::Schema> schema;
        bool door = false;
    };

    /// Re-declaring a shape WIDENS it to a door and never narrows it back: a host
    /// that writes `knows(X)` after `accepts(X)` has said two things, and the one
    /// that would silently close a door it already opened is the dangerous
    /// reading. (Nothing here can widen AUTHORITY — a door is not a grant.)
    TerminalVocabulary& add(std::shared_ptr<const loom::Schema> schema, bool door) {
        if (!schema) {
            return *this;
        }
        for (Entry& e : entries_) {
            if (e.schema->name() == schema->name() && e.schema->version() == schema->version()) {
                e.door = e.door || door;
                return *this;
            }
        }
        entries_.push_back(Entry{std::move(schema), door});
        return *this;
    }

    std::vector<Entry> entries_;
};

} // namespace loom

#endif // ZEN_TERMINAL_VOCABULARY_HPP
