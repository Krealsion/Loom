// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_DESCRIBE_HPP
#define ZEN_WEAVE_DESCRIBE_HPP

// The self-description door: ask a target by message which shapes it accepts. Every woven Weave
// answers zen.DescribeAccepted with zen.AcceptedShapes, built from Weave::accepted_schemas(),
// the vector the Switchboard matches deliveries against. zen.PokeDescribe says what a weave is;
// this says what may be said to it. Answered by the construction layer, never by the weaver.
// docs/reference/messaging.md#self-description--what-may-be-said-to-this-weave
//
// The request has no fields: the envelope names the target, and the target owns the answer.
// The answer is a snapshot of what the answering weave accepted, not a subscription, and not
// authority: sending a discovered shape is still gated by the asker's grant.
//
// Two lists: `accepted`, the roots that may be sent, and `referenced`, their structural closure
// in post-order, without which a consumer cannot decode a root that nests anything. It reuses
// the manifest's collect_referenced and zen.SchemaDesc v1 rather than zen.Manifest itself,
// which is the load contract and carries facts for a host, not a peer.

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
// Registered by hand so the wire name carries the "zen." prefix; a weaver's own struct named
// DescribeAccepted is "DescribeAccepted", with no collision.

/// Ask a weave which message shapes it accepts. Fieldless: the envelope already
/// names the target, and the target owns the answer.
struct DescribeAccepted {
    using ZenSelf = DescribeAccepted;
    static constexpr const char* zen_name = kDescribeAcceptedShapeName;
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The answer's grammar, hand-built because its fields are lists of `zen.SchemaDesc v1`, as
/// zen.Manifest's are. `referenced` is optional; absent and empty mean the same. `accepted` is
/// never empty for a woven weave, which has at least the five substrate doors; a weave that
/// declines to describe itself sends no answer.
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

/// Allow a Weave's self-description answer, which is gated like any send. mount() adds it for
/// a trusted Weave; under mount_granted an ungranted answer is `CapabilityDenied` at delivery.
/// Separate from allow_poke_answers (poke.hpp), so either can be allowed alone.
inline Grant& allow_describe_answers(Grant& grant) {
    for (const auto& s : describe_answer_schemas()) {
        grant.allow_to_any(s->name(), s->version());
    }
    return grant;
}

// ---- encode (an accept-set -> the answer Value) ------------------------------

/// Build the answer from a weave's accept-set: the roots as they are, and the closure their
/// fields reference, deduplicated by identity, in the post-order `collect_referenced` gives. A
/// root that is also another root's dependency is in both lists, and registering it twice is a
/// no-op. It cannot fail to resolve: it follows the pointers the schemas already hold.
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
// Precondition: `answer` passed the gate against accepted_shapes_schema(). A mis-ordered closure
// or a type nested past the codec's depth cap still throws, as decode_schema does.

/// Register the answer's closure into `deps`, in order: one forward pass, as for
/// decode_referenced (schema_codec.hpp).
inline void decode_accepted_referenced(const Value& answer, Registry& deps) {
    const Cell* refs = answer.get("referenced");
    if (refs == nullptr) {
        return; // nothing nested, nothing to do
    }
    for (const Cell& c : refs->as_list()) {
        deps.register_schema(decode_schema(*c.as_message(), deps));
    }
}

/// Reconstruct the accepted roots, the shapes that may be sent to the target, resolving nested
/// references against `deps`; call decode_accepted_referenced first. In the target's own
/// order: its Accept<...> list, then the substrate doors.
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
