#ifndef ZEN_AUTHOR_HPP
#define ZEN_AUTHOR_HPP

/// zen-author: the header-only authoring layer.
///
/// Pure sugar over zen-core + zen-switchboard. Write each shape once as a plain
/// C++ struct (ZEN_SHAPE) and derive the runtime Schema, the typed conversions,
/// the snapshot/revive, and the message dispatch — all through the existing
/// public API, with no second schema, no second validator, and no change to the
/// gate, the wire format, the bus, or the kernel.

#include <zen/author/shape.hpp>
#include <zen/author/shard.hpp>

#endif // ZEN_AUTHOR_HPP
