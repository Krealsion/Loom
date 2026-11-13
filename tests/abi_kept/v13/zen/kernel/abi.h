// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_ABI_H
#define ZEN_KERNEL_ABI_H

/*
 * The Zen Weave C ABI: the boundary a dynamic weave library exports. Only C crosses it: opaque
 * instance handles, function pointers, byte buffers and integer statuses; never a C++ type or
 * an exception. Every value, schema and message crosses as serialized bytes the host re-admits
 * through the gate. Valid C and C++.
 *
 * The seam grows. Every struct that crosses it and can grow says its size, a field or slot is
 * only ever appended, and a reader reads what the size reaches and no further: a field past it
 * is absent, and a slot past it is a NULL slot. A status, a provenance value and a slot keep
 * their number and their meaning for ever. docs/reference/dynamic-abi.md
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ABI's own version, distinct from any schema version. Every change to this header moves
 * it, and from ZEN_ABI_OLDEST on it says what an image needs: a host loads an image of any
 * version from ZEN_ABI_OLDEST to its own, and refuses one from before ZEN_ABI_OLDEST, or one
 * newer than itself, before calling into it, at load and as a replacement for a live
 * participant, naming both versions. What each version carried: docs/reference/dynamic-abi.md */
#define ZEN_ABI_VERSION 13u

/* The oldest version a host loads: the first whose tables say their size. */
#define ZEN_ABI_OLDEST 13u

/* Whether the struct `s` points at, whose first field is its size, reaches `field` whole: a
 * reader of a struct the other side built reads a field only where this holds. */
#define ZEN_ABI_REACHES(s, type, field) \
    ((size_t)(s)->size >= offsetof(type, field) + sizeof(((type*)0)->field))

/* Delivery provenance (ZEN_PROV_*), one value per delivery. Zero is an ordinary message. Set
 * by the host from bus-owned state; no payload a weave composes produces one. A value is
 * handed only with a shape the library declares it accepts for that value, so a value
 * appended later, with a shape of its own, never reaches a library built before it. */
enum {
    ZEN_PROV_NONE = 0u,
    /* The one authorized answer to a request this weave sent. */
    ZEN_PROV_ANSWER = 1u,
    /* Loom attests a lifecycle commit for this incarnation; the attested sequence travels
     * beside it, so an attestation for one activation cannot authenticate another. */
    ZEN_PROV_ACTIVATION = 2u,
    /* Loom's notice that this weave's own addressed send was refused before any handler ran
     * (docs/reference/messaging.md#sender-visible-dispatch-refusal). */
    ZEN_PROV_DISPATCH_REFUSAL = 3u
};

/* Status codes returned across the seam: 0 is OK, negatives are errors. No exception crosses, in
 * either direction, since each side may carry its own C++ runtime; the host adapter translates
 * these, and every host callback answers what stopped it as a status. A reader meeting a status
 * it does not know reads it as its slot says, never as success. */
typedef int32_t ZenStatus;
enum {
    ZEN_OK = 0,
    ZEN_ERR = -1,                /* generic library-side failure */
    ZEN_ERR_REFUSED = -2,        /* the host refused the request: its gate refused the bytes, or
                                    the host could not carry it out */
    ZEN_ERR_UNKNOWN_SCHEMA = -3, /* the host could not resolve the payload's schema */
    ZEN_ERR_NO_TARGET = -4,      /* the host had no such routing target */
    /* The sender does not hold the role it asked to speak for; nothing was queued. Not a
     * gate refusal. */
    ZEN_ERR_ROLE_AUTHORSHIP_DENIED = -5,
    /* Sense refusals, distinct as their causes are: nothing claimed; the grant does not permit
     * reading the shape; the shape is not in Claims<...>; the role is not held. */
    ZEN_ERR_SENSE_NO_CLAIM = -6,
    ZEN_ERR_SENSE_NOT_AUTHORIZED = -7,
    ZEN_ERR_SENSE_UNDECLARED = -8,
    ZEN_ERR_SENSE_ROLE_NOT_HELD = -9,
    /* This sender already has its bound of messages waiting. Nothing was queued. Not role
     * authorship, and not "there was no answer to spend". Answers are not this bound. */
    ZEN_ERR_PENDING_PER_SENDER = -10,
    /* A refused joint offer: ZEN_ERR_JOINT_BASE minus the loom::JointRefusal enumerator. A
     * status below the base naming no enumerator reads as NoLiveDelivery, never as accepted. */
    ZEN_ERR_JOINT_BASE = -100,
    /* The one status that is neither OK nor an error, returned only by claim_published: the
     * library, functioning, did not apply the value and keeps state of its own. */
    ZEN_CLAIM_DECLINED = 1
};

/* A host-provided byte sink: the library hands bytes over through write(), the host copies
 * them at once, and nothing either side must free crosses the seam. A door and its context,
 * whole: it does not grow. */
typedef struct ZenByteSink {
    void* ctx;
    void (*write)(void* ctx, const uint8_t* data, size_t len);
} ZenByteSink;

/* The authorship of one observed claim. The library makes it and says its size; the host
 * fills it from its own facts, as far as that size reaches. The role name is not here: it
 * crosses through a ZenByteSink like the value, so the name a reader sees is the one authored,
 * at any length. A role sink never written means a personal claim. `role_holder_is_current`
 * is meaningful only for a role claim. The two generation facts are independent: after a live
 * code replacement the life is current and the incarnation is not. */
typedef struct ZenSenseBy {
    uint32_t size;                          /* sizeof(ZenSenseBy) as the library built it */
    uint32_t author_life_is_current;        /* 0/1 */
    uint32_t author_incarnation_is_current; /* 0/1; NOT implied by the line above */
    uint32_t role_holder_is_current;        /* 0/1; meaningful iff a role was written */
    uint64_t author;
    uint64_t author_life;
    uint64_t author_incarnation;
    uint64_t revision;
} ZenSenseBy;

/* The facts of one delivery, which the host computes and says the size of: the library reads
 * what that size reaches. `provenance` is a ZEN_PROV_* value and `attested_sequence` the
 * sequence Loom attested (for ZEN_PROV_ACTIVATION, else 0). `authored_role` is the role this
 * delivery was deliberately authored as, verified by the host; NULL is personal speech. It is
 * apart from `provenance` because it may coexist with an answer or an activation. All three are
 * host-computed facts a library trusts as far as `sender`: none can be forged outbound or found
 * on an ordinary message. `addressed_role` is the role the sender addressed, resolved to this
 * weave at delivery; NULL for a directed send and for a publication. It says where the delivery
 * went, never who spoke, and it is no authority. Strings are NUL-terminated and valid for the
 * call. */
typedef struct ZenDelivery {
    uint32_t size; /* sizeof(ZenDelivery) as the host built it */
    uint32_t provenance;
    uint64_t sender;
    uint64_t reply_to;
    uint64_t correlation;
    int64_t attested_sequence;
    const char* authored_role;
    const char* addressed_role;
} ZenDelivery;

/* Host callbacks a Weave uses from inside handle(). The host builds it and says its size, at
 * least the `host_size` the library's descriptor names; a library calls a slot only where that
 * size reaches it. Payloads cross as serialized message bytes the host admits through the gate
 * before routing; weave ids are opaque (0 is none); inputs are valid only for the call.
 * `attempt_out` (may be NULL) receives an addressed send's queued attempt, 0 if nothing was
 * queued. */
typedef struct ZenHostApi {
    uint32_t size; /* sizeof(ZenHostApi) as the host built it */
    void* ctx;
    ZenStatus (*send)(void* ctx, uint64_t target, uint64_t reply_to, uint64_t correlation,
                      const uint8_t* payload, size_t len, uint64_t* attempt_out);
    ZenStatus (*publish)(void* ctx, uint64_t reply_to, uint64_t correlation,
                         const uint8_t* payload, size_t len);
    /* Send to whichever Weave holds `role` now. The host stamps the sender from the context,
     * so a library cannot speak as another. `role` is NUL-terminated. */
    ZenStatus (*send_to_role)(void* ctx, const char* role, uint64_t reply_to,
                              uint64_t correlation, const uint8_t* payload, size_t len, uint64_t* attempt_out);
    /* Deferred answers (ANS-02). The capability crosses as an opaque token, validated host-side
     * against the bound requester, respondent, both incarnations and correlation, and only from
     * the host context of a live delivery to the incarnation that earned it.
     *
     * defer_answer:     convert this delivery's answer right into a retained one; 0 if none.
     * answer_deferred:  spend it; the host chooses the recipient and correlation.
     * release_deferred: abandon it; the host slot is reclaimed at once. */
    uint64_t (*defer_answer)(void* ctx);
    ZenStatus (*answer_deferred)(void* ctx, uint64_t token, const uint8_t* payload, size_t len);
    void (*release_deferred)(void* ctx, uint64_t token);
    /* The immediate authenticated answer, as `mail.answer()` natively. The host decides whether
     * this delivery earned an answer and chooses the recipient and correlation.
     * ZEN_ERR_REFUSED means there was no answer to spend: a root's request, or one already
     * answered. */
    ZenStatus (*answer)(void* ctx, const uint8_t* payload, size_t len);
    /* Role authorship: what a native weave reaches through `mail.as_role(...)`. The library
     * requests "speak as as_role"; the host verifies the weave bound to this context holds it
     * now and stamps the provenance. ZEN_ERR_ROLE_AUTHORSHIP_DENIED means the sender does not
     * hold the role and nothing was queued. ZEN_ERR_PENDING_PER_SENDER means the sender is at
     * its waiting bound. In role_send_to_role, as_role is the role spoken for and to_role the
     * destination,
     * resolved at delivery. role_publish writes the recipient count to `recipients_out` (may
     * be NULL), so "zero listeners" and "denied" stay distinct. Strings are NUL-terminated and
     * valid for the call. */
    ZenStatus (*role_send)(void* ctx, const char* as_role, uint64_t target, uint64_t reply_to,
                             uint64_t correlation, const uint8_t* payload, size_t len, uint64_t* attempt_out);
    ZenStatus (*role_send_to_role)(void* ctx, const char* as_role, const char* to_role,
                                     uint64_t reply_to, uint64_t correlation,
                                     const uint8_t* payload, size_t len, uint64_t* attempt_out);
    ZenStatus (*role_publish)(void* ctx, const char* as_role, uint64_t reply_to,
                                uint64_t correlation, const uint8_t* payload, size_t len,
                                uint64_t* recipients_out);
    /* Senses: what a native weave reaches through `mail.claim(...)`,
     * `mail.as_role(R).claim(...)` and `mail.latest<T>(...)`. A claim's bytes are admitted
     * through the gate before they are stored, against the declared claim-set; the role form
     * verifies membership now. `revision_out` (may be NULL) receives the claim's revision.
     *
     * observe and observe_role name the shape by (name, version) and return a copy: the value
     * through `sink`, the authored role through `role_sink` (written only for a role
     * claim, exactly, at any length) and the other authorship facts through `by`, as far as its
     * size reaches. A refusal is one of the four ZEN_ERR_SENSE_* statuses. */
    ZenStatus (*sense_claim)(void* ctx, const uint8_t* payload, size_t len,
                             uint64_t* revision_out);
    ZenStatus (*sense_role_claim)(void* ctx, const char* as_role, const uint8_t* payload,
                                    size_t len, uint64_t* revision_out);
    ZenStatus (*sense_observe)(void* ctx, uint64_t author, const char* shape_name,
                               uint32_t shape_version, ZenByteSink sink,
                               ZenByteSink role_sink, ZenSenseBy* by);
    ZenStatus (*sense_observe_role)(void* ctx, const char* role, const char* shape_name,
                                      uint32_t shape_version, ZenByteSink sink,
                                      ZenByteSink role_sink, ZenSenseBy* by);
    /* Offer the next value of one of this weave's own latest claims for the joint operation
     * `op` (docs/reference/joint-publication.md). Admitted against the declared claim-set; the
     * host checks the operation, bound revision and exact claimant, and keeps the value until
     * the initiator commits or the operation aborts. Nothing is published here. Returns ZEN_OK
     * or ZEN_ERR_JOINT_BASE minus the refusal. The isolated pipe supplies NULL: a child cannot
     * present the exact identity an operation binds. */
    ZenStatus (*sense_offer)(void* ctx, uint64_t op, const uint8_t* payload, size_t len);
} ZenHostApi;

/* The descriptor a Weave library exposes through zen_weave_abi(), over an opaque instance
 * handle and byte buffers. Returns from the library go through a `sink`, which the host
 * copies; inputs from the host are const pointer and length, valid only for the call.
 *
 * Its first four fields never move: the version the image needs, the descriptor's own size,
 * the size of the host table the image was built against, and the version of the manifest
 * `describe` writes. The host reads the slots `size` reaches; one past it is NULL. A host
 * refuses, before any slot is called, an image whose version it does not load, one whose
 * `size` does not reach `handle`, one built against a larger host table than its own, and one
 * whose manifest it does not read, naming what the image needs and what the host has. */
typedef struct ZenWeaveAbi {
    uint32_t abi_version;
    uint32_t size;             /* sizeof(ZenWeaveAbi) as the library built it */
    uint32_t host_size;        /* sizeof(ZenHostApi) the library was built against */
    uint32_t manifest_version; /* the zen.Manifest version `describe` writes */

    /* Make an instance, or return NULL when the weave refuses its making, having written why to
     * `refusal` (or nothing). The host keeps the first words and says them in the load's refusal
     * (zen/kernel/making_words.hpp). */
    void* (*create)(ZenByteSink refusal);
    void (*destroy)(void* instance);

    /* Emit the manifest (zen.Manifest, zen/kernel/schema_codec.hpp) as descriptor bytes: what
     * the weave accepts, its state shape, and what it claims, emits and asks for. The host
     * re-admits and registers it at load, so all of it is known before the weave runs. */
    ZenStatus (*describe)(void* instance, ZenByteSink sink);
    /* Emit persistable state as bytes. */
    ZenStatus (*snapshot)(void* instance, ZenByteSink sink);
    /* Emit the lifecycle policy as bytes. */
    ZenStatus (*policy)(void* instance, ZenByteSink sink);
    /* Restore from state bytes the host has already admitted through the gate. */
    ZenStatus (*revive)(void* instance, const uint8_t* state, size_t len);
    /* Handle an inbound message the host has already gated, with the delivery's facts; may send
     * through `host`. */
    ZenStatus (*handle)(void* instance, const ZenDelivery* delivery, const uint8_t* payload,
                        size_t len, const ZenHostApi* host);
    /* One of this weave's own latest claims was published by a joint operation and the weave
     * has not run since. Called before its next `handle` and its next `snapshot`, with the
     * value as bytes the library re-admits against its claim-set. Not a delivery: no host
     * table, and nothing may be sent. ZEN_EXPORT_WEAVE always fills it; a NULL slot, or one the
     * descriptor's size does not reach, is shown nothing and read as Failed. The status is the
     * fact, mapped exactly (docs/reference/joint-publication.md#the-showing-and-its-three-answers):
     *
     *   ZEN_OK               Applied.  The library applied the value and stands behind it.
     *   ZEN_CLAIM_DECLINED   Declined. Functioning, it did not apply the value and keeps state
     *                                  of its own, re-claimed at its next delivery. Not held;
     *                                  the initiator is told.
     *   any negative status  Failed.   The showing did not complete: ZEN_ERR for an exception
     *                                  from the weaver's handler or `PublishedClaim::Failed`,
     *                                  ZEN_ERR_UNKNOWN_SCHEMA or ZEN_ERR_REFUSED for bytes the
     *                                  library's gate refused. The weave is held (deliveries
     *                                  refused `ApplicationFailed`, its ordinary snapshot
     *                                  refused, this slot not called again for that value)
     *                                  until reloaded or removed; the initiator is told.
     *   any other positive   Failed.   Undefined for this slot; never Applied. */
    ZenStatus (*claim_published)(void* instance, const uint8_t* value, size_t len);
} ZenWeaveAbi;

/* The export decoration, on the declaration as well as the definition: MSVC counts it as part
 * of the linkage and refuses a symbol declared plain and defined decorated (C2375), so the
 * definition macro in kernel/export.hpp reuses this token. On PE, dllexport on this one symbol
 * also turns off MinGW's export-everything, so a weave exports exactly `zen_weave_abi`. In a
 * host that only includes this header it exports nothing. */
#if defined(_WIN32)
#define ZEN_KERNEL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define ZEN_KERNEL_EXPORT __attribute__((visibility("default")))
#else
#define ZEN_KERNEL_EXPORT
#endif

/* The one symbol every Zen Weave library exports: a pointer to a static descriptor the host
 * never frees. */
ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEN_KERNEL_ABI_H */
