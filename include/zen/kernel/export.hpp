// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_EXPORT_HPP
#define ZEN_KERNEL_EXPORT_HPP

// The weaving layer. A library maker writes a clean C++ loom::Weave
// subclass and adds one line — ZEN_EXPORT_WEAVE(MyWeave) — and the macro
// generates the whole C ABI: the descriptor, every thunk, and the single
// exported symbol. No maker hand-writes a thunk, and Zen stays invisible: the
// same Weave they would compile in is the same Weave they ship in a .so.
//
// The thunks bridge C <-> C++: they serialize Values to bytes for the host's
// ByteSink, rebuild a Bus that forwards send/publish across the host callback
// table, and never let a C++ exception cross the seam (everything is caught and
// turned into a status code).

#include <zen/kernel/abi.h>
#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// On PE (the Windows development/demo backend), __declspec(dllexport) is the
// precise spelling — and marking the ONE ABI symbol for export also switches
// off MinGW's export-everything auto-export, so the library's dynamic surface
// shrinks to exactly `zen_weave_abi`: the RTLD_LOCAL spirit, PE edition. The
// ELF visibility attribute is not meaningful on PE (and is warning-hostile
// under -Werror there), hence the platform split.
#if defined(_WIN32)
#define ZEN_KERNEL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define ZEN_KERNEL_EXPORT __attribute__((visibility("default")))
#else
#define ZEN_KERNEL_EXPORT
#endif

namespace loom::detail {

inline void sink_write(ZenByteSink sink, const std::string& bytes) {
    if (sink.write != nullptr) {
        sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
}

inline std::string_view as_view(const std::uint8_t* data, std::size_t len) {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

// A Bus implementation that lives inside the library and forwards a Weave's
// send/publish across the host callback table as serialized payload bytes. The
// host assigns the real sender id and admits the bytes through the gate before
// routing; the library-side Ticket/count are therefore not meaningful here.
class HostApiBus final : public loom::Bus {
public:
    explicit HostApiBus(const ZenHostApi* host) : host_(host) {}

    loom::Ticket send(loom::WeaveId target, loom::Message msg) override {
        const std::string bytes = loom::serialize(msg.payload);
        if (host_ != nullptr && host_->send != nullptr) {
            host_->send(host_->ctx, target.value, msg.reply_to.value, msg.correlation,
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        return loom::Ticket{};
    }

    std::size_t publish(loom::Message msg) override {
        const std::string bytes = loom::serialize(msg.payload);
        if (host_ != nullptr && host_->publish != nullptr) {
            host_->publish(host_->ctx, msg.reply_to.value, msg.correlation,
                           reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        return 0;
    }

    // Out-of-process role-addressed send: ship the payload bytes + role to the host,
    // which stamps the authoritative sender from the connection and routes via
    // send_as_to_role. The sender is never passed here and never rides the wire.
    loom::Ticket send_to_role(std::string_view role, loom::Message msg) override {
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(role); // NUL-terminated for the C ABI
        if (host_ != nullptr && host_->send_to_role != nullptr) {
            host_->send_to_role(host_->ctx, role_z.c_str(), msg.reply_to.value, msg.correlation,
                                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        return loom::Ticket{};
    }

    /// THE IMMEDIATE ANSWER, across the seam (R2B-3b-1a).
    ///
    /// Without this override a loaded weave inherited `Bus::answer`'s truthful-
    /// for-a-mailbox default — invalid ticket, nothing enqueued — which for a LIVE
    /// DELIVERY is not truthful at all, merely silent. The same `mail.answer()`
    /// that works natively appeared to do nothing here, and said nothing about it.
    loom::Ticket answer(loom::Message msg) override {
        if (host_ == nullptr || host_->answer == nullptr) {
            return loom::Ticket{}; // an old host: honestly nothing, not a pretence
        }
        const std::string bytes = loom::serialize(msg.payload);
        const ZenStatus st = host_->answer(
            host_->ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        // Not a bus seq — it never is across this seam — but success/failure IS
        // meaningful, and is the only way a loaded weave learns whether its answer
        // was authorized.
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    // ---- deferred answers across the seam (R2B-2) ---------------------------
    //
    // The capability crosses as an OPAQUE TOKEN and nothing else. It carries no
    // issuer here, and does not need one: a loaded weave can only present a token
    // through the host context of the delivery that handed it one, so the token
    // reaches its own board's registry or no registry at all.

    loom::DeferredAnswer make_deferred_answer() override {
        if (host_ == nullptr || host_->defer_answer == nullptr) {
            return loom::DeferredAnswer{};
        }
        return loom::DeferredAnswer::from_host_token(host_->defer_answer(host_->ctx));
    }

    loom::Ticket spend_deferred(const loom::DeferredAnswer& answer, loom::Message msg) override {
        if (host_ == nullptr || host_->answer_deferred == nullptr || !answer.valid()) {
            return loom::Ticket{};
        }
        const std::string bytes = loom::serialize(msg.payload);
        const ZenStatus st = host_->answer_deferred(
            host_->ctx, answer.opaque_token(),
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        // The library-side Ticket is not a bus seq (it never is across this seam),
        // but success/failure IS meaningful and is the only way a loaded steward
        // can learn whether its answer went out.
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    void release_deferred(const loom::DeferredAnswer& answer) override {
        if (host_ != nullptr && host_->release_deferred != nullptr && answer.valid()) {
            host_->release_deferred(host_->ctx, answer.opaque_token());
        }
    }

    // ---- deliberate office authorship across the seam (R2D-0 / v5) ----------
    //
    // The library REQUESTS the public operation; the host verifies membership
    // and stamps. A missing door (an older or narrower host) refuses HONESTLY —
    // invalid ticket / unauthored publication — and NEVER falls back to an
    // ordinary send: a silent downgrade from office to personal speech is
    // exactly the same-word-two-meanings failure v4 closed for answer().

    loom::Ticket office_send(std::string_view as_role, loom::WeaveId target,
                             loom::Message msg) override {
        if (host_ == nullptr || host_->office_send == nullptr) {
            return loom::Ticket{}; // no door: honestly refused, not downgraded
        }
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(as_role); // NUL-terminated for the C ABI
        const ZenStatus st = host_->office_send(
            host_->ctx, role_z.c_str(), target.value, msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    loom::Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                                     loom::Message msg) override {
        if (host_ == nullptr || host_->office_send_to_role == nullptr) {
            return loom::Ticket{};
        }
        const std::string bytes = loom::serialize(msg.payload);
        const std::string as_z(as_role);
        const std::string to_z(to_role);
        const ZenStatus st = host_->office_send_to_role(
            host_->ctx, as_z.c_str(), to_z.c_str(), msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    loom::OfficePublication office_publish(std::string_view as_role,
                                           loom::Message msg) override {
        if (host_ == nullptr || host_->office_publish == nullptr) {
            return loom::OfficePublication{};
        }
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(as_role);
        std::uint64_t recipients = 0;
        const ZenStatus st = host_->office_publish(
            host_->ctx, role_z.c_str(), msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &recipients);
        if (st != ZEN_OK) {
            return loom::OfficePublication{};
        }
        // The host-side count is real here (unlike ordinary publish, whose count
        // deliberately does not cross): the door carries it out-of-band so the
        // authorized-but-unheard office is not confusable with a refused one.
        return loom::OfficePublication{true, static_cast<std::size_t>(recipients)};
    }

    // ---- Senses (v6) --------------------------------------------------------
    //
    // The same four public verbs a native weave reaches, forwarded. A missing
    // door refuses honestly rather than silently doing nothing — the failure v4
    // closed for answer() and v5 for office speech, closed here too.

    loom::SenseClaimResult claim(loom::Value value) override {
        if (host_ == nullptr || host_->sense_claim == nullptr) {
            return loom::SenseClaimResult{};
        }
        const std::string bytes = loom::serialize(value);
        std::uint64_t revision = 0;
        const ZenStatus st = host_->sense_claim(
            host_->ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
            &revision);
        return st == ZEN_OK ? loom::SenseClaimResult{true, loom::SenseRefusal::None, revision}
                            : loom::SenseClaimResult{false, sense_refusal_of(st), 0};
    }

    loom::SenseClaimResult office_claim(std::string_view as_role, loom::Value value) override {
        if (host_ == nullptr || host_->sense_office_claim == nullptr) {
            return loom::SenseClaimResult{};
        }
        const std::string bytes = loom::serialize(value);
        const std::string role_z(as_role);
        std::uint64_t revision = 0;
        const ZenStatus st = host_->sense_office_claim(
            host_->ctx, role_z.c_str(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size(), &revision);
        return st == ZEN_OK ? loom::SenseClaimResult{true, loom::SenseRefusal::None, revision}
                            : loom::SenseClaimResult{false, sense_refusal_of(st), 0};
    }

    loom::SenseReading observe(loom::WeaveId author, std::shared_ptr<const loom::Schema> shape) override {
        if (host_ == nullptr || host_->sense_observe == nullptr) {
            return loom::SenseReading{};
        }
        const std::string name_z(shape->name());
        std::string bytes;
        std::string office; // grows to whatever the host wrote; never truncated
        ZenSenseBy by{};
        ZenByteSink sink{&bytes, &collect_bytes};
        ZenByteSink office_sink{&office, &collect_bytes};
        const ZenStatus st = host_->sense_observe(host_->ctx, author.value, name_z.c_str(),
                                                  shape->version(), sink, office_sink, &by);
        return decode_reading(st, bytes, office, by, shape);
    }

    loom::SenseReading observe_office(std::string_view role, std::shared_ptr<const loom::Schema> shape) override {
        if (host_ == nullptr || host_->sense_observe_office == nullptr) {
            return loom::SenseReading{};
        }
        const std::string role_z(role);
        const std::string name_z(shape->name());
        std::string bytes;
        std::string office; // grows to whatever the host wrote; never truncated
        ZenSenseBy by{};
        ZenByteSink sink{&bytes, &collect_bytes};
        ZenByteSink office_sink{&office, &collect_bytes};
        const ZenStatus st = host_->sense_observe_office(
            host_->ctx, role_z.c_str(), name_z.c_str(), shape->version(), sink, office_sink, &by);
        return decode_reading(st, bytes, office, by, shape);
    }

private:
    /// The host's status, back to the four distinct answers. A status this side
    /// does not recognize becomes `NoClaim` rather than a fabricated success.
    static loom::SenseRefusal sense_refusal_of(ZenStatus st) {
        switch (st) {
        case ZEN_ERR_SENSE_NOT_AUTHORIZED:
            return loom::SenseRefusal::NotAuthorized;
        case ZEN_ERR_SENSE_UNDECLARED:
            return loom::SenseRefusal::Undeclared;
        case ZEN_ERR_SENSE_OFFICE_NOT_HELD:
            return loom::SenseRefusal::OfficeNotHeld;
        case ZEN_ERR_REFUSED:
            return loom::SenseRefusal::GateRefused;
        default:
            return loom::SenseRefusal::NoClaim;
        }
    }

    static void collect_bytes(void* ctx, const std::uint8_t* data, std::size_t len) {
        static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
    }

    /// Rebuild a reading library-side. The bytes were serialized from a value the
    /// HOST had already admitted, and they are re-admitted here against this
    /// library's own schema for the shape — so a library never holds a value that
    /// did not pass a gate on its own side of the seam either.
    loom::SenseReading decode_reading(ZenStatus st, const std::string& bytes,
                                      const std::string& office, const ZenSenseBy& by,
                                      const std::shared_ptr<const loom::Schema>& shape) {
        loom::SenseReading out;
        if (st != ZEN_OK) {
            out.refusal = sense_refusal_of(st);
            return out;
        }
        loom::Unverified u = loom::parse(bytes);
        loom::Admission a = loom::admit(u, shape);
        if (!a.ok()) {
            out.refusal = loom::SenseRefusal::GateRefused;
            return out;
        }
        out.refusal = loom::SenseRefusal::None;
        out.value = std::move(a).value();
        out.by.author = loom::WeaveId{by.author};
        out.by.author_life = by.author_life;
        out.by.author_incarnation = by.author_incarnation;
        out.by.author_life_is_current = by.author_life_is_current != 0;
        // Independent of the life fact above: a same-life replacement moves the
        // incarnation without ending the life, and collapsing the two would make
        // a predecessor's claim read as the current incarnation's.
        out.by.author_incarnation_is_current = by.author_incarnation_is_current != 0;
        // EXACTLY what the host wrote, at any length — empty == personal, which
        // no office name can be. Nothing here imposes or restores a bound.
        out.by.office = office;
        out.by.office_holder_is_current = by.office_holder_is_current != 0;
        out.by.revision = by.revision;
        out.by.schema_name = shape->name();
        out.by.schema_version = shape->version();
        return out;
    }

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
        std::vector<std::shared_ptr<const loom::Schema>> accepted = s->accepted_schemas();
        loom::Value state = s->snapshot();
        // The ask is optional: a Weave that declares one (via ZEN_ASK on the maker
        // base) gets it surfaced in the manifest as advice; one that doesn't emits no
        // requests section and lands on the floor. Either way it is never a grant.
        std::optional<loom::CapabilityAsk> ask;
        if constexpr (requires { s->zen_requested_capabilities(); }) {
            ask = s->zen_requested_capabilities();
        }
        // The declared claim-set (v6) rides the same manifest, so a loaded
        // artifact's Sense capability is discoverable at load rather than after
        // some runtime claim accidentally reveals a shape.
        const std::vector<std::shared_ptr<const loom::Schema>> claims = s->claimed_schemas();
        sink_write(sink, loom::serialize(loom::encode_manifest(accepted, state.schema(),
                                                                     ask ? &*ask : nullptr,
                                                                     &claims)));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_snapshot(void* instance, ZenByteSink sink) {
    try {
        sink_write(sink, loom::serialize(static_cast<S*>(instance)->snapshot()));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_policy(void* instance, ZenByteSink sink) {
    try {
        sink_write(sink, loom::serialize(static_cast<S*>(instance)->policy()));
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_revive(void* instance, const std::uint8_t* state, std::size_t len) {
    try {
        S* s = static_cast<S*>(instance);
        loom::Unverified u = loom::parse(as_view(state, len));
        // The host already admitted these bytes; the library re-admits against its
        // own state schema only to rebuild the Value its C++ revive() expects.
        loom::Value probe = s->snapshot();
        loom::Admission a = loom::admit(u, probe.schema_ptr());
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        s->revive(a.value());
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

/// Translate the host's provenance flags — and the authored office (v5) — back
/// into the C++ fact.
///
/// The library rebuilds a Provenance for its own dispatch and nothing else: the
/// value never leaves this call, and the outbound host callbacks have no field
/// to put one in. A library that lied here would be lying to itself.
inline loom::Provenance provenance_from(std::uint32_t flags, std::int64_t sequence,
                                        const char* authored_role) {
    loom::Provenance p;
    switch (flags) {
    case ZEN_PROV_ANSWER:
        p = loom::Provenance::attested(loom::Provenance::Kind::Answer, 0);
        break;
    case ZEN_PROV_ACTIVATION:
        p = loom::Provenance::attested(loom::Provenance::Kind::Activation, sequence);
        break;
    default:
        break;
    }
    // The second axis, composed rather than switched: NULL/empty means personal
    // speech, and either combines with any Kind — the same representation rule
    // the native type keeps.
    if (authored_role != nullptr && authored_role[0] != '\0') {
        p = std::move(p).with_authored_role(std::string(authored_role));
    }
    return p;
}

template <class S>
ZenStatus do_handle(void* instance, std::uint64_t sender, std::uint64_t reply_to,
                    std::uint64_t correlation, std::uint32_t provenance,
                    std::int64_t attested_sequence, const char* authored_role,
                    const std::uint8_t* payload, std::size_t len, const ZenHostApi* host) {
    try {
        S* s = static_cast<S*>(instance);
        loom::Unverified u = loom::parse(as_view(payload, len));
        std::shared_ptr<const loom::Schema> door;
        for (auto& sc : s->accepted_schemas()) {
            if (sc->name() == u.claimed_name() && sc->version() == u.claimed_version()) {
                door = sc;
                break;
            }
        }
        if (!door) {
            return ZEN_ERR_UNKNOWN_SCHEMA;
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        loom::Message msg(a.value(), loom::WeaveId{sender}, loom::WeaveId{reply_to},
                             correlation);
        msg.provenance = provenance_from(provenance, attested_sequence, authored_role);
        HostApiBus bus(host);
        s->handle(msg, bus);
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

} // namespace loom::detail

// Generate the C ABI for a Weave class. The thunks have C language linkage (to
// match the descriptor's function-pointer types) and forward to the C++ helpers
// above. Exactly one ZEN_EXPORT_WEAVE per library.
//
// THE DESCRIPTOR IS INITIALIZED BY NAME, AND THAT IS LOAD-BEARING (BL-4).
// `describe`, `snapshot` and `policy` are three different doors that share one
// type — `ZenStatus (*)(void*, ZenByteSink)` — so a positional initializer can
// put any of them in either of the others' slots and compile without a single
// diagnostic, under -Wall -Wextra -Wpedantic -Werror. A designator cannot: it
// names the door, so the field-to-function mapping is stated in the source and
// checked by the compiler rather than inferred from counting commas.
//
// This is the reviewability half only. The correctness half is separate and
// already held before this: each of those three doors emits a DIFFERENT schema,
// and the host re-admits every one through the one gate against the door it was
// asked for, so a miswire is refused rather than believed. BL-4 measured that —
// every miswire among the three is refused AT LOAD, before the artifact becomes
// a participant — and `tests/test_kernel.cpp` names the property directly.
// Neither half proves the other: a designator can still name the wrong function,
// and the gate would still be what catches it.
//
// The form is deliberately the portable one: designators in DECLARATION ORDER,
// which is standard C++20 and standard C99+. Out-of-order designators are legal
// C and ill-formed C++, so declaration order is what lets one spelling serve
// both sides of a seam whose header promises to be valid in each.
#define ZEN_EXPORT_WEAVE(WeaveClass)                                                                \
    extern "C" {                                                                                    \
    static void* zen__abi_create(void) { return ::loom::detail::do_create<WeaveClass>(); }   \
    static void zen__abi_destroy(void* i) { ::loom::detail::do_destroy<WeaveClass>(i); }      \
    static ZenStatus zen__abi_describe(void* i, ZenByteSink s) {                                    \
        return ::loom::detail::do_describe<WeaveClass>(i, s);                                \
    }                                                                                              \
    static ZenStatus zen__abi_snapshot(void* i, ZenByteSink s) {                                   \
        return ::loom::detail::do_snapshot<WeaveClass>(i, s);                                \
    }                                                                                              \
    static ZenStatus zen__abi_policy(void* i, ZenByteSink s) {                                     \
        return ::loom::detail::do_policy<WeaveClass>(i, s);                                  \
    }                                                                                              \
    static ZenStatus zen__abi_revive(void* i, const uint8_t* st, size_t n) {                       \
        return ::loom::detail::do_revive<WeaveClass>(i, st, n);                              \
    }                                                                                              \
    static ZenStatus zen__abi_handle(void* i, uint64_t sender, uint64_t reply_to,                  \
                                     uint64_t correlation, uint32_t prov, int64_t attested,        \
                                     const char* authored_role, const uint8_t* p, size_t n,        \
                                     const ZenHostApi* h) {                                        \
        return ::loom::detail::do_handle<WeaveClass>(i, sender, reply_to, correlation, prov, \
                                                            attested, authored_role, p, n, h);     \
    }                                                                                              \
    ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {                                      \
        static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION,                             \
                                        .create      = zen__abi_create,                            \
                                        .destroy     = zen__abi_destroy,                           \
                                        .describe    = zen__abi_describe,                          \
                                        .snapshot    = zen__abi_snapshot,                          \
                                        .policy      = zen__abi_policy,                            \
                                        .revive      = zen__abi_revive,                            \
                                        .handle      = zen__abi_handle};                           \
        return &abi;                                                                               \
    }                                                                                              \
    }

#endif // ZEN_KERNEL_EXPORT_HPP
