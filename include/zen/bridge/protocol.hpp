#ifndef ZEN_BRIDGE_PROTOCOL_HPP
#define ZEN_BRIDGE_PROTOCOL_HPP

// The OPERATOR-PROTOCOL: the client<->host wire for the remote-operator console. A remote console
// cannot hold a Switchboard& across a socket, so the engine runs CLIENT-side and its three host
// interactions — discovery, the tap, and sends — cross as framed messages the host answers and
// streams. This is the decision-#2 "discovery-and-tap-as-messages" purification, and it makes a
// remote operator the SAME pattern as an out-of-process weave (a proxy-participant bridging a socket
// to the bus), pointed at an operator instead of a hosted weave.
//
// Frames are length-prefixed exactly as the out-of-process-weave protocol: [u32 payload_len][u8 op]
// [payload], little-endian, read through the bounds-checked Cursor — so a hostile or truncated frame
// is rejected, never over-read. We REUSE the wire primitives (put_u*, Cursor, kMaxFrameLen) from the
// out-of-process-weave protocol header: they are portable, header-only, POSIX-free byte helpers that
// happen to live there. (A neutral wire.hpp the two protocols share is a clean future factoring —
// a noted seam, not built, to avoid touching the proven isolation header.)
//
// Zen's serialized values are the IPC currency here too: the operator's send crosses as serialized
// message bytes the host re-admits through the ONE gate, exactly as a child's Emit is re-admitted —
// and the host stamps the sender from the CONNECTION, never the wire (the anti-spoof).

#include <zen/isolation/protocol.hpp> // put_u8/u32/u64, put_bytes, Cursor, kMaxFrameLen (portable)

#include <cstdint>

namespace loom {

/// The operator-protocol opcodes. client->host requests/sends; host->client replies/streams.
enum class BridgeOp : std::uint8_t {
    // ---- client -> host ----
    Hello = 1,      ///< [u32 proto_version] — handshake; the host replies Welcome
    ListWeaves = 2, ///< (empty) — explicit discovery refresh; the host replies Weaves
    Describe = 3,   ///< [bytes name][u32 version] — the host replies Schema (encoded) or SchemaNone
    Send = 4,       ///< the operator's send (an assembled message, like Op::Emit):
                    ///<   [u8 kind][u64 wire_sender][u64 target][u64 wire_reply_to][u64 correlation][bytes payload]
                    ///< The host re-admits `payload` through the gate and STAMPS sender + reply_to
                    ///< from the CONNECTION (the proxy's id), IGNORING wire_sender and wire_reply_to.
                    ///< An honest client sets those to 0; a malicious one forges them and the bridge
                    ///< stamps over them (the forge-the-hostile-frame pin). kind: kEmitSend/kEmitPublish.

    // ---- host -> client ----
    Welcome = 16,    ///< [u64 operator_id][u32 proto_version] — the operator's stamped bus id
    Weaves = 17,     ///< [u32 n]{[u64 id][u32 m]{[bytes name][u32 version]}} — the live weave set
    Schema = 18,     ///< [bytes encoded_schema] (schema_codec) — reply to Describe (found)
    SchemaNone = 19, ///< [bytes name][u32 version] — reply to Describe (no such registered shape)
    Delivered = 20,  ///< [bytes payload] — a reply Value delivered to the operator (fills the buffer)
    Tap = 21,        ///< a copied bus event for the operator's window on the live bus:
                     ///<   [u8 kind][u64 target][u64 sender][bytes schema][u32 version][bytes refusal]
};

/// Tap event kinds on the wire (mirrors loom::EventKind, fixed so the client need not link the bus).
inline constexpr std::uint8_t kTapDelivered = 0;
inline constexpr std::uint8_t kTapRefused = 1;
inline constexpr std::uint8_t kTapDied = 2;
inline constexpr std::uint8_t kTapRevived = 3;

/// The operator-protocol version (bumped on any wire-shape change; Hello/Welcome carry it).
inline constexpr std::uint32_t kBridgeProtocolVersion = 1;

} // namespace loom

#endif // ZEN_BRIDGE_PROTOCOL_HPP
