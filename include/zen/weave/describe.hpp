// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_DESCRIBE_HPP
#define ZEN_WEAVE_DESCRIBE_HPP

// The self-description door: ask a target WHAT SHAPES IT ACCEPTS, by message.
//
// Every woven Weave (WeaveBase) answers a fifth substrate door beside the four
// zen.Poke* ones —
//   zen.DescribeAccepted -> zen.AcceptedShapes
// — from the SAME vector the Switchboard captured as its doors at registration
// (Weave::accepted_schemas()), so the answer is the acceptance truth the gate
// later enforces and not a second store that can drift from it.
//
// THE SUBJECT IS THE ACCEPT-SET, WHICH IS THE OTHER HALF OF zen.PokeDescribe.
// A poke describes what a weave *is* (its state's fields); this describes what
// may be *said* to it (its doors). Neither derives the other, and the two are
// answered by the same construction layer under the same non-interceptability
// rule: a maker cannot list these shapes in Accept<...>, so a woven weave
// cannot lie about its own vocabulary.
//
// THE REQUEST IS FIELDLESS AND ADDRESSED TO THE TARGET. `send_to_role(role,
// DescribeAccepted{})` already says who is being asked; a `role` field would
// restate the envelope and would invite a directory service to answer for
// somebody else. The target owns the fact, so the target is asked directly.
//
// WHAT THE ANSWER MEANS, EXACTLY:
//
//     these are the (name, version) shapes I accepted when I answered
//
// It is a SNAPSHOT. It carries no promise about a later delivery: a role may be
// swapped, a weave replaced, an artifact unloaded. It is not a subscription and
// nothing invalidates it. And it is not authority — knowing that a door exists
// is not permission to walk through it; sending a discovered shape is gated by
// the asker's own grant exactly as it was before the asker knew the name.
//
// ROOTS ARE NOT DEPENDENCIES, and the wire keeps them apart in two lists:
//
//     accepted    the roots -- shapes that may actually be SENT to this target
//     referenced  the structural closure -- shapes that exist only so a root can
//                 be understood, in POST-ORDER (a schema's own references first)
//
// The closure is not decoration. `zen.SchemaDesc` names a nested message by
// (name, version) and `decode_schema` resolves that against a dependency
// Registry, so a consumer handed only the roots CANNOT decode a root that nests
// anything — measured, not assumed. Shipping the closure in the encoder's
// post-order is what makes one round trip self-sufficient for a stranger that
// never compiled against any of these shapes. The manifest (zen.Manifest, see
// kernel/schema_codec.hpp) solved the identical problem the identical way for
// the load path; this reuses its collect_referenced and its zen.SchemaDesc v1
// unchanged rather than inventing a second descriptor format.
//
// WHY NOT REUSE zen.Manifest ITSELF: it is the kernel's LOAD contract and its
// subject is wider — a required state descriptor, the capability ask, the
// claim-set. Those are facts a host reads when admitting an artifact, not facts
// a peer asked for; and binding a participant-tier protocol to the load
// contract would make every future manifest bump a discovery-protocol bump.

#include <zen/kernel/schema_codec.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

// ---- the protocol shapes ----------------------------------------------------
// Hand-written registration block (not ZEN_SHAPE) so the wire name carries the
// substrate's "zen." prefix, which #ShapeName cannot produce. A maker's own
// struct named DescribeAccepted derives "DescribeAccepted" — no collision.

/// Ask a weave which message shapes it accepts. Fieldless: the envelope already
/// names the target, and the target owns the answer.
struct DescribeAccepted {
    using ZenSelf = DescribeAccepted;
    static constexpr const char* zen_name = kDescribeAcceptedShapeName;
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The grammar of the answer. Hand-built rather than a ZEN_SHAPE because its
/// fields are lists of `zen.SchemaDesc v1` — an existing SchemaBuilder shape,
/// not a C++ struct with zen_fields(). Exactly how zen.Manifest carries the
/// same two sections.
///
/// `referenced` is optional so a target whose accepted shapes nest nothing (the
/// common case, and every scalar-only shape) sends the lean form; absent and
/// empty mean the same thing. `accepted` is required and is never empty for a
/// woven weave, which always carries at least the five substrate doors — so an
/// empty answer can never be confused with "this weave declines to describe
/// itself", which is expressed by there being no answer at all.
inline std::shared_ptr<const Schema> accepted_shapes_schema() {
    static const auto s = SchemaBuilder(kAcceptedShapesShapeName, 1)
                              .list("referenced", type_message(schema_desc_schema()),
                                    /*required=*/false)
                              .list("accepted", type_message(schema_desc_schema()))
                              .build();
    return s;
}

/// The one door every woven Weave adds to its accept-set for self-description.
inline std::vector<std::shared_ptr<const Schema>> describe_door_schemas() {
    return {schema_of<DescribeAccepted>()};
}

/// The one answer shape the construction layer emits when asked.
inline std::vector<std::shared_ptr<const Schema>> describe_answer_schemas() {
    return {accepted_shapes_schema()};
}

/// Allow a Weave's self-description ANSWER. The construction layer does the
/// answering, but the send is gated like any other — the grant stays the host's
/// sole authority. mount() adds this for a trusted Weave; a host using
/// mount_granted decides for itself (an ungranted Weave's answer is
/// CapabilityDenied at delivery, visible on the tap). The symmetric partner of
/// allow_poke_answers (poke.hpp), and deliberately a SEPARATE call: a host may
/// want a weave inspectable without it being self-describing, or the reverse.
inline Grant& allow_describe_answers(Grant& grant) {
    for (const auto& s : describe_answer_schemas()) {
        grant.allow_to_any(s->name(), s->version());
    }
    return grant;
}

// ---- encode (an accept-set -> the answer Value) ------------------------------

/// Build the answer from a weave's real accept-set: the roots verbatim, plus
/// the transitive structural closure their fields reference, deduplicated by
/// (name, version) and emitted in the post-order `collect_referenced`
/// guarantees.
///
/// A root that is ALSO a dependency of another root appears in both lists, and
/// that is truthful rather than redundant — it genuinely is both. Registering it
/// twice on the consumer side is an identical re-registration, which the
/// Registry treats as a no-op.
///
/// Cannot fail for want of resolution: it walks the `TypeRef::message` pointers
/// the schemas already hold, so nothing is looked up and nothing can be missing.
/// A null entry would be a broken accept-set, which register_weave already
/// refuses before any weave can be asked anything.
inline Value encode_accepted_shapes(
    const std::vector<std::shared_ptr<const Schema>>& accepted) {
    Value v(accepted_shapes_schema());

    std::vector<std::shared_ptr<const Schema>> referenced;
    for (const auto& s : accepted) {
        collect_referenced(*s, referenced);
    }
    if (!referenced.empty()) {
        std::vector<Cell> refs;
        refs.reserve(referenced.size());
        for (const auto& s : referenced) {
            refs.push_back(Cell::message(encode_schema(*s)));
        }
        v.set("referenced", Cell::list(std::move(refs)));
    }

    std::vector<Cell> roots;
    roots.reserve(accepted.size());
    for (const auto& s : accepted) {
        roots.push_back(Cell::message(encode_schema(*s)));
    }
    v.set("accepted", Cell::list(std::move(roots)));
    return v;
}

// ---- decode (the answer Value -> Schemas a stranger can inspect) -------------
//
// Precondition: `answer` has passed the gate against accepted_shapes_schema(),
// so its declared shape is sound. Reconstruction can still fail semantically (a
// hand-built answer with a mis-ordered closure, a type nested past the codec's
// depth cap) — those throw, exactly as decode_schema does, and a consumer turns
// them into its own refusal rather than into a half-built vocabulary.

/// Register the answer's dependency closure into `deps`, in order. The
/// encoder's post-order guarantee is what makes one forward pass sufficient:
/// entry N+1's type tokens are resolved against what entries 0..N already put
/// there. The mirror of decode_referenced (schema_codec.hpp) for this reply.
inline void decode_accepted_referenced(const Value& answer, Registry& deps) {
    const Cell* refs = answer.get("referenced");
    if (refs == nullptr) {
        return; // nothing nested, nothing to do
    }
    for (const Cell& c : refs->as_list()) {
        deps.register_schema(decode_schema(*c.as_message(), deps));
    }
}

/// Reconstruct the ACCEPTED ROOTS — the shapes that may actually be sent to the
/// target — resolving nested references against `deps`. Call
/// decode_accepted_referenced first, or a root that nests anything will not
/// resolve.
///
/// Returns them in the target's own declaration order, which is the order its
/// Accept<...> named them followed by the substrate doors. That order is stable
/// for a given target and carries no meaning beyond being stable.
inline std::vector<std::shared_ptr<const Schema>> decode_accepted_roots(const Value& answer,
                                                                       const Registry& deps) {
    std::vector<std::shared_ptr<const Schema>> out;
    const Cell::Array& roots = answer.get("accepted")->as_list();
    out.reserve(roots.size());
    for (const Cell& c : roots) {
        out.push_back(decode_schema(*c.as_message(), deps));
    }
    return out;
}

} // namespace loom

#endif // ZEN_WEAVE_DESCRIBE_HPP
