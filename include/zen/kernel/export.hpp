// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_EXPORT_HPP
#define ZEN_KERNEL_EXPORT_HPP

// The weaving layer: a library maker writes an ordinary loom::Weave subclass and adds
// ZEN_EXPORT_WEAVE(MyWeave), which generates the C ABI: the descriptor, every thunk and the one
// exported symbol. The same Weave compiles in or ships in a library. The thunks serialize
// Values for the host, rebuild a Bus that forwards across the host callbacks, and turn every
// C++ exception into a status: none crosses the seam. docs/reference/dynamic-abi.md

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

// ZEN_KERNEL_EXPORT, the entry point's export decoration, comes from <zen/kernel/abi.h>,
// beside the declaration it must match (MSVC counts it as part of the linkage).

namespace loom::detail {

inline void sink_write(ZenByteSink sink, const std::string& bytes) {
    if (sink.write != nullptr) {
        sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
}

inline std::string_view as_view(const std::uint8_t* data, std::size_t len) {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

// A Bus inside the library that forwards a Weave's sends across the host callback table as
// serialized payload bytes; the host stamps the sender and admits the bytes before routing.
// Ordinary and office sends return the real queued attempt; answer tickets are status markers,
// and publication counts do not cross.
class HostApiBus final : public loom::Bus {
public:
    explicit HostApiBus(const ZenHostApi* host) : host_(host) {}

    loom::Ticket send(loom::WeaveId target, loom::Message msg) override {
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        if (host_ != nullptr && host_->send != nullptr) {
            const ZenStatus status = host_->send(host_->ctx, target.value, msg.reply_to.value, msg.correlation,
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
            if (status == ZEN_OK) { return loom::Ticket{attempt}; }
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

    // Role-addressed send: the payload bytes and role go to the host, which stamps the sender
    // from the context and routes with send_as_to_role. The sender never rides the wire.
    loom::Ticket send_to_role(std::string_view role, loom::Message msg) override {
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(role); // NUL-terminated for the C ABI
        if (host_ != nullptr && host_->send_to_role != nullptr) {
            const ZenStatus status = host_->send_to_role(host_->ctx, role_z.c_str(), msg.reply_to.value, msg.correlation,
                                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
            if (status == ZEN_OK) { return loom::Ticket{attempt}; }
        }
        return loom::Ticket{};
    }

    /// The immediate answer across the seam (ANS-06): the same `mail.answer()` a native weave
    /// makes, answered by the host.
    loom::Ticket answer(loom::Message msg) override {
        if (host_ == nullptr || host_->answer == nullptr) {
            return loom::Ticket{}; // no door: nothing sent, and said so
        }
        const std::string bytes = loom::serialize(msg.payload);
        const ZenStatus st = host_->answer(
            host_->ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        // A status marker, not a queued attempt; success or failure is how a loaded weave
        // learns whether its answer was authorized.
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    // ---- deferred answers across the seam (ANS-02, ANS-06) ------------------
    // The capability crosses as an opaque token with no issuer: a loaded weave presents it only
    // through the host context of the delivery that gave it, so it reaches its own board.

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
        // A status marker, not a queued attempt; success or failure is how a loaded weave
        // learns whether its answer went out.
        return st == ZEN_OK ? loom::Ticket{1} : loom::Ticket{};
    }

    void release_deferred(const loom::DeferredAnswer& answer) override {
        if (host_ != nullptr && host_->release_deferred != nullptr && answer.valid()) {
            host_->release_deferred(host_->ctx, answer.opaque_token());
        }
    }

    // ---- deliberate office authorship across the seam (MSG-07) ---------------
    // The library requests; the host verifies membership and stamps. A missing door (the
    // isolated pipe supplies none) refuses, and never falls back to an ordinary send.

    loom::Ticket office_send(std::string_view as_role, loom::WeaveId target,
                             loom::Message msg) override {
        if (host_ == nullptr || host_->office_send == nullptr) {
            return loom::Ticket{}; // no door: refused, never sent as personal speech
        }
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(as_role); // NUL-terminated for the C ABI
        const ZenStatus st = host_->office_send(
            host_->ctx, role_z.c_str(), target.value, msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
        return st == ZEN_OK ? loom::Ticket{attempt} : loom::Ticket{};
    }

    loom::Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                                     loom::Message msg) override {
        if (host_ == nullptr || host_->office_send_to_role == nullptr) {
            return loom::Ticket{};
        }
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const std::string as_z(as_role);
        const std::string to_z(to_role);
        const ZenStatus st = host_->office_send_to_role(
            host_->ctx, as_z.c_str(), to_z.c_str(), msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
        return st == ZEN_OK ? loom::Ticket{attempt} : loom::Ticket{};
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
        // Unlike an ordinary publish, the count crosses, so an office nobody hears is not
        // confused with a refused one.
        return loom::OfficePublication{true, static_cast<std::size_t>(recipients)};
    }

    // ---- Senses -------------------------------------------------------------
    // The four verbs a native weave reaches, forwarded. A missing door refuses rather than
    // doing nothing.

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

    // ---- Joint publication: the claimant's one verb across the seam ---------------
    // A missing door refuses with `NoLiveDelivery`. The operator's verbs do not cross: a loaded
    // weave inherits `loom::Bus`'s refusing defaults for them, since the authority is a native
    // capability the host mints (docs/reference/joint-publication.md#what-crosses-the-abi).
    loom::JointResult offer_claim(std::uint64_t op, loom::Value value) override {
        if (host_ == nullptr || host_->sense_offer == nullptr) {
            return loom::JointResult{false, loom::JointRefusal::NoLiveDelivery};
        }
        const std::string bytes = loom::serialize(value);
        const ZenStatus st = host_->sense_offer(
            host_->ctx, op, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (st == ZEN_OK) {
            return loom::JointResult{true, loom::JointRefusal::None};
        }
        return loom::JointResult{false, joint_refusal_of(st)};
    }

private:
    /// ZEN_ERR_JOINT_BASE minus the enumerator, back to the enumerator; anything else reads as
    /// `NoLiveDelivery`.
    static loom::JointRefusal joint_refusal_of(ZenStatus st) {
        const int why = ZEN_ERR_JOINT_BASE - st;
        if (why > 0 && why <= static_cast<int>(loom::JointRefusal::Cancelled)) {
            return static_cast<loom::JointRefusal>(why);
        }
        return loom::JointRefusal::NoLiveDelivery;
    }

    /// The host's status, back to the four refusals; an unknown status becomes `NoClaim`, never
    /// a success.
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

    /// Rebuild a reading library-side, re-admitting the host's bytes against this library's own
    /// schema, so the library never holds a value its own gate did not pass.
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
        // Separate from the life: a live replacement changes the incarnation, not the life.
        out.by.author_incarnation_is_current = by.author_incarnation_is_current != 0;
        // Exactly what the host wrote, at any length; empty is personal.
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
        // The ask, if the Weave declared one with ZEN_ASK: surfaced as advice, never a grant.
        std::optional<loom::CapabilityAsk> ask;
        if constexpr (requires { s->zen_requested_capabilities(); }) {
            ask = s->zen_requested_capabilities();
        }
        // The declared claim-set, so what the artifact may claim is known at load.
        const std::vector<std::shared_ptr<const loom::Schema>> claims = s->claimed_schemas();
        // The declared emit-set, which the host claims into its agreement wall beside the
        // accept-set so a divergent definition refuses at load. Read through the virtual a native
        // weave answers, so a raw `loom::Weave` sends no section. Vocabulary only.
        const std::vector<std::shared_ptr<const loom::Schema>> emits = s->emitted_schemas();
        sink_write(sink, loom::serialize(loom::encode_manifest(accepted, state.schema(),
                                                                     ask ? &*ask : nullptr,
                                                                     &claims, &emits)));
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

/// A joint-published value of one of this weave's claims, re-admitted against its declared
/// claim-set before `claim_published` sees it, as `do_revive` does for state. Applied returns
/// ZEN_OK, Declined ZEN_CLAIM_DECLINED, Failed ZEN_ERR; an exception from the maker's handler is
/// caught here and is Failed, and bytes this library's gate refuses are Failed too
/// (ZEN_ERR_UNKNOWN_SCHEMA, ZEN_ERR_REFUSED). The host's mapping is on the slot in abi.h.
template <class S>
ZenStatus do_claim_published(void* instance, const std::uint8_t* value, std::size_t len) {
    try {
        S* s = static_cast<S*>(instance);
        loom::Unverified u = loom::parse(as_view(value, len));
        std::shared_ptr<const loom::Schema> door;
        for (auto& sc : s->claimed_schemas()) {
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
        switch (s->claim_published(a.value())) {
        case loom::Weave::PublishedClaim::Applied:
            return ZEN_OK;
        case loom::Weave::PublishedClaim::Declined:
            return ZEN_CLAIM_DECLINED;
        case loom::Weave::PublishedClaim::Failed:
            return ZEN_ERR;
        }
        return ZEN_ERR;
    } catch (...) {
        return ZEN_ERR;
    }
}

template <class S>
ZenStatus do_revive(void* instance, const std::uint8_t* state, std::size_t len) {
    try {
        S* s = static_cast<S*>(instance);
        loom::Unverified u = loom::parse(as_view(state, len));
        // The host admitted these bytes; they are re-admitted against the library's own state
        // schema to rebuild the Value revive() takes.
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

/// Translate the host's provenance and authored office back into the C++ fact, for this
/// library's own dispatch only: the outbound callbacks have no field to carry one.
inline loom::Provenance provenance_from(std::uint32_t flags, std::int64_t sequence,
                                        const char* authored_role) {
    loom::Provenance p;
    switch (flags) {
    case ZEN_PROV_ANSWER:
        p = loom::Provenance::attested(loom::Provenance::Kind::Answer, 0);
        break;
    case ZEN_PROV_DISPATCH_REFUSAL:
        p = loom::Provenance::attested(loom::Provenance::Kind::DispatchRefusal, 0);
        break;
    case ZEN_PROV_ACTIVATION:
        p = loom::Provenance::attested(loom::Provenance::Kind::Activation, sequence);
        break;
    default:
        break;
    }
    // The authored office composes with any Kind; NULL or empty is personal speech.
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

// Generate the C ABI for a Weave class: thunks with C linkage forwarding to the helpers above.
// Exactly one ZEN_EXPORT_WEAVE per library.
//
// The descriptor is initialized by name (KERN-04): `describe`, `snapshot` and `policy` share one
// function-pointer type, so a positional initializer could swap them with no diagnostic. The
// host also re-admits each door's output against the schema that door must emit, so a miswire
// is refused at load (suite `kernel`). Designators are in declaration order, the one form both
// C++20 and C accept.
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
    static ZenStatus zen__abi_claim_published(void* i, const uint8_t* v, size_t n) {              \
        return ::loom::detail::do_claim_published<WeaveClass>(i, v, n);                     \
    }                                                                                              \
    ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {                                      \
        static const ZenWeaveAbi abi = {.abi_version     = ZEN_ABI_VERSION,                         \
                                        .create          = zen__abi_create,                        \
                                        .destroy         = zen__abi_destroy,                       \
                                        .describe        = zen__abi_describe,                      \
                                        .snapshot        = zen__abi_snapshot,                      \
                                        .policy          = zen__abi_policy,                        \
                                        .revive          = zen__abi_revive,                        \
                                        .handle          = zen__abi_handle,                        \
                                        .claim_published = zen__abi_claim_published};              \
        return &abi;                                                                               \
    }                                                                                              \
    }

#endif // ZEN_KERNEL_EXPORT_HPP
