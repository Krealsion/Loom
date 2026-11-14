// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// Compiled into every weave `loom_wasm_weave()` builds as a wasm32 module, beside the weave and
// its ZEN_EXPORT_WEAVE line. It turns the weave's `ZenWeaveAbi` into flat exports (`zw_*`) a host
// can call, and builds the `ZenHostApi` the weave sees out of imports (module `zen_host`), one
// per slot. Every pointer here is an offset into the module's own linear memory: the host copies
// bytes in and reads spans it has checked, and never hands the module an address of its own.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include <zen/kernel/abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define LOOM_WASM_IMPORT(name) __attribute__((import_module("zen_host"), import_name(#name)))
#define LOOM_WASM_EXPORT(name) __attribute__((export_name(#name)))

extern "C" {

// ---- imports: the host's doors, one per ZenHostApi slot, and the host's byte sink -------------
LOOM_WASM_IMPORT(sink_write) void zh_sink_write(int32_t sink, const uint8_t* data, size_t len);
LOOM_WASM_IMPORT(send) int32_t zh_send(uint64_t target, uint64_t reply_to, uint64_t correlation,
                                       const uint8_t* p, size_t n, uint64_t* attempt_out);
LOOM_WASM_IMPORT(publish) int32_t zh_publish(uint64_t reply_to, uint64_t correlation,
                                             const uint8_t* p, size_t n);
LOOM_WASM_IMPORT(send_to_role) int32_t zh_send_to_role(const char* role, uint64_t reply_to,
                                                       uint64_t correlation, const uint8_t* p,
                                                       size_t n, uint64_t* attempt_out);
LOOM_WASM_IMPORT(defer_answer) uint64_t zh_defer_answer(void);
LOOM_WASM_IMPORT(answer_deferred) int32_t zh_answer_deferred(uint64_t token, const uint8_t* p,
                                                             size_t n);
LOOM_WASM_IMPORT(release_deferred) void zh_release_deferred(uint64_t token);
LOOM_WASM_IMPORT(answer) int32_t zh_answer(const uint8_t* p, size_t n);
LOOM_WASM_IMPORT(role_send) int32_t zh_role_send(const char* as_role, uint64_t target,
                                                     uint64_t reply_to, uint64_t correlation,
                                                     const uint8_t* p, size_t n,
                                                     uint64_t* attempt_out);
LOOM_WASM_IMPORT(role_send_to_role) int32_t
zh_role_send_to_role(const char* as_role, const char* to_role, uint64_t reply_to,
                       uint64_t correlation, const uint8_t* p, size_t n, uint64_t* attempt_out);
LOOM_WASM_IMPORT(role_publish) int32_t zh_role_publish(const char* as_role, uint64_t reply_to,
                                                           uint64_t correlation, const uint8_t* p,
                                                           size_t n, uint64_t* recipients_out);
LOOM_WASM_IMPORT(sense_claim) int32_t zh_sense_claim(const uint8_t* p, size_t n,
                                                     uint64_t* revision_out);
LOOM_WASM_IMPORT(sense_role_claim) int32_t zh_sense_role_claim(const char* as_role,
                                                                   const uint8_t* p, size_t n,
                                                                   uint64_t* revision_out);
// The two library sinks of an observation cross as handles, 0 the value and 1 the role: the
// host writes each back through `zw_sink_deliver` before the import returns.
LOOM_WASM_IMPORT(sense_observe) int32_t zh_sense_observe(uint64_t author, const char* shape_name,
                                                         uint32_t shape_version, ZenSenseBy* by);
LOOM_WASM_IMPORT(sense_observe_role) int32_t zh_sense_observe_role(const char* role,
                                                                       const char* shape_name,
                                                                       uint32_t shape_version,
                                                                       ZenSenseBy* by);
LOOM_WASM_IMPORT(sense_offer) int32_t zh_sense_offer(uint64_t op, const uint8_t* p, size_t n);
// A null role pointer crosses as 0: personal speech for `as_role`, a directed ask for `to_role`.
LOOM_WASM_IMPORT(ask) int32_t zh_ask(const char* as_role, uint64_t target, const char* to_role,
                                     uint64_t reply_to, uint64_t correlation, const uint8_t* p,
                                     size_t n, uint64_t* attempt_out);
// A call's two library sinks cross as handles, 0 the answer and 1 the words, written back through
// `zw_sink_deliver` before the import returns.
LOOM_WASM_IMPORT(call) int32_t zh_call(const char* role, const uint8_t* p, size_t n,
                                       const char* output_name, uint32_t output_version);

} // extern "C"

namespace {

void to_host_sink(void* ctx, const uint8_t* data, size_t len) {
    zh_sink_write(static_cast<int32_t>(reinterpret_cast<intptr_t>(ctx)), data, len);
}

ZenByteSink host_sink(int32_t handle) {
    return ZenByteSink{reinterpret_cast<void*>(static_cast<intptr_t>(handle)), &to_host_sink};
}

/// The sinks of the observation in progress, for `zw_sink_deliver`.
ZenByteSink g_observe_sinks[2] = {{nullptr, nullptr}, {nullptr, nullptr}};

ZenStatus g_send(void*, uint64_t t, uint64_t r, uint64_t c, const uint8_t* p, size_t n,
                 uint64_t* a) {
    return zh_send(t, r, c, p, n, a);
}
ZenStatus g_publish(void*, uint64_t r, uint64_t c, const uint8_t* p, size_t n) {
    return zh_publish(r, c, p, n);
}
ZenStatus g_send_to_role(void*, const char* role, uint64_t r, uint64_t c, const uint8_t* p,
                         size_t n, uint64_t* a) {
    return zh_send_to_role(role, r, c, p, n, a);
}
uint64_t g_defer_answer(void*) { return zh_defer_answer(); }
ZenStatus g_answer_deferred(void*, uint64_t t, const uint8_t* p, size_t n) {
    return zh_answer_deferred(t, p, n);
}
void g_release_deferred(void*, uint64_t t) { zh_release_deferred(t); }
ZenStatus g_answer(void*, const uint8_t* p, size_t n) { return zh_answer(p, n); }
ZenStatus g_role_send(void*, const char* as, uint64_t t, uint64_t r, uint64_t c,
                        const uint8_t* p, size_t n, uint64_t* a) {
    return zh_role_send(as, t, r, c, p, n, a);
}
ZenStatus g_role_send_to_role(void*, const char* as, const char* to, uint64_t r, uint64_t c,
                                const uint8_t* p, size_t n, uint64_t* a) {
    return zh_role_send_to_role(as, to, r, c, p, n, a);
}
ZenStatus g_role_publish(void*, const char* as, uint64_t r, uint64_t c, const uint8_t* p,
                           size_t n, uint64_t* recipients) {
    return zh_role_publish(as, r, c, p, n, recipients);
}
ZenStatus g_sense_claim(void*, const uint8_t* p, size_t n, uint64_t* revision) {
    return zh_sense_claim(p, n, revision);
}
ZenStatus g_sense_role_claim(void*, const char* as, const uint8_t* p, size_t n,
                               uint64_t* revision) {
    return zh_sense_role_claim(as, p, n, revision);
}

/// An observation's two sinks held for its length, and the ones before them restored after: an
/// observation made from inside a sink's write leaves the outer one intact.
struct ObserveSinks {
    ZenByteSink saved[2];
    ObserveSinks(ZenByteSink value, ZenByteSink role)
        : saved{g_observe_sinks[0], g_observe_sinks[1]} {
        g_observe_sinks[0] = value;
        g_observe_sinks[1] = role;
    }
    ~ObserveSinks() {
        g_observe_sinks[0] = saved[0];
        g_observe_sinks[1] = saved[1];
    }
    ObserveSinks(const ObserveSinks&) = delete;
    ObserveSinks& operator=(const ObserveSinks&) = delete;
};

ZenStatus g_sense_observe(void*, uint64_t author, const char* name, uint32_t version,
                          ZenByteSink value, ZenByteSink role_sink, ZenSenseBy* by) {
    const ObserveSinks held(value, role_sink);
    return zh_sense_observe(author, name, version, by);
}
ZenStatus g_sense_observe_role(void*, const char* role, const char* name, uint32_t version,
                               ZenByteSink value, ZenByteSink role_sink, ZenSenseBy* by) {
    const ObserveSinks held(value, role_sink);
    return zh_sense_observe_role(role, name, version, by);
}
ZenStatus g_sense_offer(void*, uint64_t op, const uint8_t* p, size_t n) {
    return zh_sense_offer(op, p, n);
}
ZenStatus g_ask(void*, const char* as, uint64_t t, const char* to, uint64_t r, uint64_t c,
                const uint8_t* p, size_t n, uint64_t* a) {
    return zh_ask(as, t, to, r, c, p, n, a);
}
ZenStatus g_call(void*, const char* role, const uint8_t* p, size_t n, const char* output_name,
                 uint32_t output_version, ZenByteSink answer, ZenByteSink words) {
    const ObserveSinks held(answer, words);
    return zh_call(role, p, n, output_name, output_version);
}

// By name, in declaration order (KERN-04).
const ZenHostApi kHostApi{.size                 = sizeof(ZenHostApi),
                          .ctx                  = nullptr,
                          .send                 = &g_send,
                          .publish              = &g_publish,
                          .send_to_role         = &g_send_to_role,
                          .defer_answer         = &g_defer_answer,
                          .answer_deferred      = &g_answer_deferred,
                          .release_deferred     = &g_release_deferred,
                          .answer               = &g_answer,
                          .role_send          = &g_role_send,
                          .role_send_to_role  = &g_role_send_to_role,
                          .role_publish       = &g_role_publish,
                          .sense_claim          = &g_sense_claim,
                          .sense_role_claim   = &g_sense_role_claim,
                          .sense_observe        = &g_sense_observe,
                          .sense_observe_role = &g_sense_observe_role,
                          .sense_offer          = &g_sense_offer,
                          .ask                  = &g_ask,
                          .call                 = &g_call};

} // namespace

extern "C" {

// ---- exports: the weave's descriptor, flattened ------------------------------------------------
LOOM_WASM_EXPORT(zw_abi_version) uint32_t zw_abi_version(void) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi == nullptr ? 0u : abi->abi_version;
}
// The manifest version the weave's `describe` writes, which a host judges before making it.
LOOM_WASM_EXPORT(zw_abi_manifest_version) uint32_t zw_abi_manifest_version(void) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi == nullptr ? 0u : abi->manifest_version;
}
LOOM_WASM_EXPORT(zw_alloc) void* zw_alloc(size_t n) { return std::malloc(n == 0 ? 1 : n); }
LOOM_WASM_EXPORT(zw_free) void zw_free(void* p) { std::free(p); }
LOOM_WASM_EXPORT(zw_create) void* zw_create(int32_t refusal_sink) {
    return zen_weave_abi()->create(host_sink(refusal_sink));
}
// The two creation doors (ABI 14): the shape the weave is made with, and its making with a value
// the host copied into this module's memory.
LOOM_WASM_EXPORT(zw_describe_creation) int32_t zw_describe_creation(int32_t sink) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi->describe_creation == nullptr ? ZEN_OK : abi->describe_creation(host_sink(sink));
}
LOOM_WASM_EXPORT(zw_create_with) void* zw_create_with(int32_t refusal_sink, const uint8_t* value,
                                                      size_t len) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi->create_with == nullptr ? nullptr
                                       : abi->create_with(host_sink(refusal_sink), value, len);
}
LOOM_WASM_EXPORT(zw_destroy) void zw_destroy(void* instance) {
    zen_weave_abi()->destroy(instance);
}
LOOM_WASM_EXPORT(zw_describe) int32_t zw_describe(void* instance, int32_t sink) {
    return zen_weave_abi()->describe(instance, host_sink(sink));
}
LOOM_WASM_EXPORT(zw_snapshot) int32_t zw_snapshot(void* instance, int32_t sink) {
    return zen_weave_abi()->snapshot(instance, host_sink(sink));
}
LOOM_WASM_EXPORT(zw_policy) int32_t zw_policy(void* instance, int32_t sink) {
    return zen_weave_abi()->policy(instance, host_sink(sink));
}
LOOM_WASM_EXPORT(zw_revive) int32_t zw_revive(void* instance, const uint8_t* state, size_t len) {
    return zen_weave_abi()->revive(instance, state, len);
}
// The host writes the delivery's facts into this module's memory in this target's own layout,
// at the offsets the box keeps (src/kernel/wasm_box.cpp), and says their size.
static_assert(sizeof(void*) == 4 && offsetof(ZenDelivery, attested_sequence) == 32 &&
                  offsetof(ZenDelivery, authored_role) == 40 &&
                  offsetof(ZenDelivery, addressed_role) == 44 &&
                  offsetof(ZenDelivery, turn) == 48 && offsetof(ZenDelivery, turn_ms) == 56 &&
                  offsetof(ZenDelivery, turn_date_ms) == 64 && sizeof(ZenDelivery) == 72,
              "ZenDelivery's wasm32 layout is the one the box writes");
LOOM_WASM_EXPORT(zw_handle) int32_t zw_handle(void* instance, const ZenDelivery* delivery,
                                              const uint8_t* payload, size_t len) {
    return zen_weave_abi()->handle(instance, delivery, payload, len, &kHostApi);
}
LOOM_WASM_EXPORT(zw_claim_published) int32_t zw_claim_published(void* instance,
                                                                const uint8_t* value, size_t len) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi->claim_published == nullptr ? ZEN_ERR : abi->claim_published(instance, value, len);
}
// One of the weave's functions (ABI 17), of its argument alone: no instance and no host table.
// The answer goes to the host's sink `answer_sink`, the words of a failure to `words_sink`.
LOOM_WASM_EXPORT(zw_provide) int32_t zw_provide(const uint8_t* argument, size_t len,
                                                int32_t answer_sink, int32_t words_sink) {
    const ZenWeaveAbi* abi = zen_weave_abi();
    return abi->provide == nullptr
               ? ZEN_ERR
               : abi->provide(argument, len, host_sink(answer_sink), host_sink(words_sink));
}
// The host writes an observed value (handle 0) or role (handle 1), or a call's answer (0) or
// words (1), into the library's own sink, while the import is still on the stack.
LOOM_WASM_EXPORT(zw_sink_deliver) void zw_sink_deliver(int32_t handle, const uint8_t* data,
                                                       size_t len) {
    if (handle < 0 || handle > 1) {
        return;
    }
    const ZenByteSink s = g_observe_sinks[handle];
    if (s.write != nullptr) {
        s.write(s.ctx, data, len);
    }
}

} // extern "C"
