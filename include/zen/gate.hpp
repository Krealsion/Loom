// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_GATE_HPP
#define ZEN_GATE_HPP

#include <zen/admission.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
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

/// Process-wide count of how many times the single structural validator has
/// run. Exposed so tests can prove the bus path and the persistence path go
/// through the very same gate. Not a stability guarantee; for observability.
std::uint64_t gate_invocations() noexcept;

} // namespace loom

#endif // ZEN_GATE_HPP
