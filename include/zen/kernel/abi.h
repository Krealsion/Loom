#ifndef ZEN_KERNEL_ABI_H
#define ZEN_KERNEL_ABI_H

/*
 * The Zen Weave C ABI — the permanent boundary a dynamic library exports.
 *
 * Only C crosses this seam: opaque instance handles, plain function pointers,
 * const uint8_t* + size_t byte buffers, and integer status codes. No C++ types,
 * no STL, no std::any, no exceptions. Every Zen value/schema/message crosses as
 * serialized bytes, and the host re-admits those bytes through loom's gate
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
 * descriptor whose abi_version it does not support.
 *
 * v2 (R2B-1): handle() carries the delivery's PROVENANCE — Loom's own word about
 * why a recipient may trust this message's standing in a lifecycle conversation.
 * A loaded weave could previously distinguish a message's shape and its sender
 * but not whether Loom itself authorized it, which left a heir claiming an
 * inheritance by role unable to tell the steward it actually reached from any
 * weave holding the same grant.
 *
 * PAID AS A BREAK, DELIBERATELY. Provenance is a fact ABOUT ONE DELIVERY, so it
 * belongs beside `sender` in the call rather than behind an ambient query a
 * library could read at the wrong moment. The cost is that every pre-v2 artifact
 * is refused at load with a version reason — which is the honest failure. The
 * alternative (an appended host callback) would have loaded stale artifacts
 * happily and left them silently unable to accept an activation, i.e. loaded and
 * permanently inert. A refusal that names its cause beats a weave that never
 * speaks. */
#define ZEN_ABI_VERSION 4u

/* v3 (R2B-2): the host API gained the deferred-answer door, so a DYNAMICALLY
 * LOADED weave can hold an answer right across handler boundaries — the case a
 * dynamic steward needs and v2 could not express. Appending callbacks is
 * binary-compatible in itself, but the version is still bumped: a v2 library
 * loaded by a v3 host would be indistinguishable from a v3 one by number alone,
 * and a v3 library loaded by a v2 host would read past the struct. A version is
 * exactly the thing that makes those two cases refuse instead of guess. */

/* v4 (R2B-3b-1a): the host API gained the IMMEDIATE ANSWER door.
 *
 * A native weave writes `mail.answer(reply)` and Loom enqueues an authenticated
 * answer. A dynamically loaded weave writes the same line, reached `HostApiBus`,
 * and got the base class's do-nothing default: no answer, no refusal, no bus
 * event. The same public word meant two different things depending on which side
 * of this seam it was spoken, and the difference was SILENT.
 *
 * Paid as a version bump rather than a quiet append. Appending a callback is
 * binary-compatible in itself, which is exactly the danger: a v3 library loaded by
 * a v4 host would be indistinguishable by number alone and would keep failing
 * silently, and a v4 library loaded by a v3 host would read past the struct. A
 * version is the thing that makes both refuse instead of guess. */

/* Delivery provenance flags (ZEN_PROV_*). Zero means an ordinary message: it
 * stands on its shape and its sender stamp, and claims nothing more. These are
 * set by the HOST from bus-owned state; they have no wire representation and no
 * schema, so no payload a weave can compose ever produces one. */
enum {
    ZEN_PROV_NONE = 0u,
    /* This delivery is THE one authorized answer to a request this weave sent.
     * Only the weave that actually received that request could produce it, and
     * only once. */
    ZEN_PROV_ANSWER = 1u,
    /* Loom attests a lifecycle commit for THIS incarnation. The attested
     * sequence travels beside the flag so an attestation issued for one
     * activation cannot authenticate another. */
    ZEN_PROV_ACTIVATION = 2u
};

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

/* Host callbacks a Weave uses to send/publish from inside handle(). The payload
 * crosses as serialized message bytes; the host admits it through the gate
 * before routing it on the bus. Weave ids are opaque uint64 values (0 == none).
 * Inputs are valid only for the duration of the call. */
typedef struct ZenHostApi {
    void* ctx;
    ZenStatus (*send)(void* ctx, uint64_t target, uint64_t reply_to, uint64_t correlation,
                      const uint8_t* payload, size_t len);
    ZenStatus (*publish)(void* ctx, uint64_t reply_to, uint64_t correlation,
                         const uint8_t* payload, size_t len);
    /* Send to whichever Weave currently holds `role` (Part A's role-addressing). The
     * sender is NOT passed and never rides the wire — the host stamps it from the
     * connection, so a mod cannot impersonate another. `role` is NUL-terminated. */
    ZenStatus (*send_to_role)(void* ctx, const char* role, uint64_t reply_to,
                              uint64_t correlation, const uint8_t* payload, size_t len);
    /* Deferred answers (R2B-2). The capability crosses as an OPAQUE token: it has
     * no wire form, is not a message field, is not reconstructible from sender,
     * correlation, role or schema, and is validated host-side against the bound
     * requester, respondent, both incarnations and correlation. A number on its
     * own is not authority — the library must also be speaking through the host
     * context of a live delivery to the incarnation that earned the right.
     *
     * defer_answer:    convert this delivery's immediate answer right into a
     *                  retained one. 0 == there was none to convert.
     * answer_deferred: spend it. The host chooses the recipient and correlation.
     * release_deferred: abandon it; the host slot is reclaimed at once. */
    uint64_t (*defer_answer)(void* ctx);
    ZenStatus (*answer_deferred)(void* ctx, uint64_t token, const uint8_t* payload, size_t len);
    void (*release_deferred)(void* ctx, uint64_t token);
    /* The IMMEDIATE authenticated answer (v4): the same trusted operation a native
     * weave reaches through `mail.answer()`. The library asks for the public
     * operation and nothing more — no authority crosses, in either direction. The
     * host decides whether this delivery earned an answer, chooses the recipient
     * and the correlation, and stamps the requester-target provenance; a library
     * cannot name any of them. ZEN_ERR_REFUSED means there was no answer authority
     * to spend (a root's request, or one already answered), which is a REAL
     * result the caller can act on rather than the silence it used to get. */
    ZenStatus (*answer)(void* ctx, const uint8_t* payload, size_t len);
} ZenHostApi;

/* The single descriptor a Weave library exposes, returned by zen_weave_abi().
 * Every method works over the opaque instance handle and byte buffers.
 *
 * Buffer ownership:
 *   - library -> host returns go through `sink` (host copies; library frees nothing);
 *   - host -> library inputs are const ptr + len, valid only for the call.
 */
typedef struct ZenWeaveAbi {
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
    /* Handle an already-host-gated inbound message; may send/publish via `host`.
     *
     * `provenance` is a ZEN_PROV_* flag word and `attested_sequence` is the
     * sequence Loom attested (meaningful only for ZEN_PROV_ACTIVATION, else 0).
     * Both are host-computed delivery facts, not payload: a library may trust
     * them exactly as far as it trusts `sender`, and can neither forge one on
     * the way out nor find one on an ordinary message. */
    ZenStatus (*handle)(void* instance, uint64_t sender, uint64_t reply_to, uint64_t correlation,
                        uint32_t provenance, int64_t attested_sequence, const uint8_t* payload,
                        size_t len, const ZenHostApi* host);
} ZenWeaveAbi;

/* The one exported symbol every Zen Weave library provides. Returns a pointer to
 * a static descriptor (never freed by the host). */
const ZenWeaveAbi* zen_weave_abi(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEN_KERNEL_ABI_H */
