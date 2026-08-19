// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_HPP
#define ZEN_WEAVE_HPP

/// The weave layer: the header-only weaving sugar (the tools a maker uses to build a weave).
///
/// Pure sugar over loom + zen-switchboard. Write each shape once as a plain
/// C++ struct (ZEN_SHAPE) and derive the runtime Schema, the typed conversions,
/// the snapshot/revive, and the message dispatch — all through the existing
/// public API, with no second schema, no second validator, and no change to the
/// gate, the wire format, the bus, or the kernel.

#include <zen/weave/describe.hpp>
#include <zen/weave/poke.hpp>
#include <zen/weave/poke_weave.hpp>
#include <zen/weave/relay.hpp>
#include <zen/weave/shape.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <zen/weave/weave.hpp>

#endif // ZEN_WEAVE_HPP
