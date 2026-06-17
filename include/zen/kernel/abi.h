#ifndef ZEN_KERNEL_ABI_H
#define ZEN_KERNEL_ABI_H

/*
 * The Zen Shard C ABI — the permanent boundary a dynamic library exports.
 *
 * Only C crosses this seam: opaque instance handles, plain function pointers,
 * const uint8_t* + size_t byte buffers, and integer status codes. No C++ types,
 * no STL, no std::any, no exceptions. Every Zen value/schema/message crosses as
 * serialized bytes, and the host re-admits those bytes through zen-core's gate
 * before trusting them — so the DLL boundary is just another boundary the one
 * gate guards.
 *
 * This header is valid C and C++.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ABI's own version, distinct from any schema version. The host rejects a
 * descriptor whose abi_version it does not support. */
#define ZEN_ABI_VERSION 1u

/* Status codes returned across the seam. 0 == OK; negatives are errors. No
 * exception ever crosses the boundary; the host adapter translates these. */
typedef int32_t ZenStatus;
enum {
    ZEN_OK = 0,
    ZEN_ERR = -1,                /* generic library-side failure */
    ZEN_ERR_REFUSED = -2,        /* the host gate refused the bytes */
    ZEN_ERR_UNKNOWN_SCHEMA = -3, /* the host could not resolve the payload's schema */
    ZEN_ERR_NO_TARGET = -4       /* the host had no such routing target */
};

/* A host-provided byte sink. The library hands bytes to the host via write();
 * the host copies them immediately into host-owned memory. The library
 * allocates nothing host-visible and frees nothing across the seam, so no host
 * pointer can outlive the library. */
typedef struct ZenByteSink {
    void* ctx;
    void (*write)(void* ctx, const uint8_t* data, size_t len);
} ZenByteSink;

/* Host callbacks a Shard uses to send/publish from inside handle(). The payload
 * crosses as serialized message bytes; the host admits it through the gate
 * before routing it on the bus. Shard ids are opaque uint64 values (0 == none).
 * Inputs are valid only for the duration of the call. */
typedef struct ZenHostApi {
    void* ctx;
    ZenStatus (*send)(void* ctx, uint64_t target, uint64_t reply_to, uint64_t correlation,
                      const uint8_t* payload, size_t len);
    ZenStatus (*publish)(void* ctx, uint64_t reply_to, uint64_t correlation,
                         const uint8_t* payload, size_t len);
} ZenHostApi;

/* The single descriptor a Shard library exposes, returned by zen_shard_abi().
 * Every method works over the opaque instance handle and byte buffers.
 *
 * Buffer ownership:
 *   - library -> host returns go through `sink` (host copies; library frees nothing);
 *   - host -> library inputs are const ptr + len, valid only for the call.
 */
typedef struct ZenShardAbi {
    uint32_t abi_version;

    void* (*create)(void);
    void (*destroy)(void* instance);

    /* Emit the manifest (accepted schemas + state schema) as descriptor bytes. */
    ZenStatus (*describe)(void* instance, ZenByteSink sink);
    /* Emit persistable state as bytes. */
    ZenStatus (*snapshot)(void* instance, ZenByteSink sink);
    /* Emit the lifecycle policy as bytes. */
    ZenStatus (*policy)(void* instance, ZenByteSink sink);
    /* Restore from state bytes the host has already admitted through the gate. */
    ZenStatus (*revive)(void* instance, const uint8_t* state, size_t len);
    /* Handle an already-host-gated inbound message; may send/publish via `host`. */
    ZenStatus (*handle)(void* instance, uint64_t sender, uint64_t reply_to, uint64_t correlation,
                        const uint8_t* payload, size_t len, const ZenHostApi* host);
} ZenShardAbi;

/* The one exported symbol every Zen Shard library provides. Returns a pointer to
 * a static descriptor (never freed by the host). */
const ZenShardAbi* zen_shard_abi(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEN_KERNEL_ABI_H */
