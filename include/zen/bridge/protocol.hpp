// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_PROTOCOL_HPP
#define ZEN_BRIDGE_PROTOCOL_HPP

// THE CROSSING'S WIRE: what one Loom host says to another over a framed socket.
//
// A `Switchboard&` cannot cross a socket. What crosses instead is a small fixed vocabulary of
// frames: a HANDSHAKE the host answers with admission or refusal, DISCOVERY the host answers,
// a SEND the host re-admits through the one gate and stamps from the connection, a DELIVERY the
// host ships to the connection with the facts Loom stamped on it, and -- for a connection the
// host's policy lets OBSERVE -- a copy of each bus event.
//
// Frames are length-prefixed exactly as the out-of-process-weave protocol: [u32 payload_len]
// [u8 op][payload], little-endian, read through the bounds-checked Cursor -- so a hostile or
// truncated frame is rejected, never over-read. The wire primitives (put_u*, Cursor,
// kMaxFrameLen) are the portable, header-only helpers of <zen/wire.hpp>, shared with the
// isolation protocol.
//
// ---- v4: a crossing between two hosts, not only an operator's console ------------------
//
// v3 was written for ONE principal: every connection was the operator, so `Hello` carried a
// version and nothing else, `Delivered` carried payload bytes and nothing else, and every
// connection received the whole-bus tap. v4 is written for a GUEST -- another host's
// participant, admitted deliberately and granted narrowly -- and changes exactly what that
// needs:
//
//   Hello      carries a CLAIMED name and a credential. What the peer says about itself is
//              data the host's admission policy judges; it is never the identity the host
//              establishes, which comes back on Welcome.
//   Denied     the host's refusal, with its reason, before the connection is severed.
//              A refused connection has no proxy on the bus and can act on nothing.
//   Welcome    the session identity the host minted (the proxy's WeaveId) and the name the
//              host's policy ESTABLISHED for this connection (empty when the policy
//              established none).
//   Send       may address an OFFICE (`role`) as well as a WeaveId, so a guest can speak to
//              "whoever holds zengine.input" without learning a small integer first.
//   Delivered  carries what Loom stamped on the delivery beside the payload: the bus-stamped
//              sender, the correlation, whether Loom attests it as THE answer to an ask this
//              connection sent (`answers_ask`), whether it is a dispatch-refusal notice, and
//              the office the sender spoke as. A richer client cannot recover a fact the
//              crossing discarded, so the crossing no longer discards them.
//   Tap        is copied only to a connection whose admission verdict grants observation.
//
// Zen's serialized values are still the currency: a Send crosses as serialized message bytes the
// host re-admits through the ONE gate, exactly as a child's Emit is -- and the host stamps the
// sender from the CONNECTION, never the wire (the anti-spoof).

#include <zen/wire.hpp> // put_u8/u32/u64, put_bytes, Cursor, kMaxFrameLen, kEmitSend/kEmitPublish

#include <cstdint>

namespace loom {

/// The crossing's opcodes. client->host requests/sends; host->client replies/streams.
enum class BridgeOp : std::uint8_t {
    // ---- client -> host ----
    Hello = 1,      ///< [u32 proto_version][bytes claimed_name][bytes credential] -- the handshake.
                    ///< The two trailing fields are read as empty when absent, so a v3-shaped
                    ///< Hello still identifies itself; the VERSION is what a host judges.
                    ///< The host answers Welcome, or Denied and severs.
    ListWeaves = 2, ///< (empty) -- explicit discovery refresh; the host replies Weaves
    Describe = 3,   ///< [bytes name][u32 version] -- the host replies Schema (encoded) or SchemaNone
    Send = 4,       ///< the connection's send (an assembled message, like Op::Emit):
                    ///<   [u8 kind][u64 wire_sender][u64 target][u64 wire_reply_to]
                    ///<   [u64 correlation][bytes role][bytes payload]
                    ///< The host re-admits `payload` through the gate and STAMPS sender + reply_to
                    ///< from the CONNECTION (the proxy's id), IGNORING wire_sender and wire_reply_to.
                    ///< An honest client sets those to 0; a malicious one forges them and the bridge
                    ///< stamps over them (the forge-the-hostile-frame pin). kind: kEmitSend to
                    ///< `target`, kEmitPublish, or kSendToRole to whoever holds `role` at delivery.

    // ---- host -> client ----
    Welcome = 16,    ///< [u64 session_id][u32 proto_version][bytes established_name]
    Weaves = 17,     ///< [u32 n]{[u64 id][u32 m]{[bytes name][u32 version]}} -- the live weave set
    Schema = 18,     ///< [bytes encoded_schema] (schema_codec) -- reply to Describe (found)
    SchemaNone = 19, ///< [bytes name][u32 version] -- reply to Describe (no such registered shape)
    Delivered = 20,  ///< [u64 sender][u64 correlation][u8 flags][bytes authored_role][bytes payload]
                     ///< -- a message the bus delivered to this connection's proxy. `flags`:
                     ///< kDeliveredAnswersAsk (Loom attests this as THE answer to an ask this
                     ///< connection sent), kDeliveredDispatchRefused (a `zen.DispatchRefused`
                     ///< notice about one of this connection's own sends).
    Tap = 21,        ///< a copied bus event, for a connection admitted WITH observation:
                     ///<   [u8 kind][u64 target][u64 sender][bytes schema][u32 version][bytes refusal]
    SendRefused = 22, ///< [u64 correlation][bytes reason] -- the connection's Send was dropped
                      ///< BEFORE the bus (malformed header / unknown schema / gate-refused / not
                      ///< admitted), so no bus event exists for it. Per-frame and NON-fatal;
                      ///< correlation is 0 when the header did not parse far enough to yield one.
    Denied = 23,      ///< [bytes reason] -- admission refused this connection. Sent once, flushed,
                      ///< and the connection is severed; nothing it sent acted and nothing it
                      ///< sends afterwards can.
};

/// Send kinds on the wire. The first two are the isolation protocol's own (kEmitSend = 0,
/// kEmitPublish = 1); the third is this crossing's addition.
inline constexpr std::uint8_t kSendToRole = 2;

/// `Delivered` flags.
inline constexpr std::uint8_t kDeliveredAnswersAsk = 1u << 0;
inline constexpr std::uint8_t kDeliveredDispatchRefused = 1u << 1;

/// Tap event kinds on the wire (mirrors loom::EventKind, fixed so the client need not link the bus).
inline constexpr std::uint8_t kTapDelivered = 0;
inline constexpr std::uint8_t kTapRefused = 1;
inline constexpr std::uint8_t kTapDied = 2;
inline constexpr std::uint8_t kTapRevived = 3;
/// The handler was entered and did not complete normally (RTH-1). NOT a refusal:
/// Loom declined nothing, and carries no reason to send - see loom::EventKind.
inline constexpr std::uint8_t kTapHandlerFailed = 4;

/// The crossing's protocol version (bumped on any wire change -- shape OR vocabulary; Hello and
/// Welcome carry it). v2 added SendRefused; v3 added the HandlerFailed tap kind; v4 is the
/// two-host crossing above: Hello's identity, Denied, Welcome's established name, Send's role
/// address and Delivered's stamped context. A host refuses a peer whose version it does not
/// speak, in words, before anything else happens on that connection.
inline constexpr std::uint32_t kBridgeProtocolVersion = 4;

/// The longest claimed name or credential a Hello may carry. A bound on what a peer can make a
/// host hold BEFORE it is admitted, when it has earned nothing yet.
inline constexpr std::size_t kMaxHelloFieldBytes = 256;

} // namespace loom

#endif // ZEN_BRIDGE_PROTOCOL_HPP
