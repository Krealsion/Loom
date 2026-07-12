#ifndef ZEN_WEAVE_STANDARD_SHAPES_HPP
#define ZEN_WEAVE_STANDARD_SHAPES_HPP

// The standard reply shapes: ONE vocabulary for the answers every protocol
// sends, so a reader recognizes a reply instantly instead of re-learning each
// protocol's dialect (the poke protocol had PokeAck/PokeValue/PokeRefused; the
// storage broker its StorageValue; the bridge its SendRefused — each protocol
// reinventing "done / here / no").
//
// THE RULE (the least-complete-information razor, applied to replies):
// standardize the contentless and simple-payload replies — ack / refusal /
// result; keep a bespoke reply type ONLY where the reply carries genuinely
// protocol-specific structure whose absence would break the image. Every reply
// type is either a standard shape or justified by breaks-on-absence — nothing
// in between. Test each field by its absence: if removing it leaves the reader
// *confused*, it is load-bearing; if merely *less-informed*, it is sediment.
//
// The three shapes, each carrying the least that is still complete:
//   - zen.Ack      carries NOTHING — "done." A reply's correlation already
//                  ties it to the request, so the correlation carries *what*
//                  was done; restating it ("PokeAck", an op field) is sediment.
//                  Zero fields IS the complete image.
//   - zen.Refused  carries ONE field — "no, and here is why." A refusal
//                  without its reason is an incomplete image (the reader
//                  cannot tell what to fix), and a refusal is an answer,
//                  never silence. The reason is written for a stranger:
//                  self-contained, naming what matters.
//   - zen.Result   carries THE PAYLOAD — "here is what you asked for," as
//                  text. The payload is the image.
//
// Naming, deliberately NOT Error/Value: loom::Error is the gate's admission
// error (a malformed claim — a fault); a zen.Refused is a deliberate answer
// by policy ("the access model says no"), and the two must not read as one
// thing. loom::Value is the substrate's value type itself; the reply shape is
// a Result. "Refused" is also the word the protocols had already converged on
// independently (PokeRefused, SendRefused) — this shape canonizes it.
//
// These are ordinary shapes: registered, gated, content-id'd like any other.
// Registration blocks are hand-written (not ZEN_SHAPE) so the wire names carry
// the substrate's "zen." prefix, which #ShapeName cannot produce — a maker's
// own struct named Ack derives "Ack", no collision. Every use of loom::Ack in
// every protocol derives the same schema from the same struct, so the shared
// vocabulary is one content-id everywhere by construction.
//
// THE CONSUMER OBLIGATION (standing rule — the vocabulary is universal, so any
// granted participant can emit these shapes; what keeps that inert is YOU):
// a weave that Accepts a standard reply shape must match each arrival against
// its own outstanding requests — by correlation AND by bus-stamped sender
// (the answer must come from the weave you actually asked; the sender is
// stamped by the bus, so a third party cannot speak as it) — and must treat an
// unsolicited reply as plain data at best. loom::relay (relay.hpp) implements
// exactly this wall; a hand-written consumer owes the same two checks. The
// producer side is symmetric: replying with a standard shape is an ordinary
// emission — declare it in Emit<...> like any shape your code sends.
//
// (One honest note on the correlation the Ack argument leans on: it is real
// end-to-end on the wire — a poke answer echoes its request's correlation, a
// relay restores the asker's original — but the console's reply buffer today
// surfaces answers in FIFO arrival order and does not yet display it, so at
// that surface delivery order carries the tie. Correlation-in-the-buffer is a
// named console seam, not built here.)

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>

namespace loom {

/// Contentless success: "done." The correlation carries what was done.
struct Ack {
    using ZenSelf = Ack;
    static constexpr const char* zen_name = "zen.Ack";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// A refusal with its reason: "no, and here is why." An answer, never
/// silence. The reason is prose written for a stranger — self-contained.
struct Refused {
    std::string reason;
    using ZenSelf = Refused;
    static constexpr const char* zen_name = "zen.Refused";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(reason)); }
};

/// A result carrying its payload as text: "here is what you asked for."
/// Text is the transport of the protocols that use it (values cross the poke
/// boundary as exact, locale-free text); a reply whose payload is not text
/// stays bespoke by the rule above.
struct Result {
    std::string value;
    using ZenSelf = Result;
    static constexpr const char* zen_name = "zen.Result";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(value)); }
};

} // namespace loom

#endif // ZEN_WEAVE_STANDARD_SHAPES_HPP
