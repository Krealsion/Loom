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

namespace zen::kernel {

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
    // RTLD_LOCAL keeps the library's symbols (including its own copy of zen-core)
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
const ZenShardAbi* fetch_abi(void* lib, std::string& error) {
    void* sym = lib_symbol(lib, "zen_shard_abi");
    if (sym == nullptr) {
        error = "library does not export zen_shard_abi";
        return nullptr;
    }
    using AbiFn = const ZenShardAbi* (*)(void);
    AbiFn fn = nullptr;
    std::memcpy(&fn, &sym, sizeof(fn)); // object->function pointer, the -Wpedantic-clean way
    const ZenShardAbi* abi = fn();
    if (abi == nullptr) {
        error = "zen_shard_abi returned null";
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
// and routed exactly as a native Shard's are. `gated` is the per-delivery
// ShardBus (it stamps the loaded Shard's id and authorizes against its grant);
// `sb` is the Switchboard, used only for the read-only schema resolution. Valid
// only for the duration of the handle() call.
struct HostCtx {
    zen::sb::Bus* gated;
    zen::sb::Switchboard* sb;
};

} // namespace

extern "C" {

// Append library bytes into a host std::string. The host copies immediately; no
// library pointer is retained.
static void zen_host_sink(void* ctx, const std::uint8_t* data, std::size_t len) {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
}

// A library Shard sends/publishes by handing the host serialized payload bytes.
// The host admits them through the gate (the DLL-seam boundary) before routing.
static ZenStatus zen_host_send(void* ctx, std::uint64_t target, std::uint64_t reply_to,
                               std::uint64_t correlation, const std::uint8_t* payload,
                               std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    zen::Unverified u = zen::parse(zen::kernel::as_view(payload, len));
    std::shared_ptr<const zen::Schema> door = h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    zen::Admission a = zen::admit(u, door); // the DLL-seam gate, host-side
    if (!a.ok()) {
        return ZEN_ERR_REFUSED;
    }
    // Route through the gated ShardBus (it stamps the loaded Shard's id and
    // authorizes against its grant), exactly as a native Shard's send is.
    h->gated->send(zen::sb::ShardId{target},
                   zen::sb::Message(std::move(a).value(), zen::sb::ShardId{},
                                    zen::sb::ShardId{reply_to}, correlation));
    return ZEN_OK;
}

static ZenStatus zen_host_publish(void* ctx, std::uint64_t reply_to, std::uint64_t correlation,
                                  const std::uint8_t* payload, std::size_t len) {
    auto* h = static_cast<HostCtx*>(ctx);
    zen::Unverified u = zen::parse(zen::kernel::as_view(payload, len));
    std::shared_ptr<const zen::Schema> door = h->sb->resolve_schema(u.claimed_name(), u.claimed_version());
    if (!door) {
        return ZEN_ERR_UNKNOWN_SCHEMA;
    }
    zen::Admission a = zen::admit(u, door);
    if (!a.ok()) {
        return ZEN_ERR_REFUSED;
    }
    h->gated->publish(zen::sb::Message(std::move(a).value(), zen::sb::ShardId{},
                                       zen::sb::ShardId{reply_to}, correlation));
    return ZEN_OK;
}

} // extern "C"

// ---- the host adapter: a Shard backed by a library instance ---------------

class HostAdapter final : public zen::sb::Shard {
public:
    HostAdapter(const ZenShardAbi* abi, void* instance,
                std::vector<std::shared_ptr<const Schema>> accepted,
                std::shared_ptr<const Schema> state_schema, zen::sb::Switchboard* bus)
        : abi_(abi), instance_(instance), accepted_(std::move(accepted)),
          state_schema_(std::move(state_schema)), bus_(bus) {}

    ~HostAdapter() override {
        if (abi_ != nullptr && instance_ != nullptr) {
            abi_->destroy(instance_);
        }
    }

    void set_self(zen::sb::ShardId id) { self_ = id; }
    const std::shared_ptr<const Schema>& state_schema() const { return state_schema_; }

    // Swap the backing library in place (hot-reload), destroying the old
    // instance while the old library is still open.
    void rebind(const ZenShardAbi* new_abi, void* new_instance) {
        if (abi_ != nullptr && instance_ != nullptr) {
            abi_->destroy(instance_);
        }
        abi_ = new_abi;
        instance_ = new_instance;
    }

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return accepted_;
    }

    void handle(const zen::sb::Message& in, zen::sb::Bus& bus) override {
        const std::string bytes = zen::serialize(in.payload);
        // `bus` is the per-delivery ShardBus (it gates by this loaded Shard's id);
        // bus_ is the Switchboard, used only to resolve emitted schemas.
        HostCtx ctx{&bus, bus_};
        ZenHostApi api{&ctx, &zen_host_send, &zen_host_publish};
        // The DLL handler's status is contained: the message was validly
        // delivered; any internal library error is the library's own concern.
        abi_->handle(instance_, in.sender.value, in.reply_to.value, in.correlation,
                     reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &api);
    }

    zen::Value snapshot() const override {
        std::string bytes;
        ZenByteSink sink{&bytes, &zen_host_sink};
        const ZenStatus st = abi_->snapshot(instance_, sink);
        if (st != ZEN_OK) {
            throw DllBoundaryError("library snapshot() failed with status " + std::to_string(st));
        }
        return admit_bytes(bytes, state_schema_, "snapshot");
    }

    zen::Value policy() const override {
        std::string bytes;
        ZenByteSink sink{&bytes, &zen_host_sink};
        const ZenStatus st = abi_->policy(instance_, sink);
        if (st != ZEN_OK) {
            throw DllBoundaryError("library policy() failed with status " + std::to_string(st));
        }
        return admit_bytes(bytes, zen::sb::lifecycle_policy_schema(), "policy");
    }

    void revive(const zen::Value& state) override {
        const std::string bytes = zen::serialize(state);
        const ZenStatus st = abi_->revive(
            instance_, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (st != ZEN_OK) {
            throw DllBoundaryError("library revive() failed with status " + std::to_string(st));
        }
    }

private:
    zen::Value admit_bytes(const std::string& bytes, const std::shared_ptr<const Schema>& door,
                           const char* what) const {
        zen::Unverified u = zen::parse(bytes);
        zen::Admission a = zen::admit(u, door); // the DLL-seam gate, host-side
        if (!a.ok()) {
            throw DllBoundaryError(std::string("library ") + what + " refused by the gate: " +
                                   a.first_error().message());
        }
        return std::move(a).value();
    }

    const ZenShardAbi* abi_;
    void* instance_;
    std::vector<std::shared_ptr<const Schema>> accepted_;
    std::shared_ptr<const Schema> state_schema_;
    zen::sb::Switchboard* bus_;
    zen::sb::ShardId self_{};
};

// ---- Kernel ----------------------------------------------------------------

Kernel::Kernel(zen::sb::Switchboard& bus) : bus_(bus) {}

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

Kernel::Manifest Kernel::reconstruct(const ZenShardAbi* abi, void* instance) {
    std::string bytes;
    ZenByteSink sink{&bytes, &zen_host_sink};
    const ZenStatus st = abi->describe(instance, sink);
    if (st != ZEN_OK) {
        throw DllBoundaryError("library describe() failed with status " + std::to_string(st));
    }
    zen::Unverified u = zen::parse(bytes);
    zen::Admission a = zen::admit(u, manifest_schema()); // the gate, for the schema descriptor
    if (!a.ok()) {
        throw DllBoundaryError("library manifest refused by the gate: " + a.first_error().message());
    }
    const zen::Value& manifest = a.value();

    Manifest result;
    for (const zen::Cell& c : manifest.get("accepted")->as_list()) {
        auto s = decode_schema(*c.as_message(), registry_);
        registry_.register_schema(s); // enforces cross-library schema agreement
        result.accepted.push_back(std::move(s));
    }
    result.state = decode_schema(*manifest.get("state")->as_message(), registry_);
    registry_.register_schema(result.state);
    return result;
}

LoadResult Kernel::load(const std::string& name, const std::string& path) {
    if (libs_.count(name) != 0) {
        return {false, {}, "already loaded: " + name};
    }
    std::string error;
    void* lib = lib_open(path, error);
    if (lib == nullptr) {
        return {false, {}, "open failed: " + error};
    }
    const ZenShardAbi* abi = fetch_abi(lib, error);
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
        // B1 grants loaded Shards permissive bus sends; the kernel *door* (the load
        // capability) is fully gated against native Shards.
        zen::sb::ShardId id = bus_.register_shard(std::move(adapter), zen::sb::Grant{}.allow_any());
        raw->set_self(id);
        libs_.emplace(name, Loaded{name, lib, abi, raw, id});
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
        return {false, false, false, std::string("snapshot of the live shard failed: ") + e.what()};
    }

    std::string error;
    void* new_lib = lib_open(new_path, error);
    if (new_lib == nullptr) {
        return {false, false, false, "open failed: " + error};
    }
    const ZenShardAbi* new_abi = fetch_abi(new_lib, error);
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

    // Commit: swap the library behind the same adapter/ShardId. rebind destroys
    // the old instance while the old library is still open.
    void* old_lib = rec.lib;
    rec.adapter->rebind(new_abi, new_inst);
    rec.abi = new_abi;
    rec.lib = new_lib;

    // Revive the new instance from the host-owned snapshot, through the gate.
    // This is an intentional code swap, not crash recovery, so it uses the
    // unbudgeted swap_state path: a deliberate hot-reload must never be blocked by
    // (or draw down) the Shard's crash-revival allowance.
    zen::sb::ReviveOutcome ro = bus_.swap_state(rec.id, snapshot);

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
    std::unique_ptr<zen::sb::Shard> adapter = bus_.unregister_shard(rec.id);
    adapter.reset();
    lib_close(rec.lib);
    return true;
}

zen::sb::ShardId Kernel::shard_id(const std::string& name) const {
    auto it = libs_.find(name);
    return it == libs_.end() ? zen::sb::ShardId{} : it->second.id;
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

} // namespace zen::kernel
