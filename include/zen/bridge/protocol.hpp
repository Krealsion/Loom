// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_PROTOCOL_HPP
#define ZEN_BRIDGE_PROTOCOL_HPP

// The crossing's wire: what one Loom host says to another over a framed socket.
// docs/reference/bridge.md
//
// A `Switchboard&` cannot cross a socket, so a small fixed vocabulary of frames does: a
// handshake the host answers with admission or refusal, discovery the host answers, a send the
// host re-admits through the one gate and stamps from the connection, a delivery the host ships
// with the facts Loom stamped on it, and, for a connection the host's policy lets observe, a
// copy of each bus event. Frames are [u32 payload_len][u8 op][payload], little-endian, read
// through the bounds-checked Cursor of <zen/wire.hpp> (shared with the isolation protocol), so
// a hostile or truncated frame is rejected, never over-read.
//
//   Hello      a claimed name and a credential: data the host's admission policy judges, never
//              the identity it establishes, which comes back on Welcome.
//   Denied     the host's refusal and its reason, before the connection is severed. A refused
//              connection has no proxy and acts on nothing.
//   Welcome    the session identity the host minted (the proxy's WeaveId) and the name its
//              policy established (empty when none).
//   Send       to a WeaveId, to an office (`role`) or published; it may ask to be told when
//              what it set in motion on the host's bus has been dispatched.
//   Delivered  the payload and what Loom stamped beside it: the sender, the correlation,
//              whether Loom attests it as the answer to an ask this connection sent, whether it
//              is a dispatch-refusal notice, and the office the sender spoke as.
//   Settled    the host's word, once, that everything a settle-requested Send set in motion
//              has been dispatched (`loom::Fence`). Not an answer, and not a claim that nothing
//              else is pending: work deferred to a timer or a later turn is not waited for.
//   Tap        copied only to a connection whose admission verdict grants observation.
//
// A Send's payload is serialized message bytes the host re-admits through the one gate, and
// the host stamps its sender from the connection, never the wire.

#include <zen/wire.hpp> // put_u8/u32/u64, put_bytes, Cursor, kMaxFrameLen, kEmitSend/kEmitPublish

#include <cstdint>

namespace loom {

/// The crossing's opcodes. client->host requests/sends; host->client replies/streams.
enum class BridgeOp : std::uint8_t {
    // ---- client -> host ----
    Hello = 1,      ///< [u32 proto_version][bytes claimed_name][bytes credential] -- the handshake.
                    ///< The two trailing fields read as empty when absent, so a shorter
                    ///< Hello still parses; the VERSION is what a host judges.
                    ///< The host answers Welcome, or Denied and severs.
    ListWeaves = 2, ///< (empty) -- explicit discovery refresh; the host replies Weaves
    Describe = 3,   ///< [bytes name][u32 version] -- the host replies Schema (encoded) or SchemaNone
    Send = 4,       ///< the connection's send (an assembled message, like Op::Emit):
                    ///<   [u8 kind][u8 flags][u64 wire_sender][u64 target][u64 wire_reply_to]
                    ///<   [u64 correlation][bytes role][bytes payload]
                    ///< `flags`: kSendSettle asks for one Settled under `correlation` once what
                    ///< the send set in motion has been dispatched.
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
    Settled = 24,     ///< [u64 correlation] -- everything the settle-requested Send under this
                      ///< correlation set in motion on the host's bus has been dispatched. Once
                      ///< per such Send that reached the bus; a Send refused before the bus
                      ///< (SendRefused) has nothing to settle and is told nothing more.
};

/// Send kinds on the wire. The first two are the isolation protocol's own (kEmitSend and
/// kEmitPublish); the third is this crossing's addition.
inline constexpr std::uint8_t kSendToRole = 2;

/// `Send` flags. A publication cannot ask for settlement (it has no one envelope to fence), and
/// a host refuses the Send before the bus when it cannot track one more.
inline constexpr std::uint8_t kSendSettle = 1u << 0;

/// `Delivered` flags.
inline constexpr std::uint8_t kDeliveredAnswersAsk = 1u << 0;
inline constexpr std::uint8_t kDeliveredDispatchRefused = 1u << 1;

/// Tap event kinds on the wire (mirrors loom::EventKind, fixed so the client need not link the bus).
inline constexpr std::uint8_t kTapDelivered = 0;
inline constexpr std::uint8_t kTapRefused = 1;
inline constexpr std::uint8_t kTapDied = 2;
inline constexpr std::uint8_t kTapRevived = 3;
/// The handler was entered and did not complete normally. Not a refusal: Loom declined
/// nothing and has no reason to send (loom::EventKind).
inline constexpr std::uint8_t kTapHandlerFailed = 4;

/// The crossing's protocol version, raised on any wire change of shape or vocabulary; Hello and
/// Welcome carry it. A host refuses a peer whose version it does not speak, in words, before
/// anything else happens on that connection.
inline constexpr std::uint32_t kBridgeProtocolVersion = 4;

/// The longest claimed name or credential a Hello may carry. A bound on what a peer can make a
/// host hold BEFORE it is admitted, when it has earned nothing yet.
inline constexpr std::size_t kMaxHelloFieldBytes = 256;

} // namespace loom

#endif // ZEN_BRIDGE_PROTOCOL_HPP
