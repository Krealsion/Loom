#include <zen/kernel/kernel.hpp>

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace loom {

namespace {

// ---- platform loader ------------------------------------------------------

void* lib_open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    void* h = static_cast<void*>(::LoadLibraryA(path.c_str()));
    if (h == nullptr) {
        error = "LoadLibrary failed";
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
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

std::string_view as_view(const std::uint8_t* data, std::size_t len) {
    return std::string_view(reinterpret_cast<const char*>(data), len);
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
};

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
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    loom::Admission a = loom::admit(u, door); // the DLL-seam gate, host-side
    if (!a.ok()) {
        return ZEN_ERR_REFUSED;
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
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return ZEN_ERR_REFUSED;
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
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    loom::Admission a = loom::admit(u, door);
    if (!a.ok()) {
        return ZEN_ERR_REFUSED;
    }
    h->gated->send_to_role(role, loom::Message(std::move(a).value(), loom::WeaveId{},
                                                  loom::WeaveId{reply_to}, correlation));
    return ZEN_OK;
}

} // extern "C"

// ---- the host adapter: a Weave backed by a library instance ---------------

class HostAdapter final : public loom::Weave {
public:
    HostAdapter(const ZenWeaveAbi* abi, void* instance,
                std::vector<std::shared_ptr<const Schema>> accepted,
                std::shared_ptr<const Schema> state_schema, loom::Switchboard* bus)
        : abi_(abi), instance_(instance), accepted_(std::move(accepted)),
          state_schema_(std::move(state_schema)), bus_(bus) {}

    ~HostAdapter() override {
        if (abi_ != nullptr && instance_ != nullptr) {
            abi_->destroy(instance_);
        }
    }

    void set_self(loom::WeaveId id) { self_ = id; }
    const std::shared_ptr<const Schema>& state_schema() const { return state_schema_; }

    // Swap the backing library in place (hot-reload), destroying the old
    // instance while the old library is still open.
    void rebind(const ZenWeaveAbi* new_abi, void* new_instance) {
        if (abi_ != nullptr && instance_ != nullptr) {
            abi_->destroy(instance_);
        }
        abi_ = new_abi;
        instance_ = new_instance;
    }

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return accepted_;
    }

    void handle(const loom::Message& in, loom::Bus& bus) override {
        const std::string bytes = loom::serialize(in.payload);
        // `bus` is the per-delivery WeaveBus (it gates by this loaded Weave's id);
        // bus_ is the Switchboard, used only to resolve emitted schemas.
        HostCtx ctx{&bus, bus_};
        ZenHostApi api{&ctx, &zen_host_send, &zen_host_publish, &zen_host_send_to_role};
        // The DLL handler's status is contained: the message was validly
        // delivered; any internal library error is the library's own concern.
        abi_->handle(instance_, in.sender.value, in.reply_to.value, in.correlation,
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
    std::vector<std::shared_ptr<const Schema>> accepted_;
    std::shared_ptr<const Schema> state_schema_;
    loom::Switchboard* bus_;
    loom::WeaveId self_{};
};

// ---- Kernel ----------------------------------------------------------------

Kernel::Kernel(loom::Switchboard& bus) : bus_(bus) {}

Kernel::~Kernel() {
    std::vector<std::string> names;
    names.reserve(libs_.size());
    for (const auto& entry : libs_) {
        names.push_back(entry.first);
    }
    for (const std::string& n : names) {
        unload(n);
    }
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
    for (const loom::Cell& c : manifest.get("accepted")->as_list()) {
        auto s = decode_schema(*c.as_message(), registry_);
        registry_.register_schema(s); // enforces cross-library schema agreement
        result.accepted.push_back(std::move(s));
    }
    result.state = decode_schema(*manifest.get("state")->as_message(), registry_);
    registry_.register_schema(result.state);
    return result;
}

LoadResult Kernel::load(const std::string& name, const std::string& path,
                        const std::string& role) {
    if (libs_.count(name) != 0) {
        return {false, {}, "already loaded: " + name};
    }
    std::string error;
    void* lib = lib_open(path, error);
    if (lib == nullptr) {
        return {false, {}, "open failed: " + error};
    }
    const ZenWeaveAbi* abi = fetch_abi(lib, error);
    if (abi == nullptr) {
        lib_close(lib);
        return {false, {}, error};
    }
    void* instance = abi->create();
    if (instance == nullptr) {
        lib_close(lib);
        return {false, {}, "library create() returned null"};
    }

    std::unique_ptr<HostAdapter> adapter;
    bool adapter_built = false;
    try {
        Manifest mf = reconstruct(abi, instance);
        adapter = std::make_unique<HostAdapter>(abi, instance, std::move(mf.accepted),
                                                std::move(mf.state), &bus_);
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
        loom::WeaveId id =
            bus_.register_weave(std::move(adapter), loom::Grant{}.allow_any(), role);
        raw->set_self(id);
        libs_.emplace(name, Loaded{name, lib, abi, raw, id, role});
        return {true, id, ""};
    } catch (const std::exception& e) {
        if (!adapter_built) {
            abi->destroy(instance); // instance never reached an adapter
        } else if (adapter) {
            adapter.reset(); // built but not handed to the bus; dtor destroys the instance
        }
        // else: handed to the bus and register threw -> the moved adapter's dtor
        //       already destroyed the instance.
        lib_close(lib);
        return {false, {}, std::string("load refused: ") + e.what()};
    }
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
    void* new_lib = lib_open(new_path, error);
    if (new_lib == nullptr) {
        return {false, false, false, "open failed: " + error};
    }
    const ZenWeaveAbi* new_abi = fetch_abi(new_lib, error);
    if (new_abi == nullptr) {
        lib_close(new_lib);
        return {false, false, false, error};
    }
    void* new_inst = new_abi->create();
    if (new_inst == nullptr) {
        lib_close(new_lib);
        return {false, false, false, "library create() returned null"};
    }

    std::shared_ptr<const Schema> new_state;
    try {
        new_state = reconstruct(new_abi, new_inst).state;
    } catch (const std::exception& e) {
        new_abi->destroy(new_inst);
        lib_close(new_lib);
        return {false, false, false, std::string("new library refused: ") + e.what()};
    }

    const std::shared_ptr<const Schema>& old_state = rec.adapter->state_schema();
    if (new_state->name() != old_state->name() || new_state->version() != old_state->version() ||
        new_state->content_id() != old_state->content_id()) {
        new_abi->destroy(new_inst);
        lib_close(new_lib);
        return {true, false, true, "state schema version mismatch; reload refused"};
    }

    // Commit: swap the library behind the same adapter/WeaveId. rebind destroys
    // the old instance while the old library is still open.
    void* old_lib = rec.lib;
    rec.adapter->rebind(new_abi, new_inst);
    rec.abi = new_abi;
    rec.lib = new_lib;

    // Revive the new instance from the host-owned snapshot, through the gate.
    // This is an intentional code swap, not crash recovery, so it uses the
    // unbudgeted swap_state path: a deliberate hot-reload must never be blocked by
    // (or draw down) the Weave's crash-revival allowance.
    loom::ReviveOutcome ro = bus_.swap_state(rec.id, snapshot);

    lib_close(old_lib); // the old instance is already gone; no live pointer remains

    if (!ro.revived) {
        return {true, false, false, "revive after swap was refused"};
    }
    return {true, true, false, ""};
}

bool Kernel::unload(const std::string& name) {
    auto it = libs_.find(name);
    if (it == libs_.end()) {
        return false;
    }
    const Loaded rec = it->second;
    libs_.erase(it);

    // Destroy the adapter (and, in its dtor, the library instance) BEFORE closing
    // the library — so no call ever lands in a closed library.
    std::unique_ptr<loom::Weave> adapter = bus_.unregister_weave(rec.id);
    adapter.reset();
    lib_close(rec.lib);
    return true;
}

bool Kernel::unload_role(const std::string& role) {
    if (role.empty()) {
        return false; // "no role" is not a role; never unload an unbound weave by it
    }
    for (const auto& entry : libs_) {
        if (entry.second.role == role) {
            return unload(entry.first); // the unregister releases the role slot
        }
    }
    return false;
}

loom::WeaveId Kernel::weave_id(const std::string& name) const {
    auto it = libs_.find(name);
    return it == libs_.end() ? loom::WeaveId{} : it->second.id;
}

std::string Kernel::role_of(const std::string& name) const {
    auto it = libs_.find(name);
    return it == libs_.end() ? std::string{} : it->second.role;
}

Kernel::RoleQuery Kernel::query_role(const std::string& role, const std::string& shape_name,
                                     std::uint32_t shape_version) const {
    RoleQuery out;
    if (role.empty()) {
        return out; // "no role" is not a role
    }
    for (const auto& entry : libs_) {
        if (entry.second.role != role) {
            continue;
        }
        out.holder = entry.second.id;
        // The accept-set the bus published for this weave at registration — the
        // same list the kernel reconstructed from the library's own manifest, and
        // the same list delivery is matched against. Asking the bus rather than
        // caching it here keeps one truth.
        for (const auto& s : bus_.accepted_schemas(entry.second.id)) {
            if (s && s->name() == shape_name && s->version() == shape_version) {
                out.accepts = true;
                break;
            }
        }
        return out;
    }
    return out;
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
