// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/kernel/kernel.hpp>

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace loom {

namespace {

/// The host-side lifetime ledger behind `kernel_lifetime_counts()`. Process-wide,
/// monotonic, single-threaded like everything else here, and read by nothing that
/// makes a decision.
KernelLifetimeCounts& ledger() noexcept {
    static KernelLifetimeCounts counts;
    return counts;
}

// ---- platform loader ------------------------------------------------------

void* lib_open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    // The development/demo backend (see Kernel::containment_note). LoadLibraryA
    // is ANSI: a non-ANSI path is a known, named limitation (a LoadLibraryW +
    // UTF-8→UTF-16 conversion is the follow-on), not a silent one.
    void* h = static_cast<void*>(::LoadLibraryA(path.c_str()));
    if (h == nullptr) {
        error = "LoadLibrary failed (error " + std::to_string(::GetLastError()) +
                "): " + path;
    }
    return h;
#else
    ::dlerror();
    // RTLD_LOCAL keeps the library's symbols (including its own copy of loom)
    // out of the global namespace, so the host and the library never interpose.
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char* e = ::dlerror();
        error = (e != nullptr) ? e : "dlopen failed";
    }
    return h;
#endif
}

// Wrapped, so the ledger counts every successful open at the one place opening
// happens rather than at each of its callers.
void* lib_open_counted(const std::string& path, std::string& error) {
    void* h = lib_open(path, error);
    if (h != nullptr) {
        ++ledger().libraries_opened;
    }
    return h;
}

void* lib_symbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

void lib_close(void* handle) {
    if (handle == nullptr) {
        return;
    }
    ++ledger().libraries_closed;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

// Destroying a library instance goes through here and nowhere else, so the
// ledger's "the host called destroy" count is complete by construction. The
// host is the ONLY caller of abi->destroy, which is what makes counting these
// calls a real proof that no instance is destroyed twice.
void destroy_instance(const ZenWeaveAbi* abi, void* instance) {
    if (abi == nullptr || instance == nullptr) {
        return;
    }
    ++ledger().instances_destroyed;
    abi->destroy(instance);
}

void* create_instance(const ZenWeaveAbi* abi) {
    void* instance = abi->create();
    if (instance != nullptr) {
        ++ledger().instances_created;
    }
    return instance;
}

std::string_view as_view(const std::uint8_t* data, std::size_t len) {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

// Does a published accept-set declare this shape? The one place the question is
// answered, so query_role and accepts() cannot drift apart on what "declares"
// means. (name, version) is the identity routing selects a door by.
bool declares(const std::vector<std::shared_ptr<const Schema>>& accept,
              const std::string& shape_name, std::uint32_t shape_version) {
    for (const auto& s : accept) {
        if (s && s->name() == shape_name && s->version() == shape_version) {
            return true;
        }
    }
    return false;
}

// The comparable identity of one published schema. Reload's accepted-contract
// check compares SETS of these: name and version are what routing selects on,
// content_id is what the shape actually says.
using SchemaIdentity = std::tuple<std::string, std::uint32_t, ContentId>;

std::vector<SchemaIdentity> identities(const std::vector<std::shared_ptr<const Schema>>& schemas) {
    std::vector<SchemaIdentity> out;
    out.reserve(schemas.size());
    for (const auto& s : schemas) {
        if (s) {
            out.emplace_back(s->name(), s->version(), s->content_id());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Exact set equality, order-independent: same count, nothing missing, nothing
// added, nothing changed behind the same (name, version). Sorting rather than
// hashing keeps duplicates significant — two declarations of one shape is not
// the same contract as one, and pretending otherwise would be a quiet widening.
bool same_accepted_contract(const std::vector<std::shared_ptr<const Schema>>& a,
                            const std::vector<std::shared_ptr<const Schema>>& b) {
    return identities(a) == identities(b);
}

// Resolve and validate the descriptor exported by a loaded library.
const ZenWeaveAbi* fetch_abi(void* lib, std::string& error) {
    void* sym = lib_symbol(lib, "zen_weave_abi");
    if (sym == nullptr) {
        error = "library does not export zen_weave_abi";
        return nullptr;
    }
    using AbiFn = const ZenWeaveAbi* (*)(void);
    AbiFn fn = nullptr;
    std::memcpy(&fn, &sym, sizeof(fn)); // object->function pointer, the -Wpedantic-clean way
    const ZenWeaveAbi* abi = fn();
    if (abi == nullptr) {
        error = "zen_weave_abi returned null";
        return nullptr;
    }
    if (abi->abi_version != ZEN_ABI_VERSION) {
        error = "unsupported abi_version " + std::to_string(abi->abi_version) + " (host supports " +
                std::to_string(ZEN_ABI_VERSION) + ")";
        return nullptr;
    }
    return abi;
}

// ---- host callbacks the library calls during handle() ---------------------

// Binds a running delivery so the library's emitted messages are resolved, gated,
// and routed exactly as a native Weave's are. `gated` is the per-delivery
// WeaveBus (it stamps the loaded Weave's id and authorizes against its grant);
// `sb` is the Switchboard, used only for the read-only schema resolution. Valid
// only for the duration of the handle() call.
struct HostCtx {
    loom::Bus* gated;
    loom::Switchboard* sb;
    /// WHO IS SPEAKING (R2E-0). The gated Bus stamps this on everything it
    /// routes, but a seam rejection never reaches the routing path — so the
    /// diagnostic needs the id here to name the artifact that attempted the
    /// emission. It is the host's own record, never anything the library says.
    loom::WeaveId self;
};

/// Report a seam rejection to the host's diagnostics, and return the status the
/// library was always given. ONE place, so no entry point can quietly forget:
/// every `resolve_schema`/`admit` failure below routes through here.
///
/// `target` is the invalid id wherever the emission named no target (a
/// publication, an answer whose conversation partner the seam never looked up) —
/// see RefusalReason::SeamUnresolved on why a fabricated one would be worse than
/// the silence this replaces.
ZenStatus seam_reject(HostCtx* h, const loom::Unverified& u, loom::WeaveId target,
                      const loom::Refusal& refusal) {
    h->sb->note_seam_refusal(h->self, target, u.claimed_name(), u.claimed_version(), refusal);
    return refusal.reason == loom::RefusalReason::SeamUnresolved ? ZEN_ERR_UNKNOWN_SCHEMA
                                                                 : ZEN_ERR_REFUSED;
}

/// The two seam refusals, named once.
inline loom::Refusal seam_unresolved() {
    return loom::Refusal{loom::RefusalReason::SeamUnresolved, {}};
}
inline loom::Refusal seam_gate_refused(const loom::Admission& a) {
    return loom::Refusal{loom::RefusalReason::GateRefused, a.first_error()};
}

} // namespace

// ---- one open dynamic library, owned by everybody who could still need it ----

/// ONE OPEN LIBRARY, CLOSED EXACTLY ONCE, BY THE LAST HOLDER TO LET GO.
///
/// The forbidden outcomes this exists to make unrepresentable rather than merely
/// avoided: closing twice, and closing while code from the library could still
/// execute. Both used to be questions about the ORDER of statements in the
/// Kernel — destroy the instance, *then* close — which is a rule every future
/// call site has to remember. It is now a question about who is still holding a
/// `shared_ptr`, which nobody has to remember:
///
///   the Kernel's Loaded record   holds one while it owns the artifact name
///   the HostAdapter              holds one for exactly as long as an instance
///                                built by that library exists
///
/// So the close cannot precede the instance's destruction (the adapter's share
/// outlives its own destructor body), and cannot happen twice (a refcount reaches
/// zero once).
///
/// Deliberately neither copyable nor movable: this object IS the handle's
/// identity, and "the same library, at a different address" has no meaning.
class LoadedLibrary {
public:
    explicit LoadedLibrary(void* handle) noexcept : handle_(handle) {}
    ~LoadedLibrary() { lib_close(handle_); }

    LoadedLibrary(const LoadedLibrary&) = delete;
    LoadedLibrary& operator=(const LoadedLibrary&) = delete;
    LoadedLibrary(LoadedLibrary&&) = delete;
    LoadedLibrary& operator=(LoadedLibrary&&) = delete;

    void* get() const noexcept { return handle_; }

private:
    void* handle_;
};

namespace {

/// Open `path` and hand back an owner for it, or nullptr with `error` set.
std::shared_ptr<LoadedLibrary> open_library(const std::string& path, std::string& error) {
    void* handle = lib_open_counted(path, error);
    if (handle == nullptr) {
        return nullptr;
    }
    return std::make_shared<LoadedLibrary>(handle);
}

} // namespace

extern "C" {

// Append library bytes into a host std::string. The host copies immediately; no
// library pointer is retained.
static void zen_host_sink(void* ctx, const std::uint8_t* data, std::size_t len) {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
}

// A library Weave sends/publishes by handing the host serialized payload bytes.
// The host admits them through the gate (the DLL-seam boundary) before routing.
static ZenStatus zen_host_send(void* ctx, std::uint64_t target, std::uint64_t reply_to,
                               std::uint64_t correlation, const std::uint8_t* payload,
                               std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door = h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        // The one target the seam CAN honestly name: the library said where.
        return seam_reject(h, u, loom::WeaveId{target}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{target}, seam_gate_refused(a));
    }
    // Route through the gated WeaveBus (it stamps the loaded Weave's id and
    // authorizes against its grant), exactly as a native Weave's send is.
    h->gated->send(loom::WeaveId{target},
                   loom::Message(std::move(a).value(), loom::WeaveId{},
                                    loom::WeaveId{reply_to}, correlation));
    return ZEN_OK;
}

static ZenStatus zen_host_publish(void* ctx, std::uint64_t reply_to, std::uint64_t correlation,
                                  const std::uint8_t* payload, std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door = h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        // A publication names NO target. The diagnostic says so rather than
        // inventing one; fanout never ran, so there is nobody to blame.
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    h->gated->publish(loom::Message(std::move(a).value(), loom::WeaveId{},
                                       loom::WeaveId{reply_to}, correlation));
    return ZEN_OK;
}

// A library Weave sends to a role the same way: hand the host serialized bytes + the
// role; the host admits, stamps the loaded Weave's id, and routes by role.
static ZenStatus zen_host_send_to_role(void* ctx, const char* role, std::uint64_t reply_to,
                                       std::uint64_t correlation, const std::uint8_t* payload,
                                       std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        // A ROLE is a destination slot, not a WeaveId, and it is resolved at
        // delivery — which never happens here. No target is named.
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    h->gated->send_to_role(role, loom::Message(std::move(a).value(), loom::WeaveId{},
                                                  loom::WeaveId{reply_to}, correlation));
    return ZEN_OK;
}

// Deferred answers (R2B-2). Each of these is a thin pass-through to the gated
// WeaveBus of the delivery in progress — which is what makes the token safe: the
// bus checks it against the bound participants and their exact incarnations, and
// the ctx a library can reach is only ever the one for its own live delivery.
static uint64_t zen_host_defer_answer(void* ctx) {
    auto* h = static_cast<HostCtx*>(ctx);
    return h->gated->make_deferred_answer().opaque_token();
}

static ZenStatus zen_host_answer_deferred(void* ctx, uint64_t token, const std::uint8_t* payload,
                                          std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        // The conversation partner is bus-private; the seam never looked it up.
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side as always
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    const loom::Ticket t = h->gated->spend_deferred(
        loom::DeferredAnswer::from_host_token(token),
        loom::Message(std::move(a).value(), loom::WeaveId{}, loom::WeaveId{}, 0));
    return t.valid() ? ZEN_OK : ZEN_ERR_REFUSED;
}

static ZenStatus zen_host_answer(void* ctx, const std::uint8_t* payload, std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side as always
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    // Straight to the gated WeaveBus of the delivery in progress — the SAME
    // trusted operation `mail.answer()` reaches natively. The host owns the
    // recipient, the correlation and the requester-target provenance; the library
    // supplied only what it wanted to say.
    const loom::Ticket t = h->gated->answer(loom::Message(std::move(a).value()));
    return t.valid() ? ZEN_OK : ZEN_ERR_REFUSED;
}

static void zen_host_release_deferred(void* ctx, uint64_t token) {
    auto* h = static_cast<HostCtx*>(ctx);
    h->gated->release_deferred(loom::DeferredAnswer::from_host_token(token));
}

// Deliberate office authorship (v5). Same shape as every outbound door: the
// payload bytes are admitted through the gate host-side, then routed through the
// gated WeaveBus — which stamps the loaded weave's id from the CONNECTION and
// asks the Switchboard's authorship door to verify membership. The library
// requested "speak as R"; the host decided whether that is true. An invalid
// result maps to the precise status, so a loaded office learns "you do not hold
// that role" rather than a generic failure.
static ZenStatus zen_host_office_send(void* ctx, const char* as_role, std::uint64_t target,
                                      std::uint64_t reply_to, std::uint64_t correlation,
                                      const std::uint8_t* payload, std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return seam_reject(h, u, loom::WeaveId{target}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side as always
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{target}, seam_gate_refused(a));
    }
    const loom::Ticket t = h->gated->office_send(
        as_role, loom::WeaveId{target},
        loom::Message(std::move(a).value(), loom::WeaveId{}, loom::WeaveId{reply_to},
                      correlation));
    // On this door an invalid ticket means exactly one thing: the authorship was
    // refused (shape and schema failures returned above).
    return t.valid() ? ZEN_OK : ZEN_ERR_ROLE_AUTHORSHIP_DENIED;
}

static ZenStatus zen_host_office_send_to_role(void* ctx, const char* as_role, const char* to_role,
                                              std::uint64_t reply_to, std::uint64_t correlation,
                                              const std::uint8_t* payload, std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    const loom::Ticket t = h->gated->office_send_to_role(
        as_role, to_role,
        loom::Message(std::move(a).value(), loom::WeaveId{}, loom::WeaveId{reply_to},
                      correlation));
    return t.valid() ? ZEN_OK : ZEN_ERR_ROLE_AUTHORSHIP_DENIED;
}

static ZenStatus zen_host_office_publish(void* ctx, const char* as_role, std::uint64_t reply_to,
                                         std::uint64_t correlation, const std::uint8_t* payload,
                                         std::size_t len, std::uint64_t* recipients_out) {
    auto* h = static_cast<HostCtx*>(ctx);
    if (recipients_out != nullptr) {
        *recipients_out = 0;
    }
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    const loom::OfficePublication p = h->gated->office_publish(
        as_role, loom::Message(std::move(a).value(), loom::WeaveId{}, loom::WeaveId{reply_to},
                               correlation));
    if (!p.authored) {
        return ZEN_ERR_ROLE_AUTHORSHIP_DENIED;
    }
    if (recipients_out != nullptr) {
        *recipients_out = p.recipients;
    }
    return ZEN_OK;
}

// ---- Senses (v6) -----------------------------------------------------------
//
// Same shape as every other outbound door: the value crosses as bytes, the HOST
// admits it through the one gate, and the host — not the library — knows which
// weave is speaking, whether it declared the shape, and whether it holds the
// office. A library requests; it never attests.

/// The four distinct Sense refusals, back to the seam's four distinct statuses.
static ZenStatus sense_status_of(loom::SenseRefusal why) {
    switch (why) {
    case loom::SenseRefusal::NoClaim:
        return ZEN_ERR_SENSE_NO_CLAIM;
    case loom::SenseRefusal::NotAuthorized:
        return ZEN_ERR_SENSE_NOT_AUTHORIZED;
    case loom::SenseRefusal::Undeclared:
        return ZEN_ERR_SENSE_UNDECLARED;
    case loom::SenseRefusal::OfficeNotHeld:
        return ZEN_ERR_SENSE_OFFICE_NOT_HELD;
    case loom::SenseRefusal::GateRefused:
        return ZEN_ERR_REFUSED;
    case loom::SenseRefusal::None:
        break;
    }
    return ZEN_OK;
}

/// Fill the C-layout authorship from the host's own reading. `office` is copied
/// into the fixed buffer and always NUL-terminated; a name at or past the bound
/// is truncated rather than allocated across the seam — see ZenSenseBy.
static void fill_sense_by(const loom::SenseReading& r, ZenSenseBy* by) {
    if (by == nullptr) {
        return;
    }
    *by = ZenSenseBy{};
    by->author = r.by.author.value;
    by->author_life = r.by.author_life;
    by->author_incarnation = r.by.author_incarnation;
    by->author_life_is_current = r.by.author_life_is_current ? 1u : 0u;
    by->office_holder_is_current = r.by.office_holder_is_current ? 1u : 0u;
    by->revision = r.by.revision;
    const std::size_t n = r.by.office.size() < (ZEN_SENSE_OFFICE_MAX - 1)
                              ? r.by.office.size()
                              : (ZEN_SENSE_OFFICE_MAX - 1);
    std::memcpy(by->office, r.by.office.data(), n);
    by->office[n] = '\0';
}

static ZenStatus zen_host_sense_claim(void* ctx, const std::uint8_t* payload, std::size_t len,
                                      std::uint64_t* revision_out) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        // A claim of a shape this Loom never heard of is a seam rejection like
        // any other, and is observable for the same reason (R2E-0 / P-011).
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    const loom::SenseClaimResult r = h->gated->claim(std::move(a).value());
    if (revision_out != nullptr) {
        *revision_out = r.revision;
    }
    return r.accepted ? ZEN_OK : sense_status_of(r.why);
}

static ZenStatus zen_host_sense_office_claim(void* ctx, const char* as_role,
                                             const std::uint8_t* payload, std::size_t len,
                                             std::uint64_t* revision_out) {
    auto* h = static_cast<HostCtx*>(ctx);
    loom::Unverified u = loom::parse(loom::as_view(payload, len));
    std::shared_ptr<const loom::Schema> door =
        h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return seam_reject(h, u, loom::WeaveId{}, seam_unresolved());
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return seam_reject(h, u, loom::WeaveId{}, seam_gate_refused(a));
    }
    const loom::SenseClaimResult r =
        h->gated->office_claim(as_role != nullptr ? as_role : "", std::move(a).value());
    if (revision_out != nullptr) {
        *revision_out = r.revision;
    }
    return r.accepted ? ZEN_OK : sense_status_of(r.why);
}

static ZenStatus zen_host_sense_observe(void* ctx, std::uint64_t author, const char* shape_name,
                                        std::uint32_t shape_version, ZenByteSink sink,
                                        ZenSenseBy* by) {
    auto* h = static_cast<HostCtx*>(ctx);
    // The host resolves the shape it was NAMED. A library asking for a shape this
    // Loom never registered is the same seam rejection as an emission of one, and
    // is observable for the same reason (R2E-0 / P-011).
    std::shared_ptr<const loom::Schema> shape =
        h->sb->resolve_schema(shape_name != nullptr ? shape_name : "", shape_version);
    if (!shape) {
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    const loom::SenseReading r = h->gated->observe(loom::WeaveId{author}, std::move(shape));
    if (!r) {
        return sense_status_of(r.refusal);
    }
    fill_sense_by(r, by);
    // The value crosses as BYTES the library copies out of the sink — the same
    // ownership rule every other outbound value follows. No host pointer into
    // the repository ever reaches the library, so a loaded reader has exactly the
    // reach a native one has: none.
    const std::string bytes = loom::serialize(*r.value);
    sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return ZEN_OK;
}

static ZenStatus zen_host_sense_observe_office(void* ctx, const char* role,
                                               const char* shape_name,
                                               std::uint32_t shape_version, ZenByteSink sink,
                                               ZenSenseBy* by) {
    auto* h = static_cast<HostCtx*>(ctx);
    std::shared_ptr<const loom::Schema> shape =
        h->sb->resolve_schema(shape_name != nullptr ? shape_name : "", shape_version);
    if (!shape) {
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    const loom::SenseReading r =
        h->gated->observe_office(role != nullptr ? role : "", std::move(shape));
    if (!r) {
        return sense_status_of(r.refusal);
    }
    fill_sense_by(r, by);
    const std::string bytes = loom::serialize(*r.value);
    sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return ZEN_OK;
}

} // extern "C"

// ---- the host adapter: a Weave backed by a library instance ---------------

class HostAdapter final : public loom::Weave {
public:
    HostAdapter(const ZenWeaveAbi* abi, void* instance, std::shared_ptr<LoadedLibrary> lib,
                std::vector<std::shared_ptr<const Schema>> accepted,
                std::shared_ptr<const Schema> state_schema, loom::Switchboard* bus,
                std::vector<std::shared_ptr<const Schema>> claims = {})
        : abi_(abi), instance_(instance), lib_(std::move(lib)), accepted_(std::move(accepted)),
          claims_(std::move(claims)), state_schema_(std::move(state_schema)), bus_(bus) {}

    /// THE ONE PLACE A LIVE DYNAMIC ARTIFACT ENDS — whoever caused it.
    ///
    /// The Switchboard owns this object, so this runs when the Kernel unloads,
    /// when a host hands the adapter back and drops it, and when a prepared
    /// replacement aborts deep inside a delivery and discards its candidate. That
    /// last one is why the notification lives here and not in a hook the
    /// Switchboard calls: the bus does not know a Kernel exists, and the only
    /// thing that is true in every case is that this destructor ran.
    ///
    /// ORDER, and it is guaranteed by the language rather than by care: the body
    /// destroys the instance and tells the Kernel to release the artifact record;
    /// `lib_` — a MEMBER — is released afterwards. So the library cannot close
    /// before the instance built from it is gone, whichever share is the last.
    ~HostAdapter() override {
        destroy_instance(abi_, instance_);
        if (owner_ != nullptr) {
            owner_->adapter_destroyed(artifact_, this);
        }
    }

    void set_self(loom::WeaveId id) { self_ = id; }
    const std::shared_ptr<const Schema>& state_schema() const { return state_schema_; }

    /// Bind this adapter to the Kernel record that names it, and to nothing
    /// otherwise: an adapter with no owner (one whose registration threw before a
    /// record existed) simply destroys its instance and goes.
    void attach(loom::Kernel* owner, std::string artifact) {
        owner_ = owner;
        artifact_ = std::move(artifact);
    }
    void detach() noexcept { owner_ = nullptr; }

    // Swap the backing library in place (hot-reload), destroying the old
    // instance while the old library is still open — which the argument order
    // guarantees: `lib_` is not reassigned until after the destroy above it.
    void rebind(const ZenWeaveAbi* new_abi, void* new_instance,
                std::shared_ptr<LoadedLibrary> new_lib) {
        destroy_instance(abi_, instance_);
        abi_ = new_abi;
        instance_ = new_instance;
        lib_ = std::move(new_lib);
    }

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return accepted_;
    }

    /// The declared claim-set the library published in its manifest (v6), so a
    /// loaded artifact answers "what Senses can you provide?" exactly as a native
    /// weave does — at load, before it has claimed anything.
    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return claims_;
    }

    void handle(const loom::Message& in, loom::Bus& bus) override {
        const std::string bytes = loom::serialize(in.payload);
        // `bus` is the per-delivery WeaveBus (it gates by this loaded Weave's id);
        // bus_ is the Switchboard, used only to resolve emitted schemas.
        HostCtx ctx{&bus, bus_, self_};
        ZenHostApi api{&ctx,
                       &zen_host_send,
                       &zen_host_publish,
                       &zen_host_send_to_role,
                       &zen_host_defer_answer,
                       &zen_host_answer_deferred,
                       &zen_host_release_deferred,
                       &zen_host_answer,
                       &zen_host_office_send,
                       &zen_host_office_send_to_role,
                       &zen_host_office_publish,
                       &zen_host_sense_claim,
                       &zen_host_sense_office_claim,
                       &zen_host_sense_observe,
                       &zen_host_sense_observe_office};
        // Provenance crosses as host-computed facts beside the sender, and it
        // crosses ONE WAY ONLY: the office doors above carry REQUESTS the host
        // verifies, never attested facts, so the seam is a place a loaded weave
        // learns Loom's word and never a place it can invent one.
        std::uint32_t prov = ZEN_PROV_NONE;
        switch (in.provenance.kind()) {
        case loom::Provenance::Kind::Answer:
            prov = ZEN_PROV_ANSWER;
            break;
        case loom::Provenance::Kind::Activation:
            prov = ZEN_PROV_ACTIVATION;
            break;
        case loom::Provenance::Kind::None:
            break;
        }
        // The authored office (v5): the delivery's STAMPED fact, NUL-terminated
        // for the seam; NULL means personal speech.
        const std::string authored(in.provenance.authored_role());
        // The DLL handler's status is contained: the message was validly
        // delivered; any internal library error is the library's own concern.
        abi_->handle(instance_, in.sender.value, in.reply_to.value, in.correlation, prov,
                     in.provenance.attested_sequence(),
                     authored.empty() ? nullptr : authored.c_str(),
                     reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &api);
    }

    loom::Value snapshot() const override {
        std::string bytes;
        ZenByteSink sink{&bytes, &zen_host_sink};
        const ZenStatus st = abi_->snapshot(instance_, sink);
        if (st != ZEN_OK) {
            throw DllBoundaryError("library snapshot() failed with status " + std::to_string(st));
        }
        return admit_bytes(bytes, state_schema_, "snapshot");
    }

    loom::Value policy() const override {
        std::string bytes;
        ZenByteSink sink{&bytes, &zen_host_sink};
        const ZenStatus st = abi_->policy(instance_, sink);
        if (st != ZEN_OK) {
            throw DllBoundaryError("library policy() failed with status " + std::to_string(st));
        }
        return admit_bytes(bytes, loom::lifecycle_policy_schema(), "policy");
    }

    void revive(const loom::Value& state) override {
        const std::string bytes = loom::serialize(state);
        const ZenStatus st = abi_->revive(
            instance_, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (st != ZEN_OK) {
            throw DllBoundaryError("library revive() failed with status " + std::to_string(st));
        }
    }

private:
    loom::Value admit_bytes(const std::string& bytes, const std::shared_ptr<const Schema>& door,
                           const char* what) const {
        loom::Unverified u = loom::parse(bytes);
        loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side
        if (!a.ok()) {
            throw DllBoundaryError(std::string("library ") + what + " refused by the gate: " +
                                   a.first_error().message());
        }
        return std::move(a).value();
    }

    const ZenWeaveAbi* abi_;
    void* instance_;
    /// DECLARED AFTER `instance_` ON PURPOSE, and it is load-bearing: members are
    /// destroyed in reverse declaration order and after the destructor body, so
    /// this share of the library outlives both the body's destroy() call and the
    /// instance pointer it used.
    std::shared_ptr<LoadedLibrary> lib_;
    std::vector<std::shared_ptr<const Schema>> accepted_;
    std::vector<std::shared_ptr<const Schema>> claims_; ///< the declared claim-set (v6)
    std::shared_ptr<const Schema> state_schema_;
    loom::Switchboard* bus_;
    loom::WeaveId self_{};
    /// The Kernel whose record names this adapter, and under which name. Null
    /// whenever no record does — before registration succeeds, and after a record
    /// is dropped by anything other than this destructor.
    loom::Kernel* owner_ = nullptr;
    std::string artifact_;
};

// ---- Kernel ----------------------------------------------------------------

const char* name_of(ArtifactStatus s) noexcept {
    switch (s) {
    case ArtifactStatus::NotLoaded:
        return "NotLoaded";
    case ArtifactStatus::Live:
        return "Live";
    case ArtifactStatus::Sealed:
        return "Sealed";
    case ArtifactStatus::Dead:
        return "Dead";
    case ArtifactStatus::Unregistered:
        return "Unregistered";
    }
    return "Unknown";
}

KernelLifetimeCounts kernel_lifetime_counts() noexcept { return ledger(); }

Kernel::Kernel(loom::Switchboard& bus) : bus_(bus) {}

Kernel::~Kernel() {
    // The names are copied first because unloading one artifact can release
    // ANOTHER: taking a weave off the bus can invalidate a prepared replacement,
    // which discards its candidate, whose adapter's destructor reaps that
    // artifact's record here. A later `unload` of a name already reaped simply
    // finds nothing and answers false.
    std::vector<std::string> names;
    names.reserve(libs_.size());
    for (const auto& entry : libs_) {
        names.push_back(entry.first);
    }
    for (const std::string& n : names) {
        unload(n);
    }
}

void Kernel::adapter_destroyed(const std::string& name, const HostAdapter* who) noexcept {
    auto it = libs_.find(name);
    // THE NAMESAKE CHECK. An artifact name is reusable the moment its predecessor
    // is released, so "there is a record under this name" is not the same question
    // as "that record is mine". Identity, never the name alone.
    if (it == libs_.end() || it->second.adapter != who) {
        return;
    }
    // No detach(): `who` is mid-destruction, and its owner pointer dies with it.
    libs_.erase(it);
}

void Kernel::forget(const std::string& name) noexcept {
    auto it = libs_.find(name);
    if (it == libs_.end()) {
        return;
    }
    // The adapter may still be alive — a host that took ownership through
    // `unregister_weave` and kept it. Cut its link to a record that is going, so
    // its eventual destructor cannot reach a Kernel that no longer names it.
    if (it->second.adapter != nullptr) {
        it->second.adapter->detach();
    }
    libs_.erase(it);
}

const Kernel::Loaded* Kernel::record_for(loom::WeaveId id) const {
    if (!id.valid()) {
        return nullptr;
    }
    for (const auto& entry : libs_) {
        if (entry.second.id == id) {
            return &entry.second;
        }
    }
    return nullptr;
}

Kernel::Manifest Kernel::reconstruct(const ZenWeaveAbi* abi, void* instance) {
    std::string bytes;
    ZenByteSink sink{&bytes, &zen_host_sink};
    const ZenStatus st = abi->describe(instance, sink);
    if (st != ZEN_OK) {
        throw DllBoundaryError("library describe() failed with status " + std::to_string(st));
    }
    loom::Unverified u = loom::parse(bytes);
    loom::Admission a = loom::admit(u, manifest_schema()); // the gate, for the schema descriptor
    if (!a.ok()) {
        throw DllBoundaryError("library manifest refused by the gate: " + a.first_error().message());
    }
    const loom::Value& manifest = a.value();

    Manifest result;
    // Referenced (nested component) schemas first — the manifest is
    // self-contained, so a library whose doors or state nest a shape (a
    // List<Pos>, a Pos field) brings that shape with it. Same registry, same
    // agreement wall: a component conflicting with what another library
    // already registered refuses the load.
    decode_referenced(manifest, registry_);
    for (const loom::Cell& c : manifest.get("accepted")->as_list()) {
        auto s = decode_schema(*c.as_message(), registry_);
        registry_.register_schema(s); // enforces cross-library schema agreement
        result.accepted.push_back(std::move(s));
    }
    result.state = decode_schema(*manifest.get("state")->as_message(), registry_);
    registry_.register_schema(result.state);
    // The declared claim-set (v6). Optional: a weave that claims nothing emits no
    // section, which is a declaration rather than an absence. Registered through
    // the same agreement wall, so a Sense shape two libraries disagree about
    // refuses the load exactly as a message shape does.
    if (const loom::Cell* claims = manifest.get("claims")) {
        for (const loom::Cell& c : claims->as_list()) {
            auto s = decode_schema(*c.as_message(), registry_);
            registry_.register_schema(s);
            result.claims.push_back(std::move(s));
        }
    }
    return result;
}

LoadResult Kernel::load(const std::string& name, const std::string& path,
                        const std::string& role) {
    // The kernel's default: permissive bus sends, and no Sense read authority.
    // Grant's floor is empty and R2E-0 did not move it — observing another
    // participant's claims is a decision, not a consequence of being loadable.
    return load(name, path, role, loom::Grant{}.allow_any());
}

LoadResult Kernel::load(const std::string& name, const std::string& path, const std::string& role,
                        Grant grant) {
    if (libs_.count(name) != 0) {
        return {false, {}, "already loaded: " + name};
    }
    std::string error;
    // OWNED FROM THE FIRST MOMENT IT IS OPEN. Every refusal below simply returns:
    // the handle closes when this local share goes out of scope, so there is no
    // failure path that can forget to close and none that can close twice.
    std::shared_ptr<LoadedLibrary> lib = open_library(path, error);
    if (!lib) {
        return {false, {}, "open failed: " + error};
    }
    const ZenWeaveAbi* abi = fetch_abi(lib->get(), error);
    if (abi == nullptr) {
        return {false, {}, error};
    }
    void* instance = create_instance(abi);
    if (instance == nullptr) {
        return {false, {}, "library create() returned null"};
    }

    std::unique_ptr<HostAdapter> adapter;
    bool adapter_built = false;
    try {
        Manifest mf = reconstruct(abi, instance);
        adapter = std::make_unique<HostAdapter>(abi, instance, lib, std::move(mf.accepted),
                                                std::move(mf.state), &bus_, std::move(mf.claims));
        adapter_built = true;
        HostAdapter* raw = adapter.get();
        // A loaded .so can bypass the bus and reach syscalls directly, so a
        // restrictive *bus* grant on it is not real containment in B1 — that is
        // B2's OS sandbox (which the grant's reserved OS-capability flags drive).
        // B1 grants loaded Weaves permissive bus sends; the kernel *door* (the load
        // capability) is fully gated against native Weaves.
        // A non-empty role binds the slot here, at the only moment it can be
        // bound. register_weave throws if the role is already held; that throw is
        // caught below and becomes a clean LoadResult failure — the incumbent
        // keeps its role and its life.
        loom::WeaveId id = bus_.register_weave(std::move(adapter), std::move(grant), role);
        raw->set_self(id);
        libs_.emplace(name, Loaded{name, std::move(lib), abi, raw, id});
        // THE RECORD AND THE ADAPTER ARE NOW ONE THING. From here the adapter's
        // destructor releases this record, whoever destroys it — which is the
        // whole synchronization mechanism, and it is wired last so that a failed
        // registration leaves an adapter that reaps nothing.
        raw->attach(this, name);
        return {true, id, ""};
    } catch (const std::exception& e) {
        if (!adapter_built) {
            destroy_instance(abi, instance); // instance never reached an adapter
        } else if (adapter) {
            adapter.reset(); // built but not handed to the bus; dtor destroys the instance
        }
        // else: handed to the bus and register threw -> the moved adapter's dtor
        //       already destroyed the instance. Either way it was never attached,
        //       so nothing tried to reap a record that does not exist.
        return {false, {}, std::string("load refused: ") + e.what()};
    }
}

LoadResult Kernel::load_candidate(const std::string& name, const std::string& path,
                                  loom::WeaveId coordinator) {
    // Deliberately the ordinary load, then the seal — rather than a second,
    // simpler loading path that would drift from it. The prepared artifact MUST be
    // the artifact that becomes live, so it is built by the same code that builds
    // every other weave, and the only difference is that it is born outside the
    // world.
    //
    // NOTHING CAN OBSERVE THE GAP between the two calls. `fanout` chooses its
    // recipients at ENQUEUE time, so a publication already in the queue never
    // named this weave; a publication enqueued later finds it sealed and skips it.
    // And no delivery can run in between, because this is host code, not a handler.
    LoadResult lr = load(name, path, /*role=*/std::string{});
    if (!lr.ok) {
        return lr;
    }
    if (!bus_.seal_weave(lr.id, coordinator)) {
        (void)unload(name); // could not be sealed => must not be left in the world
        return {false, {}, "candidate could not be sealed"};
    }
    return lr;
}

bool Kernel::commit_candidate(const std::string& incumbent_name,
                              const std::string& candidate_name, const std::string& role) {
    auto inc = libs_.find(incumbent_name);
    auto cand = libs_.find(candidate_name);
    if (inc == libs_.end() || cand == libs_.end()) {
        return false;
    }
    // The bus is the authority on who holds the role, and since R2B-3b-3a it is
    // the ONLY one: there is no kernel-side role to catch up afterwards, so this
    // is the whole operation. A commit through any other door — a prepared
    // replacement, or a direct `admit_candidate` this Kernel never hears about —
    // therefore leaves the Kernel's answers just as true as this one does.
    return bus_.commit_candidate(cand->second.id, inc->second.id, role);
}

ReloadResult Kernel::reload_from(const std::string& name, const std::string& new_path) {
    auto it = libs_.find(name);
    if (it == libs_.end()) {
        return {false, false, false, "not loaded: " + name};
    }
    Loaded& rec = it->second;

    std::string snapshot;
    try {
        snapshot = bus_.snapshot_bytes(rec.id); // host-owned, independent of either library
    } catch (const std::exception& e) {
        return {false, false, false, std::string("snapshot of the live weave failed: ") + e.what()};
    }

    std::string error;
    // Owned from the moment it is open, as in `load`: every refusal below returns,
    // and the handle closes when this share leaves scope.
    std::shared_ptr<LoadedLibrary> new_lib = open_library(new_path, error);
    if (!new_lib) {
        return {false, false, false, "open failed: " + error};
    }
    const ZenWeaveAbi* new_abi = fetch_abi(new_lib->get(), error);
    if (new_abi == nullptr) {
        return {false, false, false, error};
    }
    void* new_inst = create_instance(new_abi);
    if (new_inst == nullptr) {
        return {false, false, false, "library create() returned null"};
    }

    // Reconstruct the candidate's WHOLE manifest once: both halves of the
    // contract are checked from it, and neither check may commit anything.
    Manifest cand;
    try {
        cand = reconstruct(new_abi, new_inst);
    } catch (const std::exception& e) {
        destroy_instance(new_abi, new_inst);
        return {false, false, false, std::string("new library refused: ") + e.what()};
    }

    const std::shared_ptr<const Schema>& old_state = rec.adapter->state_schema();
    if (cand.state->name() != old_state->name() ||
        cand.state->version() != old_state->version() ||
        cand.state->content_id() != old_state->content_id()) {
        destroy_instance(new_abi, new_inst);
        return {true, false, true, "state schema version mismatch; reload refused"};
    }

    // The second half of the contract: the doors. Compared against what the BUS
    // published for the incumbent — the same list delivery matches against and
    // the same list commit will silently keep using — so equality here is what
    // makes the retained set true rather than merely retained. Refused before
    // rebind: the incumbent's instance, library, state, WeaveId and role are all
    // still untouched at this point.
    //
    // Scoped honestly: refused before INCUMBENT REPLACEMENT and before any
    // change to its published routing contract — not "the Loom is unchanged".
    // reconstruct() above is what produced these schemas, and it admitted them
    // into registry_ on the way; a candidate rejected here has therefore already
    // bound its (name, version) keys in this Kernel's dependency registry, which
    // a later conflicting load will meet at the agreement wall. That is named,
    // not endorsed: whether admission is intentionally monotonic or belongs to a
    // future prepared-replacement transaction is R2B's decision (see the ledger).
    if (!same_accepted_contract(cand.accepted, bus_.accepted_schemas(rec.id))) {
        destroy_instance(new_abi, new_inst);
        return {true, false, false, "accepted schema contract mismatch; reload refused"};
    }

    // Commit: swap the library behind the same adapter/WeaveId. rebind destroys
    // the old instance while the old library is still open — this local share is
    // what keeps it so, and dropping it at the end of the function is the close.
    // From here the incumbent is gone: a revive failure below leaves the weave
    // unavailable rather than rolling back (the honest edge named in the header).
    const std::shared_ptr<LoadedLibrary> old_lib = rec.lib;
    const loom::WeaveId id = rec.id;
    rec.adapter->rebind(new_abi, new_inst, new_lib);
    rec.abi = new_abi;
    rec.lib = new_lib;

    // Revive the new instance from the host-owned snapshot, through the gate.
    // This is an intentional code swap, not crash recovery, so it uses the
    // unbudgeted swap_state path: a deliberate hot-reload must never be blocked by
    // (or draw down) the Weave's crash-revival allowance.
    //
    // ...AND `rec` MAY NOT SURVIVE IT. New code is a new participant, so if some
    // prepared replacement bound this weave as its CANDIDATE, the swap ends that
    // transaction, ending it discards the candidate, and the discard runs this
    // adapter's destructor — which releases the very record this reference names.
    // Nothing below touches `rec`; the two facts still needed were copied above.
    loom::ReviveOutcome ro = bus_.swap_state(id, snapshot);

    if (libs_.count(name) == 0) {
        // The swap happened and was then undone by the discard. Saying `reloaded`
        // here would name a success whose subject no longer exists.
        return {true, false, false,
                "the reload ended a prepared replacement that had bound this weave as its "
                "candidate; the candidate was discarded and the artifact released"};
    }
    if (!ro.revived) {
        return {true, false, false, "revive after swap was refused"};
    }
    return {true, true, false, ""};
}

bool Kernel::unload(const std::string& name) {
    auto it = libs_.find(name);
    if (it == libs_.end()) {
        return false; // including an artifact a transaction already discarded
    }
    // THE NAME MUST BE OURS, NOT A REFERENCE INTO THE RECORD WE ARE DESTROYING.
    // `unload_role` legitimately passes a record's own `name` field, and the
    // adapter's destructor below erases that record — so a borrowed reference
    // dangles from that moment and `forget()` at the end would read freed memory.
    // ASan found exactly this; the Debug lane was green on it. Copy first.
    const std::string artifact = name;
    const loom::WeaveId id = it->second.id;

    // Take the weave off the bus and destroy the adapter it hands back. That
    // destructor destroys the library instance and releases this artifact's
    // record — so by the time it returns, `it` is dangling and the record is
    // usually already gone. Nothing here dereferences it again.
    std::unique_ptr<loom::Weave> adapter = bus_.unregister_weave(id);
    adapter.reset();

    // Two cases leave the record standing: the bus never had this weave (a
    // transaction discarded it earlier and something still held the name), or the
    // bus handed the adapter to somebody else who is keeping it. Dropping our
    // share is correct for both — and in the second it deliberately does NOT
    // close the library, because a live adapter still holds one.
    forget(artifact);
    return true;
}

bool Kernel::unload_role(const std::string& role) {
    if (role.empty()) {
        return false; // "no role" is not a role; never unload an unbound weave by it
    }
    // THE BUS'S ROLE TABLE IS THE AUTHORITY, so a role that moved by admission
    // selects the weave that holds it now, not whoever was loaded under it.
    const Loaded* rec = record_for(bus_.role_holder(role));
    if (rec == nullptr) {
        return false; // unheld, or held by a native weave that is not ours to unload
    }
    return unload(rec->name); // the unregister releases the role slot
}

loom::WeaveId Kernel::weave_id(const std::string& name) const {
    auto it = libs_.find(name);
    return it == libs_.end() ? loom::WeaveId{} : it->second.id;
}

std::string Kernel::role_of(const std::string& name) const {
    auto it = libs_.find(name);
    // Derived, never remembered: the Switchboard is the only thing that moves a
    // role, so it is the only thing that can answer for one.
    return it == libs_.end() ? std::string{} : bus_.role_of(it->second.id);
}

ArtifactStatus Kernel::status(const std::string& name) const {
    auto it = libs_.find(name);
    if (it == libs_.end()) {
        return ArtifactStatus::NotLoaded;
    }
    const loom::WeaveId id = it->second.id;
    if (bus_.weave(id) == nullptr) {
        return ArtifactStatus::Unregistered;
    }
    if (!bus_.alive(id)) {
        return ArtifactStatus::Dead; // aliveness is the coarser fact; it outranks the seal
    }
    return bus_.sealed(id) ? ArtifactStatus::Sealed : ArtifactStatus::Live;
}

Kernel::RoleQuery Kernel::query_role(const std::string& role, const std::string& shape_name,
                                     std::uint32_t shape_version) const {
    RoleQuery out;
    if (role.empty()) {
        return out; // "no role" is not a role
    }
    // Who holds it is the BUS's answer; whether that holder is one of ours is the
    // only part the Kernel decides, and it is what the documented `holder == 0`
    // means. A native holder answers the same as an unheld role, deliberately.
    const Loaded* rec = record_for(bus_.role_holder(role));
    if (rec == nullptr) {
        return out;
    }
    out.holder = rec->id;
    out.accepts = accepts(rec->id, shape_name, shape_version);
    return out;
}

bool Kernel::accepts(loom::WeaveId id, const std::string& shape_name,
                     std::uint32_t shape_version) const {
    // The accept-set the bus published for this weave at registration — the same
    // list the kernel reconstructed from the library's own manifest, and the same
    // list delivery is matched against. Asking the bus rather than caching it
    // here keeps one truth; an id the bus does not know answers with an empty
    // set, which is the clean false this must give.
    return declares(bus_.accepted_schemas(id), shape_name, shape_version);
}

bool Kernel::is_loaded(const std::string& name) const { return libs_.count(name) != 0; }

std::vector<std::string> Kernel::loaded() const {
    std::vector<std::string> names;
    names.reserve(libs_.size());
    for (const auto& entry : libs_) {
        names.push_back(entry.first);
    }
    return names;
}

} // namespace loom
