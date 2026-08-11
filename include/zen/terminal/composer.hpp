// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TERMINAL_COMPOSER_HPP
#define ZEN_TERMINAL_COMPOSER_HPP

// THE TYPED COMPOSER — turning a handful of loose values into one exact Loom
// message, from the real schema, with every refusal saying which rung it fell
// off.
//
// It is the console's assumption ladder, moved down a layer and stopped one step
// earlier. The ladder itself (named wins -> positional -> type-directed ->
// prompt) is unchanged in behaviour, unchanged in ordering, and unchanged in
// what it refuses. COMPOSING and SENDING are deliberately two
// operations instead of one. `compose_message` assembles and stops. Whoever
// wants the message sent sends it — the console engine through its
// `LadderHost`, a terminal participant through its own identity-bound channel, a
// future Workshop pane by showing it to a person first and sending it if they
// say so.
//
// THAT SPLIT IS THE WHOLE REASON THIS FILE EXISTS. A composer that sends cannot
// be reused by a presentation that wants to display a message before it goes,
// and a composer wired to one sender cannot be reused by a second participant
// with a different identity. Both were true of the old shape, and neither is a
// property of the LADDER — only of where it stopped.
//
// WHAT IT DOES NOT DO, and must never learn to: it does not know what a shape
// MEANS, does not know who is running, does not resolve a role, and does not
// decide whether the composer may send what it just composed. It reads a schema
// and places values into fields. Authority is the Kernel's answer, given later,
// at delivery.

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace loom {

/// One field of a shape, read from a schema (compose-time guidance).
struct FieldDesc {
    std::string name;
    std::string type; ///< the kind's spelling ("Int", "Text", "List<Int>", "Message(Foo v1)")
    bool required;
};

/// A shape's full description (its fields), for a person or a pane to fill.
struct ShapeDesc {
    std::string name;
    std::uint32_t version;
    std::vector<FieldDesc> fields;
};

/// Build a ShapeDesc (name/version + each field's type spelling and required-ness) from a resolved
/// Schema — shared by every surface that describes a shape: the console over the bus registry, the
/// remote console over a reconstructed Schema, and a terminal participant over its own vocabulary.
ShapeDesc describe_schema(const loom::Schema& schema);

/// A typed field value for compose-by-name. It carries the scalar kinds; Message/List fields are
/// not composable (a required one left unset is caught at the gate).
using FieldValue = std::variant<std::int64_t, double, std::string, bool, loom::Bytes>;

/// A reference to a field of an already-received message — `$m1.count` → {label "m1", field
/// "count"}. A reference is a *wire*: one message's output read into another's input. Resolution
/// belongs to whoever owns the received values (the console's reply buffer, a terminal
/// participant's received store); this type only names one.
struct Ref {
    std::string label; ///< the owner's label for a received message, e.g. "m1" / "r1"
    std::string field; ///< a field of that message's Value
};

/// One argument to the assumption ladder: a literal or a reference, optionally named
/// (`field=…`). A named arg is assigned to that field; a bare (unnamed) arg is a
/// positional/type-directed candidate.
struct Arg {
    std::optional<std::string> name;     ///< set iff `field=…` (the named rung)
    std::variant<FieldValue, Ref> value; ///< a literal value or a reference
};

/// THE TWO LOOKUPS THE LADDER NEEDS, AND NOTHING ELSE — deliberately not a send
/// surface, so a composer can be handed to something that has no authority to
/// speak at all.
class ComposeSource {
public:
    virtual ~ComposeSource() = default;

    /// The schema for (name, version), or nullptr if this source does not know it. WHERE it looks
    /// is the source's business and is a real difference in power: the console reads the bus's
    /// whole registry; a terminal participant reads only the vocabulary its host handed it.
    virtual std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                               std::uint32_t version) const = 0;

    /// Resolve `$label.field` to a scalar Cell off an already-received message, or nullopt +
    /// *error. A read, never a mutation: the referenced value is immutable history.
    virtual std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error) const = 0;

protected:
    ComposeSource() = default;
    ComposeSource(const ComposeSource&) = default;
    ComposeSource& operator=(const ComposeSource&) = default;
};

/// A COMPOSITION, STOPPED ONE STEP BEFORE ANYTHING IS SENT.
///
/// `Ready` carries the schema and the cells the ladder placed — enough to
/// assemble the exact Value, and not yet a message anyone has authored. The
/// other two statuses are the ladder's two honest ways of not producing one, and
/// they are different questions for the caller: `Error` means this can never be
/// what you meant; `NeedsInput` means say more.
struct Composition {
    enum class Status { Ready, NeedsInput, Error };

    Status status = Status::Error;
    std::shared_ptr<const loom::Schema> schema;   ///< Ready: the shape that was resolved
    std::map<std::string, loom::Cell> cells;      ///< Ready: field name -> placed value
    std::string error;                            ///< Error: the compose-time verdict
    std::vector<FieldDesc> open_fields;           ///< NeedsInput: the still-unfilled fields
    std::vector<std::string> unplaced;            ///< NeedsInput: args the ladder could not place

    explicit operator bool() const noexcept { return status == Status::Ready; }
};

/// THE ASSUMPTION LADDER. Assign each arg (literal or reference, each optionally
/// named) to a field of `name v<version>`:
///
///   1. NAMED WINS.      `count=3` goes to `count` or is an error — never a guess.
///   2. POSITIONAL.      bare args fill the still-open fields in DECLARATION order,
///                       all-or-falls: one misfit and the whole rung is abandoned.
///   3. TYPE-DIRECTED.   each bare arg to its UNIQUE fitting open field, all distinct.
///   4. PROMPT.          genuine ambiguity, or a still-open REQUIRED field, returns
///                       NeedsInput with the open fields named.
///
/// Never guess on ambiguity, never compose something incomplete on purpose. A
/// literal widens Int -> Float and nothing else; a REFERENCE matches its resolved
/// kind exactly, because wiring one message's output into another's input should
/// be predictable rather than clever.
Composition compose_message(const ComposeSource& source, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args);

/// Assemble a Ready composition into the Value it describes.
///
/// An unset optional field is left ABSENT rather than defaulted — the gate is the
/// unconditional backstop at send, and a composer that invented values would be
/// deciding something the maker did not say.
loom::Value assemble(const Composition& composition);

} // namespace loom

#endif // ZEN_TERMINAL_COMPOSER_HPP
