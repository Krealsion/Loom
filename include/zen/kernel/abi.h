// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

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
#define ZEN_ABI_VERSION 6u

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

/* v5 (R2D-0): role-authored delivery provenance crosses the seam, both ways.
 *
 * v5 carries the office-authorship fact into handle() beside the other
 * host-computed delivery facts, and gives loaded weaves the explicit
 * office-authorship doors native weaves have: office_send / office_send_to_role
 * / office_publish. The host verifies role membership at the authorship moment —
 * the library REQUESTS "speak as R" and never attests itself, exactly as it
 * never chooses its own sender id.
 *
 * Inbound, the authored role travels as its own parameter rather than another
 * ZEN_PROV_* kind, because it is a DIFFERENT AXIS: an answer or an activation
 * could in principle also be office speech, and folding the office into the
 * mutually-exclusive flag word would foreclose that representation for a layout
 * convenience.
 *
 * Paid as a break, once again deliberately: a v4 artifact under a v5 host would
 * be silently unable to author or observe office speech — the exact
 * same-word-two-meanings failure v4 closed for answer(). Old artifacts refuse
 * at load with the version named; nothing loads crippled. */

/* v6 (R2E-0): SENSES cross the seam, both ways.
 *
 * A Sense is a deliberate immutable claim of the latest observation a
 * participant has made available — the second thing a participant can say, read
 * synchronously and carrying no causality. Senses are intended for real loadable
 * components, so a loaded weave gets exactly the surface a native one has:
 * claim / office_claim outbound, observe / observe_office inbound, and the
 * declared claim-set in the manifest.
 *
 * The manifest half is what makes DISCOVERY work across the seam: the claim-set
 * is descriptor bytes the host re-admits and registers at load, so "what Senses
 * can this artifact provide?" is answerable before it has claimed anything —
 * rather than after a runtime claim accidentally reveals a shape.
 *
 * The trust split is v5's, unchanged: the library REQUESTS "claim as R" and the
 * host verifies membership at the claim moment; the library asks to observe and
 * the host authorizes against the loaded weave's own grant. A library attests
 * nothing about itself in either direction.
 *
 * Paid as a break for the third time, and for the third time deliberately: a v5
 * artifact under a v6 host would compile against a Bus whose claim/observe verbs
 * silently return the refusing defaults — a weave unable to claim, unable to
 * read, and unable to say so. That is precisely the same-word-two-meanings
 * failure v4 closed for answer() and v5 closed for office speech. Old artifacts
 * refuse at load with both versions named; nothing loads crippled. */

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
    ZEN_ERR_NO_TARGET = -4,      /* the host had no such routing target */
    /* Role authorship denied (v5): the sender does not currently hold the role
     * it deliberately asked to speak for. NOT a gate refusal and NOT a grant
     * problem — nothing was queued, and nothing was downgraded to personal
     * speech. Distinct so a caller can tell "the office refused me" from "the
     * payload was malformed". */
    ZEN_ERR_ROLE_AUTHORSHIP_DENIED = -5,
    /* Sense refusals (v6). Four distinct answers, kept distinct across the seam
     * for the reason every refusal in Loom is kept distinct: they send a maker
     * to four different places. NO_CLAIM is "nobody has claimed that"; NOT
     * AUTHORIZED is "your grant does not permit reading that shape" — collapsing
     * those two would let a misconfigured grant masquerade as an empty world.
     * UNDECLARED is "you did not list that shape in Claims<...>"; OFFICE is the
     * claim-side twin of ZEN_ERR_ROLE_AUTHORSHIP_DENIED. */
    ZEN_ERR_SENSE_NO_CLAIM = -6,
    ZEN_ERR_SENSE_NOT_AUTHORIZED = -7,
    ZEN_ERR_SENSE_UNDECLARED = -8,
    ZEN_ERR_SENSE_OFFICE_NOT_HELD = -9
};

/* A host-provided byte sink. The library hands bytes to the host via write();
 * the host copies them immediately into host-owned memory. The library
 * allocates nothing host-visible and frees nothing across the seam, so no host
 * pointer can outlive the library. */
typedef struct ZenByteSink {
    void* ctx;
    void (*write)(void* ctx, const uint8_t* data, size_t len);
} ZenByteSink;

/* THE AUTHORSHIP OF ONE OBSERVED CLAIM (v6), filled by the HOST. Plain data with
 * a fixed layout, because it must cross a C boundary; every field is a fact the
 * host computed, and none is anything the claiming library said about itself.
 *
 * THE OFFICE NAME IS NOT IN THIS STRUCT, AND THAT IS THE POINT. It crosses
 * through a caller-provided ZenByteSink instead, exactly as the claim's VALUE
 * does — so the identity a reader observes is the identity that was authored,
 * byte for byte, at any length. An earlier draft carried it as a fixed
 * `char office[128]` and truncated silently at the bound; that let a dynamic
 * observation report an office identity NOBODY EVER AUTHORED — a 200-character
 * role arriving as a plausible 127-character prefix, indistinguishable from a
 * real name. A fabricated identity is worse than a refusal and far worse than a
 * long copy, so the bound is gone rather than raised: a bigger buffer would only
 * move the lie further out. The sink keeps the original ownership property that
 * motivated the buffer — the library allocates the storage and the host only
 * writes into it, so nothing crosses the seam that either side must free.
 *
 * An office sink that is never written means the claim was PERSONAL, which no
 * real office name can collide with (a zero-length role is not a role).
 * `office_holder_is_current` is meaningful only for an office claim — false says
 * the office has moved since, and the claim is the previous holder's.
 *
 * THE TWO GENERATION FACTS ARE INDEPENDENT and both are carried, because a
 * live replacement moves the incarnation WITHOUT ending the life:
 *   author_life_is_current         the life that claimed is still at that address
 *   author_incarnation_is_current  the code that claimed is still the code there
 * After a same-life replacement the first is true and the second is false, and a
 * reader that cannot tell those apart cannot tell a predecessor's still-valid
 * historical claim from one the current incarnation just authored. Deriving the
 * second from the first would erase exactly that distinction. */
typedef struct ZenSenseBy {
    uint64_t author;
    uint64_t author_life;
    uint64_t author_incarnation;
    uint32_t author_life_is_current;         /* 0/1 */
    uint32_t author_incarnation_is_current;  /* 0/1; NOT implied by the line above */
    uint32_t office_holder_is_current;       /* 0/1; meaningful iff an office was written */
    uint32_t reserved_;                      /* keep the 64-bit field below aligned */
    uint64_t revision;
} ZenSenseBy;

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
    /* Deliberate office authorship (v5): the same trusted operations a native
     * weave reaches through `mail.as_role(...)`. The library REQUESTS "speak as
     * as_role" — it cannot attest anything: the host knows the exact weave bound
     * to this context, verifies role_holder(as_role) == that weave AT THIS
     * MOMENT, and stamps the provenance itself. Every string is NUL-terminated
     * and valid only for the call. ZEN_ERR_ROLE_AUTHORSHIP_DENIED means the
     * sender does not hold that office — nothing was queued, nothing downgraded.
     *
     * office_send_to_role carries TWO roles that are different facts: as_role is
     * the office spoken for (verified now); to_role is the destination slot
     * (resolved at delivery). office_publish reports the recipient count through
     * `recipients_out` (may be NULL) so "authorized, zero listeners" and
     * "authorship denied" stay distinct across the seam. */
    ZenStatus (*office_send)(void* ctx, const char* as_role, uint64_t target, uint64_t reply_to,
                             uint64_t correlation, const uint8_t* payload, size_t len);
    ZenStatus (*office_send_to_role)(void* ctx, const char* as_role, const char* to_role,
                                     uint64_t reply_to, uint64_t correlation,
                                     const uint8_t* payload, size_t len);
    ZenStatus (*office_publish)(void* ctx, const char* as_role, uint64_t reply_to,
                                uint64_t correlation, const uint8_t* payload, size_t len,
                                uint64_t* recipients_out);
    /* SENSES (v6): the same trusted operations a native weave reaches through
     * `mail.claim(...)` / `mail.as_role(R).claim(...)` / `mail.latest<T>(...)`.
     *
     * claim / office_claim take the claimed value as serialized bytes, which the
     * host admits through the one gate before storing — a stored claim is never
     * an unadmitted value. The host checks the loaded weave's DECLARED claim-set
     * (from the manifest) and, for the office form, verifies role membership at
     * the claim moment, exactly as office_send does. `revision_out` (may be NULL)
     * receives the claim's sequence under its key.
     *
     * observe / observe_office name the shape by (name, version) rather than
     * carrying a schema, and return the claim as bytes through `sink`, the
     * AUTHORED OFFICE NAME through `office_sink`, and the remaining authorship
     * facts through `by` — the host's own facts, never the library's. The
     * library gets a COPY: no host pointer outlives the call, so a loaded reader
     * has no more reach into a claimant than a native one does.
     *
     * `office_sink` is written only for an office claim, and is written EXACTLY:
     * a personal claim leaves it untouched, and no length bound is imposed in
     * either direction. Two sinks rather than one struct field is what makes
     * "the observed identity is the authored identity" true at any name length —
     * see ZenSenseBy for why the earlier fixed buffer was a defect rather than a
     * simplification.
     *
     * A refusal crosses back as ZEN_ERR_SENSE_* so "not authorized", "nothing
     * claimed", "you did not declare that" and "you do not hold that office"
     * stay four distinct answers rather than one silence. */
    ZenStatus (*sense_claim)(void* ctx, const uint8_t* payload, size_t len,
                             uint64_t* revision_out);
    ZenStatus (*sense_office_claim)(void* ctx, const char* as_role, const uint8_t* payload,
                                    size_t len, uint64_t* revision_out);
    ZenStatus (*sense_observe)(void* ctx, uint64_t author, const char* shape_name,
                               uint32_t shape_version, ZenByteSink sink,
                               ZenByteSink office_sink, ZenSenseBy* by);
    ZenStatus (*sense_observe_office)(void* ctx, const char* role, const char* shape_name,
                                      uint32_t shape_version, ZenByteSink sink,
                                      ZenByteSink office_sink, ZenSenseBy* by);
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

    /* Emit the manifest (accepted schemas + state schema + declared claim-set) as
     * descriptor bytes.
     *
     * v6 adds the CLAIM-SET to the manifest rather than adding a second
     * descriptor entry point, because it is the same kind of fact as the
     * accept-set — what this weave's contract is — and one manifest means one
     * decode, one gate crossing, and one place a maker can look. The host
     * re-admits and registers the claim-set at load, which is what makes a loaded
     * artifact's Sense capability discoverable BEFORE it has claimed anything. */
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
     * `authored_role` (v5) is the office this delivery was DELIBERATELY authored
     * as, verified by the host at the authorship moment — NULL (or empty) means
     * personal speech, which no real office can be confused with. It is a
     * separate parameter, not a ZEN_PROV_* kind, because it is a separate axis:
     * the conversation/lifecycle standing and the authored office may coexist.
     * The string is NUL-terminated and valid only for the duration of the call.
     * All are host-computed delivery facts, not payload: a library may trust
     * them exactly as far as it trusts `sender`, and can neither forge one on
     * the way out nor find one on an ordinary message. */
    ZenStatus (*handle)(void* instance, uint64_t sender, uint64_t reply_to, uint64_t correlation,
                        uint32_t provenance, int64_t attested_sequence, const char* authored_role,
                        const uint8_t* payload, size_t len, const ZenHostApi* host);
} ZenWeaveAbi;

/* THE EXPORT DECORATION BELONGS TO THE DECLARATION, NOT ONLY THE DEFINITION.
 *
 * On PE, __declspec(dllexport) is the precise spelling -- and marking the ONE ABI
 * symbol for export also switches off MinGW's export-everything auto-export, so a
 * weave's dynamic surface shrinks to exactly `zen_weave_abi`: the RTLD_LOCAL
 * spirit, PE edition. The ELF visibility attribute is not meaningful on PE (and is
 * warning-hostile under -Werror there), hence the platform split.
 *
 * IT LIVES HERE, BESIDE THE DECLARATION, BECAUSE MSVC COUNTS IT AS PART OF THE
 * LINKAGE (MSVC-0). It used to live only at the definition site in
 * kernel/export.hpp, so every weave declared this symbol undecorated (here) and
 * then defined it decorated (there). GCC and MinGW merge that silently; MSVC
 * refuses it outright -- `error C2375: 'zen_weave_abi': redefinition; different
 * linkage` -- and refused every weave in this tree, which is the honest reading:
 * the two spellings genuinely disagreed about what the symbol was, and only one
 * compiler said so.
 *
 * So the entry point's COMPLETE signature is stated once, in the header that owns
 * the ABI, and the definition macro reuses this very token rather than re-deriving
 * an equivalent one. Declaration and definition now agree by construction rather
 * than by two platform ladders happening to stay in step.
 *
 * On an image that merely INCLUDES this header without defining the entry point --
 * every host, the Kernel included -- the decoration is inert: nothing is exported
 * because nothing is defined, and the host still finds the symbol the only way it
 * ever has, by name through the dynamic loader. */
#if defined(_WIN32)
#define ZEN_KERNEL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define ZEN_KERNEL_EXPORT __attribute__((visibility("default")))
#else
#define ZEN_KERNEL_EXPORT
#endif

/* The one exported symbol every Zen Weave library provides. Returns a pointer to
 * a static descriptor (never freed by the host). */
ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEN_KERNEL_ABI_H */
