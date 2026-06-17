#ifndef ZEN_KERNEL_EXPORT_HPP
#define ZEN_KERNEL_EXPORT_HPP

// The authoring layer. A library author writes a clean C++ zen::sb::Shard
// subclass and adds one line — ZEN_EXPORT_SHARD(MyShard) — and the macro
// generates the whole C ABI: the descriptor, every thunk, and the single
// exported symbol. No author hand-writes a thunk, and Zen stays invisible: the
// same Shard they would compile in is the same Shard they ship in a .so.
//
// The thunks bridge C <-> C++: they serialize Values to bytes for the host's
// ByteSink, rebuild a Bus that forwards send/publish across the host callback
// table, and never let a C++ exception cross the seam (everything is caught and
// turned into a status code).

#include <zen/kernel/abi.h>
#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard/shard.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define ZEN_KERNEL_EXPORT __attribute__((visibility("default")))
#else
#define ZEN_KERNEL_EXPORT
#endif

namespace zen::kernel::detail {

inline void sink_write(ZenByteSink sink, const std::string& bytes) {
    if (sink.write != nullptr) {
        sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
}

inline std::string_view as_view(const std::uint8_t* data, std::size_t len) {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

// A Bus implementation that lives inside the library and forwards a Shard's
// send/publish across the host callback table as serialized payload bytes. The
// host assigns the real sender id and admits the bytes through the gate before
// routing; the library-side Ticket/count are therefore not meaningful here.
class HostApiBus final : public zen::sb::Bus {
public:
    explicit HostApiBus(const ZenHostApi* host) : host_(host) {}

    zen::sb::Ticket send(zen::sb::ShardId target, zen::sb::Message msg) override {
        const std::string bytes = zen::serialize(msg.payload);
        if (host_ != nullptr && host_->send != nullptr) {
            host_->send(host_->ctx, target.value, msg.reply_to.value, msg.correlation,
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        return zen::sb::Ticket{};
    }

    std::size_t publish(zen::sb::Message msg) override {
        const std::string bytes = zen::serialize(msg.payload);
        if (host_ != nullptr && host_->publish != nullptr) {
            host_->publish(host_->ctx, msg.reply_to.value, msg.correlation,
                           reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        return 0;
    }

private:
    const ZenHostApi* host_;
};

// ---- the per-method helpers the generated thunks forward to ------------------

template <class S>
void* do_create() {
    try {
        return static_cast<void*>(new S());
    } catch (...) {
        return nullptr;
    }
}

template <class S>
void do_destroy(void* instance) {
    delete static_cast<S*>(instance);
}

template <class S>
ZenStatus do_describe(void* instance, ZenByteSink sink) {
    try {
        S* s = static_cast<S*>(instance);
        std::vector<std::shared_ptr<const zen::Schema>> accepted = s->accepted_schemas();
        zen::Value state = s->snapshot();
        sink_write(sink, zen::serialize(zen::kernel::encode_manifest(accepted, state.schema())));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_snapshot(void* instance, ZenByteSink sink) {
    try {
        sink_write(sink, zen::serialize(static_cast<S*>(instance)->snapshot()));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_policy(void* instance, ZenByteSink sink) {
    try {
        sink_write(sink, zen::serialize(static_cast<S*>(instance)->policy()));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_revive(void* instance, const std::uint8_t* state, std::size_t len) {
    try {
        S* s = static_cast<S*>(instance);
        zen::Unverified u = zen::parse(as_view(state, len));
        // The host already admitted these bytes; the library re-admits against its
        // own state schema only to rebuild the Value its C++ revive() expects.
        zen::Value probe = s->snapshot();
        zen::Admission a = zen::admit(u, probe.schema_ptr());
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        s->revive(a.value());
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_handle(void* instance, std::uint64_t sender, std::uint64_t reply_to,
                    std::uint64_t correlation, const std::uint8_t* payload, std::size_t len,
                    const ZenHostApi* host) {
    try {
        S* s = static_cast<S*>(instance);
        zen::Unverified u = zen::parse(as_view(payload, len));
        std::shared_ptr<const zen::Schema> door;
        for (auto& sc : s->accepted_schemas()) {
            if (sc->name() == u.claimed_name() && sc->version() == u.claimed_version()) {
                door = sc;
                break;
            }
        }
        if (!door) {
            return ZEN_ERR_UNKNOWN_SCHEMA;
        }
        zen::Admission a = zen::admit(u, door);
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        zen::sb::Message msg(a.value(), zen::sb::ShardId{sender}, zen::sb::ShardId{reply_to},
                             correlation);
        HostApiBus bus(host);
        s->handle(msg, bus);
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

} // namespace zen::kernel::detail

// Generate the C ABI for a Shard class. The thunks have C language linkage (to
// match the descriptor's function-pointer types) and forward to the C++ helpers
// above. Exactly one ZEN_EXPORT_SHARD per library.
#define ZEN_EXPORT_SHARD(ShardClass)                                                                \
    extern "C" {                                                                                    \
    static void* zen__abi_create(void) { return ::zen::kernel::detail::do_create<ShardClass>(); }   \
    static void zen__abi_destroy(void* i) { ::zen::kernel::detail::do_destroy<ShardClass>(i); }      \
    static ZenStatus zen__abi_describe(void* i, ZenByteSink s) {                                    \
        return ::zen::kernel::detail::do_describe<ShardClass>(i, s);                                \
    }                                                                                              \
    static ZenStatus zen__abi_snapshot(void* i, ZenByteSink s) {                                   \
        return ::zen::kernel::detail::do_snapshot<ShardClass>(i, s);                                \
    }                                                                                              \
    static ZenStatus zen__abi_policy(void* i, ZenByteSink s) {                                     \
        return ::zen::kernel::detail::do_policy<ShardClass>(i, s);                                  \
    }                                                                                              \
    static ZenStatus zen__abi_revive(void* i, const uint8_t* st, size_t n) {                       \
        return ::zen::kernel::detail::do_revive<ShardClass>(i, st, n);                              \
    }                                                                                              \
    static ZenStatus zen__abi_handle(void* i, uint64_t sender, uint64_t reply_to,                  \
                                     uint64_t correlation, const uint8_t* p, size_t n,             \
                                     const ZenHostApi* h) {                                         \
        return ::zen::kernel::detail::do_handle<ShardClass>(i, sender, reply_to, correlation, p, n, \
                                                            h);                                    \
    }                                                                                              \
    ZEN_KERNEL_EXPORT const ZenShardAbi* zen_shard_abi(void) {                                      \
        static const ZenShardAbi abi = {ZEN_ABI_VERSION, zen__abi_create,   zen__abi_destroy,      \
                                        zen__abi_describe, zen__abi_snapshot, zen__abi_policy,       \
                                        zen__abi_revive,   zen__abi_handle};                        \
        return &abi;                                                                               \
    }                                                                                              \
    }

#endif // ZEN_KERNEL_EXPORT_HPP
