#ifndef ZEN_SWITCHBOARD_HPP
#define ZEN_SWITCHBOARD_HPP

/// zen-switchboard: the first live boundary in Zen.
///
/// An in-process message bus (the Switchboard) routes self-describing Values
/// between Shards and gates every delivery through zen-core's one validator —
/// admit(Value, recipient_accept_schema). It reimplements no validation, schema,
/// or serialization logic; it links zen-core and uses it. Dispatch is
/// single-threaded and FIFO, so delivery is deterministic and never reentrant.
///
/// This umbrella pulls in the whole public surface.

#include <zen/switchboard/message.hpp>
#include <zen/switchboard/shard.hpp>
#include <zen/switchboard/switchboard.hpp>

#endif // ZEN_SWITCHBOARD_HPP
