// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_ISOLATION_PROTOCOL_HPP
#define ZEN_ISOLATION_PROTOCOL_HPP

// The parent<->child wire protocol for out-of-process Weave hosting. Frames are
// length-prefixed: [u32 payload_len][u8 op][payload]. The payload sub-fields are
// little-endian and read through a bounds-checked Cursor, so a hostile or
// truncated frame is rejected, never over-read.
//
// Zen's serialized values are the IPC currency (exactly as for persistence and
// the DLL boundary): every Value/message/snapshot/policy crosses as bytes and is
// re-admitted host-side through the one gate.

#include <zen/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loom {

enum class Op : std::uint8_t {
    // child -> parent
    Hello = 1,    ///< [bytes manifest][bytes policy][bytes snapshot] (the handshake)
    Emit = 2,     ///< [u8 kind][u64 target][u64 reply_to][u64 correlation][bytes payload]
    Snapshot = 3, ///< [bytes snapshot] (refreshed state, after each handle/revive)
    EmitRole = 4, ///< [bytes role][u64 reply_to][u64 correlation][bytes payload] — NO sender
                  ///< on the wire; the host stamps it from link.id and routes via role
    // parent -> child
    Deliver = 16,  ///< [u64 sender][u64 reply_to][u64 correlation][bytes payload]
    Revive = 17,   ///< [bytes state]
    Shutdown = 18, ///< (empty)
};

// The wire primitives -- put_u8/u32/u64, put_bytes, Cursor, kMaxFrameLen, the send kinds --
// live in <zen/wire.hpp> since the two-host crossing, shared with the bridge protocol.

} // namespace loom

#endif // ZEN_ISOLATION_PROTOCOL_HPP
