#ifndef ZEN_DETAIL_GATE_INTERNAL_HPP
#define ZEN_DETAIL_GATE_INTERNAL_HPP

// Internal to zen-core. The single structural validator that every admission
// path funnels through. <zen/gate.hpp>'s admit()/diagnose() (the live-message
// path) and <zen/serialize.hpp>'s admit() (the persisted-bytes path) both call
// this and only this to decide conformance — there is no second validator.

#include <zen/admission.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <vector>

namespace zen::detail {

/// Validate that `claimant` claims `door`'s identity and is a well-formed
/// instance of it. Returns the errors found (empty == conforms). When
/// `collect_all` is false, stops at the first error. Increments the process
/// gate-invocation counter (see zen::gate_invocations()).
std::vector<Error> validate_into(const Value& claimant, const Schema& door, bool collect_all);

} // namespace zen::detail

#endif // ZEN_DETAIL_GATE_INTERNAL_HPP
