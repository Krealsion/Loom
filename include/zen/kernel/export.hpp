// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_EXPORT_HPP
#define ZEN_KERNEL_EXPORT_HPP

// The weaving layer: a weaver writes an ordinary loom::Weave subclass and adds
// ZEN_EXPORT_WEAVE(MyWeave), which generates the C ABI: the descriptor, every thunk and the one
// exported symbol. The same Weave compiles in or ships in a library. The thunks serialize
// Values for the host, rebuild a Bus that forwards across the host callbacks, and turn every
// C++ exception into a status: none crosses the seam. docs/reference/dynamic-abi.md

#include <zen/kernel/abi.h>
#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/value.hpp>
#include <zen/weave/weave.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
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
// Ordinary and role sends return the real queued attempt; answer tickets are status markers,
// and publication counts do not cross.

/// The host status, back onto the ticket a native weave would have been handed.
inline loom::Ticket ticket_from(ZenStatus status, std::uint64_t attempt) {
    if (status == ZEN_OK) {
        return loom::Ticket{attempt};
    }
    if (status == ZEN_ERR_PENDING_PER_SENDER) {
        return loom::Ticket{0, loom::RefusalReason::PendingPerSender};
    }
    if (status == ZEN_ERR_ROLE_AUTHORSHIP_DENIED) {
        return loom::Ticket{0, loom::RefusalReason::RoleAuthorshipDenied};
    }
#if ZEN_ABI_VERSION >= 15
    if (status == ZEN_ERR_ASK_ENDED_NOT_ACCEPTED) {
        return loom::Ticket{0, loom::RefusalReason::AskEndedNotAccepted};
    }
#endif
    return loom::Ticket{};
}

// A host door this library may call: the host's table reaches it and fills it. A slot the
// table does not reach is a door this host does not have, refused as a NULL one is.
#define LOOM_HOST_DOOR(slot)                                                                       \
    (host_ != nullptr && ZEN_ABI_REACHES(host_, ZenHostApi, slot) && host_->slot != nullptr)

class HostApiBus final : public loom::Bus {
public:
    explicit HostApiBus(const ZenHostApi* host) : host_(host) {}

    loom::Ticket send(loom::WeaveId target, loom::Message msg) override {
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        if (LOOM_HOST_DOOR(send)) {
            const ZenStatus status = host_->send(host_->ctx, target.value, msg.reply_to.value, msg.correlation,
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
            return ticket_from(status, attempt);
        }
        return loom::Ticket{};
    }

    std::size_t publish(loom::Message msg) override {
        const std::string bytes = loom::serialize(msg.payload);
        if (LOOM_HOST_DOOR(publish)) {
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
        if (LOOM_HOST_DOOR(send_to_role)) {
            const ZenStatus status = host_->send_to_role(host_->ctx, role_z.c_str(), msg.reply_to.value, msg.correlation,
                                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
            return ticket_from(status, attempt);
        }
        return loom::Ticket{};
    }

    /// The immediate answer across the seam (ANS-06): the same `mail.answer()` a native weave
    /// makes, answered by the host.
    loom::Ticket answer(loom::Message msg) override {
        if (!LOOM_HOST_DOOR(answer)) {
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
        if (!LOOM_HOST_DOOR(defer_answer)) {
            return loom::DeferredAnswer{};
        }
        return loom::DeferredAnswer::from_host_token(host_->defer_answer(host_->ctx));
    }

    loom::Ticket spend_deferred(const loom::DeferredAnswer& answer, loom::Message msg) override {
        if (!LOOM_HOST_DOOR(answer_deferred) || !answer.valid()) {
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
        if (LOOM_HOST_DOOR(release_deferred) && answer.valid()) {
            host_->release_deferred(host_->ctx, answer.opaque_token());
        }
    }

#if ZEN_ABI_VERSION >= 15
    // ---- asks across the seam (ANS-08) --------------------------------------
    // One host door for the four forms. A host without it (one before 15, or the isolated pipe)
    // refuses the ask, and it is never sent as a plain send instead. An image built against an
    // earlier header has no ask, as it had no door to ask through.

    loom::Ticket ask(loom::WeaveId target, loom::Message msg) override {
        return ask_through(nullptr, target, nullptr, msg);
    }
    loom::Ticket ask_role(std::string_view role, loom::Message msg) override {
        const std::string to_z(role);
        return ask_through(nullptr, loom::WeaveId{}, to_z.c_str(), msg);
    }
    loom::Ticket role_ask(std::string_view as_role, loom::WeaveId target,
                          loom::Message msg) override {
        const std::string as_z(as_role);
        return ask_through(as_z.c_str(), target, nullptr, msg);
    }
    loom::Ticket role_ask_role(std::string_view as_role, std::string_view to_role,
                               loom::Message msg) override {
        const std::string as_z(as_role);
        const std::string to_z(to_role);
        return ask_through(as_z.c_str(), loom::WeaveId{}, to_z.c_str(), msg);
    }
#endif

    // ---- deliberate role authorship across the seam (MSG-07) ---------------
    // The library requests; the host verifies membership and stamps. A missing door (the
    // isolated pipe supplies none) refuses, and never falls back to an ordinary send.

    loom::Ticket role_send(std::string_view as_role, loom::WeaveId target,
                             loom::Message msg) override {
        if (!LOOM_HOST_DOOR(role_send)) {
            return loom::Ticket{}; // no door: refused, never sent as personal speech
        }
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(as_role); // NUL-terminated for the C ABI
        const ZenStatus st = host_->role_send(
            host_->ctx, role_z.c_str(), target.value, msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
        return ticket_from(st, attempt);
    }

    loom::Ticket role_send_to_role(std::string_view as_role, std::string_view to_role,
                                     loom::Message msg) override {
        if (!LOOM_HOST_DOOR(role_send_to_role)) {
            return loom::Ticket{};
        }
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const std::string as_z(as_role);
        const std::string to_z(to_role);
        const ZenStatus st = host_->role_send_to_role(
            host_->ctx, as_z.c_str(), to_z.c_str(), msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
        return ticket_from(st, attempt);
    }

    loom::RolePublication role_publish(std::string_view as_role,
                                           loom::Message msg) override {
        if (!LOOM_HOST_DOOR(role_publish)) {
            return loom::RolePublication{};
        }
        const std::string bytes = loom::serialize(msg.payload);
        const std::string role_z(as_role);
        std::uint64_t recipients = 0;
        const ZenStatus st = host_->role_publish(
            host_->ctx, role_z.c_str(), msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &recipients);
        if (st == ZEN_ERR_PENDING_PER_SENDER) {
            loom::RolePublication published{true, static_cast<std::size_t>(recipients)};
            published.refusal = loom::RefusalReason::PendingPerSender;
            return published;
        }
        if (st != ZEN_OK) {
            return loom::RolePublication{};
        }
        // Unlike an ordinary publish, the count crosses, so a role nobody hears is not
        // confused with a refused one.
        return loom::RolePublication{true, static_cast<std::size_t>(recipients)};
    }

    // ---- Senses -------------------------------------------------------------
    // The four verbs a native weave reaches, forwarded. A missing door refuses rather than
    // doing nothing.

    loom::SenseClaimResult claim(loom::Value value) override {
        if (!LOOM_HOST_DOOR(sense_claim)) {
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

    loom::SenseClaimResult role_claim(std::string_view as_role, loom::Value value) override {
        if (!LOOM_HOST_DOOR(sense_role_claim)) {
            return loom::SenseClaimResult{};
        }
        const std::string bytes = loom::serialize(value);
        const std::string role_z(as_role);
        std::uint64_t revision = 0;
        const ZenStatus st = host_->sense_role_claim(
            host_->ctx, role_z.c_str(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size(), &revision);
        return st == ZEN_OK ? loom::SenseClaimResult{true, loom::SenseRefusal::None, revision}
                            : loom::SenseClaimResult{false, sense_refusal_of(st), 0};
    }

    loom::SenseReading observe(loom::WeaveId author, std::shared_ptr<const loom::Schema> shape) override {
        if (!LOOM_HOST_DOOR(sense_observe)) {
            return loom::SenseReading{};
        }
        const std::string name_z(shape->name());
        HeldBytes bytes;
        HeldBytes authored; // the role authored as: grows to whatever the host wrote, never truncated
        ZenSenseBy by{};
        by.size = sizeof(ZenSenseBy);
        ZenByteSink sink{&bytes, &collect_bytes};
        ZenByteSink role_sink{&authored, &collect_bytes};
        const ZenStatus st = host_->sense_observe(host_->ctx, author.value, name_z.c_str(),
                                                  shape->version(), sink, role_sink, &by);
        return decode_reading(st, bytes, authored, by, shape);
    }

    loom::SenseReading observe_role(std::string_view role, std::shared_ptr<const loom::Schema> shape) override {
        if (!LOOM_HOST_DOOR(sense_observe_role)) {
            return loom::SenseReading{};
        }
        const std::string role_z(role);
        const std::string name_z(shape->name());
        HeldBytes bytes;
        HeldBytes authored; // the role authored as: grows to whatever the host wrote, never truncated
        ZenSenseBy by{};
        by.size = sizeof(ZenSenseBy);
        ZenByteSink sink{&bytes, &collect_bytes};
        ZenByteSink role_sink{&authored, &collect_bytes};
        const ZenStatus st = host_->sense_observe_role(
            host_->ctx, role_z.c_str(), name_z.c_str(), shape->version(), sink, role_sink, &by);
        return decode_reading(st, bytes, authored, by, shape);
    }

    // ---- Joint publication: the claimant's one verb across the seam ---------------
    // A missing door refuses with `NoLiveDelivery`. The initiator's verbs do not cross: a loaded
    // weave inherits `loom::Bus`'s refusing defaults for them, since the authority is a native
    // capability the host mints (docs/reference/joint-publication.md#what-crosses-the-abi).
    loom::JointResult offer_claim(std::uint64_t op, loom::Value value) override {
        if (!LOOM_HOST_DOOR(sense_offer)) {
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
#if ZEN_ABI_VERSION >= 15
    loom::Ticket ask_through(const char* as_role, loom::WeaveId target, const char* to_role,
                             const loom::Message& msg) {
        if (!LOOM_HOST_DOOR(ask)) {
            return loom::Ticket{}; // no door: refused, never sent as a plain send
        }
        std::uint64_t attempt = 0;
        const std::string bytes = loom::serialize(msg.payload);
        const ZenStatus st = host_->ask(
            host_->ctx, as_role, target.value, to_role, msg.reply_to.value, msg.correlation,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &attempt);
        return ticket_from(st, attempt);
    }
#endif

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
        case ZEN_ERR_SENSE_ROLE_NOT_HELD:
            return loom::SenseRefusal::RoleNotHeld;
        case ZEN_ERR_REFUSED:
            return loom::SenseRefusal::GateRefused;
        default:
            return loom::SenseRefusal::NoClaim;
        }
    }

    /// What the host hands this library through a byte sink, and whether the library held all of
    /// it: the sink is called from the host's frames, which no exception of this library's may
    /// reach, so a write it could not hold is recorded instead.
    struct HeldBytes {
        std::string bytes;
        bool lost = false;
    };

    static void collect_bytes(void* ctx, const std::uint8_t* data, std::size_t len) {
        auto* out = static_cast<HeldBytes*>(ctx);
        try {
            out->bytes.append(reinterpret_cast<const char*>(data), len);
        } catch (...) {
            out->lost = true;
        }
    }

    /// Rebuild a reading library-side, re-admitting the host's bytes against this library's own
    /// schema, so the library never holds a value its own gate did not pass.
    loom::SenseReading decode_reading(ZenStatus st, const HeldBytes& bytes,
                                      const HeldBytes& authored, const ZenSenseBy& by,
                                      const std::shared_ptr<const loom::Schema>& shape) {
        loom::SenseReading out;
        if (st != ZEN_OK) {
            out.refusal = sense_refusal_of(st);
            return out;
        }
        // A reading this library could not hold whole is not admitted as one.
        if (bytes.lost || authored.lost) {
            out.refusal = loom::SenseRefusal::GateRefused;
            return out;
        }
        loom::Unverified u = loom::parse(bytes.bytes);
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
        out.by.role = authored.bytes;
        out.by.role_holder_is_current = by.role_holder_is_current != 0;
        out.by.revision = by.revision;
        out.by.schema_name = shape->name();
        out.by.schema_version = shape->version();
        return out;
    }

    const ZenHostApi* host_;
};

#undef LOOM_HOST_DOOR

// ---- the per-method helpers the generated thunks forward to ------------------

// A making refused: what the constructor threw, written to the host's sink as its words; an
// exception that is no std::exception says nothing.
/// Admit bytes at one of this library's own doors. An Any field names a shape the door does not,
/// and the library reads it against the shapes it declares itself: its doors, its state, its
/// claims and its emits, and every shape they nest. A shape it does not declare is not one it
/// can read, and the value is refused as any value its gate does not pass.
template <class S>
loom::Admission admit_declared(const S& s, const loom::Unverified& u,
                               const std::shared_ptr<const loom::Schema>& door) {
    loom::Admission a = loom::admit(u, door);
    if (a.ok() || a.first_error().kind != loom::ErrorKind::UnknownSchema ||
        a.first_error().path.empty()) {
        return a; // nothing open in it, or refused for another reason
    }
    loom::Registry own;
    try {
        std::vector<std::shared_ptr<const loom::Schema>> roots = s.accepted_schemas();
        for (const auto& more : {s.claimed_schemas(), s.emitted_schemas()}) {
            roots.insert(roots.end(), more.begin(), more.end());
        }
        roots.push_back(s.snapshot().schema_ptr());
        std::vector<std::shared_ptr<const loom::Schema>> all;
        for (const auto& r : roots) {
            loom::collect_referenced(*r, all);
            all.push_back(r);
        }
        for (const auto& sc : all) {
            own.register_schema(sc);
        }
    } catch (const std::exception&) {
        return a; // a vocabulary that does not agree with itself reads nothing open
    }
    return loom::admit(u, door, own);
}

/// What a weave's constructor threw, as the words of a refused making.
inline void say_refusal(ZenByteSink refusal, std::string_view words) {
    if (refusal.write != nullptr && !words.empty()) {
        refusal.write(refusal.ctx, reinterpret_cast<const std::uint8_t*>(words.data()),
                      words.size());
    }
}

template <class S>
void* do_create(ZenByteSink refusal) {
    try {
        if constexpr (std::is_default_constructible_v<S>) {
            return static_cast<void*>(new S());
        } else {
            // A weave made with a value is never made from nothing: the host gates a load's value
            // against its declared shape and calls `create_with`, and refuses a load naming none.
            say_refusal(refusal, "this weave is made with a value, and none was given");
            return nullptr;
        }
    } catch (const std::exception& e) {
        const std::string_view words = e.what();
        if (refusal.write != nullptr && !words.empty()) {
            refusal.write(refusal.ctx, reinterpret_cast<const std::uint8_t*>(words.data()),
                          words.size());
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

/// The shape the weave declares it is made with, as zen.Creation, or nothing for one made with
/// none. Reads the declaration alone: no instance, and no value.
template <class S>
ZenStatus do_describe_creation(ZenByteSink sink) {
    try {
        using T = loom::made_with_t<S>;
        if constexpr (!std::is_void_v<T>) {
            sink_write(sink, loom::serialize(loom::encode_creation(*loom::schema_of<T>())));
        }
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

/// Make the weave from the value the host admitted against its declared shape, re-admitted here
/// against this library's own definition, as `do_revive` does state. A weave made with none is
/// made with nothing, and refuses a value.
template <class S>
void* do_create_with(ZenByteSink refusal, const std::uint8_t* value, std::size_t len) {
    try {
        using T = loom::made_with_t<S>;
        if constexpr (std::is_void_v<T>) {
            (void)value, (void)len;
            say_refusal(refusal, "this weave is made with no value, and one was given");
            return nullptr;
        } else {
            loom::Admission a = loom::admit(loom::parse(as_view(value, len)), loom::schema_of<T>());
            if (!a.ok()) {
                say_refusal(refusal, "the creation value is refused by this weave's gate: " +
                                         a.first_error().message());
                return nullptr;
            }
            return static_cast<void*>(new S(loom::from_value<T>(a.value())));
        }
    } catch (const std::exception& e) {
        say_refusal(refusal, e.what());
        return nullptr;
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
/// ZEN_OK, Declined ZEN_CLAIM_DECLINED, Failed ZEN_ERR; an exception from the weaver's handler is
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
        loom::Admission a = admit_declared(*s, u, door);
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
        loom::Admission a = admit_declared(*s, u, probe.schema_ptr());
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        s->revive(a.value());
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

/// Translate the host's provenance and authored role back into the C++ fact, for this
/// library's own dispatch only: the outbound callbacks have no field to carry one. A value this
/// library does not know is an ordinary message.
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
#if ZEN_ABI_VERSION >= 15
    case ZEN_PROV_ASK_ENDED:
        p = loom::Provenance::attested(loom::Provenance::Kind::AskEnded, 0);
        break;
#endif
    default:
        break;
    }
    // The authored role composes with any Kind; NULL or empty is personal speech.
    if (authored_role != nullptr && authored_role[0] != '\0') {
        p = std::move(p).with_authored_role(std::string(authored_role));
    }
    return p;
}

/// One fact of the host's delivery, read where the host's size reaches it, and absent past it.
#define LOOM_DELIVERY_FACT(field, absent)                                                          \
    (d != nullptr && ZEN_ABI_REACHES(d, ZenDelivery, field) ? d->field : (absent))

template <class S>
ZenStatus do_handle(void* instance, const ZenDelivery* d, const std::uint8_t* payload,
                    std::size_t len, const ZenHostApi* host) {
    try {
        const std::uint64_t sender = LOOM_DELIVERY_FACT(sender, std::uint64_t{0});
        const std::uint64_t reply_to = LOOM_DELIVERY_FACT(reply_to, std::uint64_t{0});
        const std::uint64_t correlation = LOOM_DELIVERY_FACT(correlation, std::uint64_t{0});
        const std::uint32_t provenance = LOOM_DELIVERY_FACT(provenance, std::uint32_t{ZEN_PROV_NONE});
        const std::int64_t attested_sequence = LOOM_DELIVERY_FACT(attested_sequence, std::int64_t{0});
        const char* authored_role = LOOM_DELIVERY_FACT(authored_role, nullptr);
        const char* addressed_role = LOOM_DELIVERY_FACT(addressed_role, nullptr);
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
        loom::Admission a = admit_declared(*s, u, door);
        if (!a.ok()) {
            return ZEN_ERR_REFUSED;
        }
        loom::Message msg(a.value(), loom::WeaveId{sender}, loom::WeaveId{reply_to},
                             correlation);
        msg.provenance = provenance_from(provenance, attested_sequence, authored_role);
        if (addressed_role != nullptr) {
            msg.addressed_role = addressed_role;
        }
        HostApiBus bus(host);
        s->handle(msg, bus);
        return ZEN_OK;
    } catch (...) {
        return ZEN_ERR;
    }
}

#undef LOOM_DELIVERY_FACT

} // namespace loom::detail

// The slots each version appended, filled where the header this image is built against has them,
// so the macro builds an image against any header a host still loads.
#if ZEN_ABI_VERSION >= 14
#define ZEN_DETAIL_CREATION_THUNKS(WeaveClass)                                                     \
    static ZenStatus zen__abi_describe_creation(ZenByteSink s) {                                   \
        return ::loom::detail::do_describe_creation<WeaveClass>(s);                                \
    }                                                                                              \
    static void* zen__abi_create_with(ZenByteSink r, const uint8_t* v, size_t n) {                 \
        return ::loom::detail::do_create_with<WeaveClass>(r, v, n);                                \
    }
#define ZEN_DETAIL_CREATION_SLOTS                                                                  \
    , .describe_creation = zen__abi_describe_creation, .create_with = zen__abi_create_with
#else
#define ZEN_DETAIL_CREATION_THUNKS(WeaveClass)
#define ZEN_DETAIL_CREATION_SLOTS
#endif

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
    static void* zen__abi_create(ZenByteSink r) { return ::loom::detail::do_create<WeaveClass>(r); } \
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
    static ZenStatus zen__abi_handle(void* i, const ZenDelivery* d, const uint8_t* p, size_t n,    \
                                     const ZenHostApi* h) {                                        \
        return ::loom::detail::do_handle<WeaveClass>(i, d, p, n, h);                               \
    }                                                                                              \
    static ZenStatus zen__abi_claim_published(void* i, const uint8_t* v, size_t n) {              \
        return ::loom::detail::do_claim_published<WeaveClass>(i, v, n);                     \
    }                                                                                              \
    ZEN_DETAIL_CREATION_THUNKS(WeaveClass)                                                          \
    ZEN_KERNEL_EXPORT const ZenWeaveAbi* zen_weave_abi(void) {                                      \
        static const ZenWeaveAbi abi = {.abi_version     = ZEN_ABI_VERSION,                         \
                                        .size            = sizeof(ZenWeaveAbi),                    \
                                        .host_size       = sizeof(ZenHostApi),                     \
                                        .manifest_version = ::loom::kManifestVersion,              \
                                        .create          = zen__abi_create,                        \
                                        .destroy         = zen__abi_destroy,                       \
                                        .describe        = zen__abi_describe,                      \
                                        .snapshot        = zen__abi_snapshot,                      \
                                        .policy          = zen__abi_policy,                        \
                                        .revive          = zen__abi_revive,                        \
                                        .handle          = zen__abi_handle,                        \
                                        .claim_published = zen__abi_claim_published               \
                                        ZEN_DETAIL_CREATION_SLOTS};                                \
        return &abi;                                                                               \
    }                                                                                              \
    }

#endif // ZEN_KERNEL_EXPORT_HPP
