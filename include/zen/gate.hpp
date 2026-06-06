#ifndef ZEN_GATE_HPP
#define ZEN_GATE_HPP

#include <zen/admission.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <vector>

namespace zen {

/// How thoroughly the gate reports.
enum class Report : std::uint8_t {
    FirstError, ///< hot path: stop at the first offense
    Full,       ///< diagnostics: collect every offense
};

/// THE gate. Asks two questions with one function:
///   1. identity  — does the claimant claim the schema this door admits?
///   2. structure — is the claimant genuinely a well-formed instance of it?
/// recursing into nested messages and lists. On success it yields the value
/// (moved out of `claimant`); on failure it yields machine-readable errors.
///
/// This is the single admission point for *live* values. Deserialized values
/// reach the identical structural validator via <zen/serialize.hpp>'s admit().
Admission admit(Value claimant, const Schema& door, Report report = Report::FirstError);

/// Non-consuming full diagnosis: every conformance error (empty == conforms),
/// without taking ownership. Runs the same single validator as admit().
std::vector<Error> diagnose(const Value& claimant, const Schema& door);

/// Process-wide count of how many times the single structural validator has
/// run. Exposed so tests can prove the bus path and the persistence path go
/// through the very same gate. Not a stability guarantee; for observability.
std::uint64_t gate_invocations() noexcept;

} // namespace zen

#endif // ZEN_GATE_HPP
