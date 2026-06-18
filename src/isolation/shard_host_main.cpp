// zen-shard-host: the child process that runs one Shard out-of-process. It reuses
// the kernel C ABI directly — load the .so, get its descriptor, drive the same
// create/describe/snapshot/policy/revive/handle thunks — and bridges to the parent
// over a framed socket. The Shard's outbound Bus is a stub: its send/publish ship
// Emit frames; gating happens parent-side. The .so is unchanged and does not know
// it is out-of-process.
//
// I/O here is blocking — the child has nothing to do but service the parent — so
// the async/non-blocking machinery lives only on the host side.

#include <zen/isolation/protocol.hpp>
#include <zen/kernel/abi.h>

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

using namespace zen::isolation;

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

bool emit_snapshot(const ZenShardAbi* abi, void* instance) {
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
                                std::size_t len) {
    (void)ctx;
    return ship_emit(kEmitSend, target, reply_to, correlation, payload, len);
}
static ZenStatus zen_child_publish(void* ctx, std::uint64_t reply_to, std::uint64_t correlation,
                                   const std::uint8_t* payload, std::size_t len) {
    (void)ctx;
    return ship_emit(kEmitPublish, 0, reply_to, correlation, payload, len);
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
    void* sym = ::dlsym(lib, "zen_shard_abi");
    if (sym == nullptr) {
        return 4;
    }
    using AbiFn = const ZenShardAbi* (*)(void);
    AbiFn abi_fn = nullptr;
    std::memcpy(&abi_fn, &sym, sizeof(abi_fn));
    const ZenShardAbi* abi = abi_fn();
    if (abi == nullptr || abi->abi_version != ZEN_ABI_VERSION) {
        return 5;
    }
    void* instance = abi->create();
    if (instance == nullptr) {
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

    ZenHostApi api{nullptr, &zen_child_send, &zen_child_publish};

    for (;;) {
        Op op = Op::Hello;
        std::string payload;
        if (!read_frame(op, payload)) {
            break; // parent gone
        }
        Cursor cursor(payload);
        if (op == Op::Deliver) {
            std::uint64_t sender = 0;
            std::uint64_t reply_to = 0;
            std::uint64_t correlation = 0;
            if (!cursor.u64(sender) || !cursor.u64(reply_to) || !cursor.u64(correlation)) {
                break;
            }
            std::string_view bytes = cursor.rest();
            abi->handle(instance, sender, reply_to, correlation,
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &api);
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
