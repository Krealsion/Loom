#ifndef ZEN_ZEN_HPP
#define ZEN_ZEN_HPP

/// zen-core: the self-describing-value-and-gate foundation.
///
/// A value carries a reference to the schema it claims to be — typed enough to
/// be challenged at any boundary, dynamic enough to be built at runtime from a
/// schema that was discovered rather than compiled. Exactly one gate (admit)
/// guards every boundary: the live message bus and the persistence layer reach
/// the same validator. Nothing crosses a boundary it cannot prove it belongs
/// across.
///
/// This umbrella pulls in the whole public surface. Prefer including the
/// specific header you need.

#include <zen/admission.hpp>
#include <zen/gate.hpp>
#include <zen/kind.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>

#endif // ZEN_ZEN_HPP
