// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// zen-weave-host: the child process that runs one Weave out of process. It drives the kernel C
// ABI directly (load the .so, get its descriptor, call the same thunks) and bridges to the
// parent over a framed socket; the Weave's outbound Bus ships Emit frames and gating happens in
// the parent. The .so does not know it is out of process. I/O here blocks: the child has
// nothing to do but serve the parent.

#include <zen/isolation/protocol.hpp>
#include <zen/kernel/abi.h>
#include <zen/kernel/descriptor.hpp>
#include <zen/kernel/making_words.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace loom;

int g_sock = -1;

bool write_all(const char* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        const ssize_t w = ::send(g_sock, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += static_cast<std::size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void send_frame(Op op, std::string_view payload) {
    std::string frame;
    put_u32(frame, static_cast<std::uint32_t>(payload.size()));
    put_u8(frame, static_cast<std::uint8_t>(op));
    frame.append(payload);
    (void)write_all(frame.data(), frame.size()); // if the parent is gone we exit on read EOF
}

bool read_exact(char* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        const ssize_t r = ::recv(g_sock, p + off, n - off, 0);
        if (r > 0) {
            off += static_cast<std::size_t>(r);
        } else if (r == 0) {
            return false; // EOF: parent closed
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool read_frame(Op& op, std::string& payload) {
    char header[5];
    if (!read_exact(header, sizeof(header))) {
        return false;
    }
    Cursor cursor(std::string_view(header, sizeof(header)));
    std::uint32_t len = 0;
    std::uint8_t op_value = 0;
    (void)cursor.u32(len);
    (void)cursor.u8(op_value);
    if (len > kMaxFrameLen) {
        return false;
    }
    payload.assign(len, '\0');
    if (len > 0 && !read_exact(payload.data(), len)) {
        return false;
    }
    op = static_cast<Op>(op_value);
    return true;
}

void sink_to_string(void* ctx, const std::uint8_t* data, std::size_t len) {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
}

ZenStatus ship_emit(std::uint8_t kind, std::uint64_t target, std::uint64_t reply_to,
                    std::uint64_t correlation, const std::uint8_t* payload, std::size_t len) {
    std::string out;
    put_u8(out, kind);
    put_u64(out, target);
    put_u64(out, reply_to);
    put_u64(out, correlation);
    out.append(reinterpret_cast<const char*>(payload), len); // raw trailing message bytes
    send_frame(Op::Emit, out);
    return ZEN_OK;
}

ZenStatus ship_emit_role(const char* role, std::uint64_t reply_to, std::uint64_t correlation,
                         const std::uint8_t* payload, std::size_t len) {
    std::string out;
    put_bytes(out, std::string_view(role)); // length-prefixed role; NO sender on the wire
    put_u64(out, reply_to);
    put_u64(out, correlation);
    out.append(reinterpret_cast<const char*>(payload), len); // raw trailing message bytes
    send_frame(Op::EmitRole, out);
    return ZEN_OK;
}

bool emit_snapshot(const ZenWeaveAbi* abi, void* instance) {
    std::string snap;
    ZenByteSink sink{&snap, &sink_to_string};
    if (abi->snapshot(instance, sink) != ZEN_OK) {
        return false;
    }
    send_frame(Op::Snapshot, snap);
    return true;
}

} // namespace

extern "C" {
static ZenStatus zen_child_send(void* ctx, std::uint64_t target, std::uint64_t reply_to,
                                std::uint64_t correlation, const std::uint8_t* payload,
                                std::size_t len, std::uint64_t* attempt_out) {
    if (attempt_out != nullptr) { *attempt_out = 0; }
    (void)ctx;
    return ship_emit(kEmitSend, target, reply_to, correlation, payload, len);
}
static ZenStatus zen_child_publish(void* ctx, std::uint64_t reply_to, std::uint64_t correlation,
                                   const std::uint8_t* payload, std::size_t len) {
    (void)ctx;
    return ship_emit(kEmitPublish, 0, reply_to, correlation, payload, len);
}
static ZenStatus zen_child_send_to_role(void* ctx, const char* role, std::uint64_t reply_to,
                                        std::uint64_t correlation, const std::uint8_t* payload,
                                        std::size_t len, std::uint64_t* attempt_out) {
    if (attempt_out != nullptr) { *attempt_out = 0; }
    (void)ctx;
    return ship_emit_role(role, reply_to, correlation, payload, len);
}
} // extern "C"

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    g_sock = std::atoi(argv[1]);
    const char* so_path = argv[2];

    void* lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        return 3;
    }
    void* sym = ::dlsym(lib, "zen_weave_abi");
    if (sym == nullptr) {
        return 4;
    }
    using AbiFn = const ZenWeaveAbi* (*)(void);
    AbiFn abi_fn = nullptr;
    std::memcpy(&abi_fn, &sym, sizeof(abi_fn));
    const ZenWeaveAbi* exported = abi_fn();
    // Read as the parent's own kernel reads one (zen/kernel/descriptor.hpp): an image it would
    // refuse is refused here before any slot of it runs.
    ZenWeaveAbi descriptor{};
    std::string why;
    if (exported == nullptr || !loom::detail::read_descriptor(exported, descriptor, why)) {
        return 5;
    }
    const ZenWeaveAbi* abi = &descriptor;
    // What it is made with, said to the parent before anything of the weave runs; the parent
    // answers with the value it admitted against that shape, or with none.
    std::string declared;
    if (abi->describe_creation != nullptr) {
        ZenByteSink ds{&declared, &sink_to_string};
        if (abi->describe_creation(ds) != ZEN_OK) {
            return 7;
        }
    }
    {
        std::string said;
        put_bytes(said, declared);
        send_frame(Op::Creation, said);
    }
    std::string made_with;
    bool with = false;
    {
        Op op = Op::Make;
        std::string payload;
        if (!read_frame(op, payload) || op != Op::Make) {
            return 8;
        }
        Cursor cursor(payload);
        std::uint8_t has = 0;
        std::string_view value;
        if (!cursor.u8(has) || !cursor.bytes(value)) {
            return 8;
        }
        with = has != 0;
        made_with.assign(value);
    }
    loom::MakingWords words;
    void* instance = nullptr;
    if (!with) {
        instance = abi->create(words.sink());
    } else if (abi->create_with != nullptr) {
        instance = abi->create_with(words.sink(),
                                    reinterpret_cast<const std::uint8_t*>(made_with.data()),
                                    made_with.size());
    }
    if (instance == nullptr) {
        // ...SAID TO THE PARENT before the exit, so its refusal is the weave's own words.
        std::string refused;
        put_u8(refused, words.cut ? 1 : 0);
        put_bytes(refused, words.said);
        send_frame(Op::Refused, refused);
        return 6;
    }

    // Handshake: Hello = manifest | policy | snapshot.
    {
        std::string manifest;
        std::string policy;
        std::string snapshot;
        ZenByteSink ms{&manifest, &sink_to_string};
        ZenByteSink ps{&policy, &sink_to_string};
        ZenByteSink ss{&snapshot, &sink_to_string};
        if (abi->describe(instance, ms) != ZEN_OK || abi->policy(instance, ps) != ZEN_OK ||
            abi->snapshot(instance, ss) != ZEN_OK) {
            abi->destroy(instance);
            return 7;
        }
        std::string hello;
        put_bytes(hello, manifest);
        put_bytes(hello, policy);
        put_bytes(hello, snapshot);
        send_frame(Op::Hello, hello);
    }

    // The pipe carries no attestation and no verified identity, so every door that would need
    // one is null and fails closed at the library shim: answers (the answer authority lives in
    // the parent's delivery), role authorship (never a silent downgrade to personal speech),
    // and Sense claims, reads and offers (a child cannot present the parent's grant or identity).
    // A verified role frame on this pipe is the seam an out-of-process role would need.
    // By name, in declaration order (KERN-04): a designator is the comment the compiler reads.
    ZenHostApi api{.size                 = sizeof(ZenHostApi),
                   .ctx                  = nullptr,
                   .send                 = &zen_child_send,
                   .publish              = &zen_child_publish,
                   .send_to_role         = &zen_child_send_to_role,
                   .defer_answer         = nullptr,
                   .answer_deferred      = nullptr,
                   .release_deferred     = nullptr,
                   .answer               = nullptr,
                   .role_send          = nullptr,
                   .role_send_to_role  = nullptr,
                   .role_publish       = nullptr,
                   .sense_claim          = nullptr,
                   .sense_role_claim   = nullptr,
                   .sense_observe        = nullptr,
                   .sense_observe_role = nullptr,
                   // the isolated pipe supplies no offer door, as it supplies no claim or
                   // observe door: a child cannot present the identity a joint operation binds
                   .sense_offer          = nullptr,
                   // ...and no ask door: the pipe carries no attestation back, so an ask's end
                   // would have nowhere to go (ANS-08)
                   .ask                  = nullptr};

    for (;;) {
        Op op = Op::Hello;
        std::string payload;
        if (!read_frame(op, payload)) {
            break; // parent gone
        }
        Cursor cursor(payload);
        if (op == Op::Deliver) {
            std::string_view facts;
            std::uint64_t sender = 0;
            std::uint64_t reply_to = 0;
            std::uint64_t correlation = 0;
            std::string_view addressed;
            if (!cursor.bytes(facts)) {
                break;
            }
            // What this child knows of the facts, in order; what follows is a later fact.
            Cursor known(facts);
            if (!known.u64(sender) || !known.u64(reply_to) || !known.u64(correlation) ||
                !known.bytes(addressed)) {
                break;
            }
            // The turn's time, when the parent appended it: a flag, then both readings.
            std::uint8_t told = 0;
            std::uint64_t turn_ms = 0;
            std::uint64_t turn_date_ms = 0;
            if (known.u8(told) && told == 1 && !(known.u64(turn_ms) && known.u64(turn_date_ms))) {
                told = 0;
            }
            std::string_view bytes = cursor.rest();
            const std::string addressed_role(addressed); // NUL-terminated for the seam
            // Provenance does not cross this pipe: a Deliver frame carries no attestation, so an
            // out-of-process weave is told nothing here is attested, and never adopts an
            // authenticated answer, an attested activation or an authored role (the NULL role
            // below) that the parent could not vouch for across it. The addressed role is no
            // attestation: it says where the parent delivered, and crosses.
            const ZenDelivery delivery{
                .size = sizeof(ZenDelivery),
                .provenance = ZEN_PROV_NONE,
                .sender = sender,
                .reply_to = reply_to,
                .correlation = correlation,
                .attested_sequence = 0,
                .authored_role = nullptr,
                .addressed_role = addressed_role.empty() ? nullptr : addressed_role.c_str(),
                .turn = told == 1 ? std::uint32_t{ZEN_TURN_KNOWN} : 0u,
                .turn_ms = told == 1 ? static_cast<std::int64_t>(turn_ms) : 0,
                .turn_date_ms = told == 1 ? static_cast<std::int64_t>(turn_date_ms) : 0};
            abi->handle(instance, &delivery, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        bytes.size(), &api);
            (void)emit_snapshot(abi, instance);
        } else if (op == Op::Revive) {
            std::string_view state = cursor.rest();
            (void)abi->revive(instance, reinterpret_cast<const std::uint8_t*>(state.data()),
                              state.size());
            (void)emit_snapshot(abi, instance);
        } else if (op == Op::Shutdown) {
            break;
        }
        // unknown ops are ignored
    }

    abi->destroy(instance);
    ::dlclose(lib);
    return 0;
}
