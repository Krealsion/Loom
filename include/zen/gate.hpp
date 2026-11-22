// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_GATE_HPP
#define ZEN_GATE_HPP

#include <zen/admission.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace loom {

/// How thoroughly the gate reports.
enum class Report : std::uint8_t {
    FirstError, ///< hot path: stop at the first offense
    Full,       ///< diagnostics: collect every offense
};

class Registry;

/// THE gate. Asks two questions with one function:
///   1. identity  — does the claimant claim the schema this door admits?
///   2. structure — is the claimant genuinely a well-formed instance of it?
/// recursing into nested messages, unions and lists. On success it yields the value
/// (moved out of `claimant`); on failure it yields machine-readable errors.
///
/// Structure is whole: a record holds every required field and no field holds the wrong kind;
/// a union holds exactly one of its cases (`NoCase` otherwise); and the value holds no more
/// cells than a decode would take back (`TooLarge`, `kMaxDecodedCells`), counted a visit, so a
/// value whose parts are shared is bounded by the walk it costs.
///
/// This is the single admission point for *live* values. Deserialized values
/// reach the identical structural validator via <zen/serialize.hpp>'s admit().
///
/// A field of Kind::Any holds a value of any shape, and this overload has nothing to check that
/// shape against, so it refuses one with `UnknownSchema`; the next one checks it.
Admission admit(Value claimant, const Schema& door, Report report = Report::FirstError);

/// The same gate, checking each value an Any field holds against `open`: its shape must be
/// registered there, as one definition with it (`same_identity`), and the value must conform to
/// it. The bus admits every delivery through this one, against its own registry.
Admission admit(Value claimant, const Schema& door, const Registry& open,
                Report report = Report::FirstError);

/// Non-consuming full diagnosis: every conformance error (empty == conforms),
/// without taking ownership. Runs the same single validator as admit().
std::vector<Error> diagnose(const Value& claimant, const Schema& door);
std::vector<Error> diagnose(const Value& claimant, const Schema& door, const Registry& open);

/// What a value makes its holder keep, in the two units it is kept in: the cells a decode of it
/// would materialise, and the payload bytes its `Text` and `Bytes` fields carry. One walk answers
/// both, because a holder bounded in one of them alone is bounded in neither: structure without
/// bytes misses a one-cell value carrying megabytes, and bytes without structure misses the
/// compact value that commands the decoder's whole allowance.
struct ValueWeight {
    /// Counted as the decoder and the gate count cells: one per declared field of every record,
    /// one per union, one per list element, and a nested value at each place it stands. The walk
    /// stops once this passes `kMaxDecodedCells` (<zen/serialize.hpp>) and answers one more than
    /// it, a count no boundary admits, so weighing costs at most that many steps whatever the
    /// value holds -- and so `bytes` is what the walk reached, which is all of a value any
    /// boundary would admit.
    std::uint64_t cells = 0;
    /// The bytes those `Text` and `Bytes` cells carry, which the decoder's allowance never
    /// counted: a 1 MiB `Bytes` field is one cell and a megabyte.
    std::uint64_t payload_bytes = 0;

    /// The host memory holding this value keeps: its cells' slots and its payloads' bytes. A
    /// holder bounding memory alone (a window of kept values) is bounded in this one number,
    /// where the bus, which also owes each waiting message a walk and a delivery, is bounded in
    /// the cells apart.
    ///
    /// It is a budget, not a measurement of the heap, and never an RSS guarantee (as LIFE-07 is
    /// not): a cell is charged the slot a `Value` allocates per declared field, while the heap
    /// also holds each nested value's own object, and a value whose parts are shared holds one
    /// copy of what this counts at every place it stands. Measured against a counting allocator,
    /// a value of `kMaxDecodedCells` cells whose nested parts are shared holds 0.83x of this, and
    /// one decoded from bytes, whose parts are its own, about 2x. What it is exactly is
    /// proportional to the structure and the payloads, which is what a budget needs.
    std::uint64_t held_bytes() const noexcept {
        return cells * sizeof(std::optional<Cell>) + payload_bytes;
    }
};

/// Weigh `v` as its holder keeps it. It reads the value's own shapes and judges nothing, so for a
/// value the gate admits this is the gate's own count. The bus charges each waiting message both
/// halves against its sender's room (`Switchboard::kMaxPendingCellsPerSender`,
/// `kMaxPendingBytesPerSender`). docs/reference/bounds.md
ValueWeight weigh(const Value& v) noexcept;

/// `weigh(v).cells` alone, for a holder that bounds structure: the count every boundary's
/// allowance is spent in. docs/reference/bounds.md#the-decode-materialization-bound
std::uint64_t decoded_cells(const Value& v) noexcept;

/// Process-wide count of how many times the single structural validator has
/// run. Exposed so tests can prove the bus path and the persistence path go
/// through the very same gate. Not a stability guarantee; for observability.
std::uint64_t gate_invocations() noexcept;

} // namespace loom

#endif // ZEN_GATE_HPP
