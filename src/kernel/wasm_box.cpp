// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The kernel's WebAssembly backend: wasm_box.hpp says what it is. One Wasmtime engine and one
// linker serve the whole process; a module is compiled once per content and shared; each
// instance of a weave owns one store, and is driven through `box_abi()`, whose slots forward into
// the module's `zw_*` exports. The module reaches the host through its `zen_host.*` imports,
// which call the host table of the delivery in progress; outside a delivery every door refuses.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include "wasm_box.hpp"

#include "detail/sha256.hpp"

#include <zen/serialize.hpp>
#include <zen/switchboard/switchboard.hpp> // lifecycle_policy_schema

#include <wasmtime.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace loom::detail {

namespace {

// ---- Wasmtime's words ---------------------------------------------------------------------------

std::string error_text(wasmtime_error_t* e) {
    wasm_byte_vec_t m;
    wasmtime_error_message(e, &m);
    std::string s(m.data, m.size);
    wasm_byte_vec_delete(&m);
    wasmtime_error_delete(e);
    return s;
}

std::string trap_text(wasm_trap_t* t) {
    wasm_byte_vec_t m;
    wasm_trap_message(t, &m);
    std::string s(m.data, m.size);
    wasm_byte_vec_delete(&m);
    wasm_trap_delete(t);
    return s;
}

wasm_trap_t* trap(const std::string& words) { return wasmtime_trap_new(words.data(), words.size()); }

wasmtime_val_t i32v(std::int32_t v) {
    wasmtime_val_t x;
    x.kind = WASMTIME_I32;
    x.of.i32 = v;
    return x;
}

wasmtime_val_t i64v(std::int64_t v) {
    wasmtime_val_t x;
    x.kind = WASMTIME_I64;
    x.of.i64 = v;
    return x;
}

std::int32_t as_i32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

// ---- the counts ---------------------------------------------------------------------------------

std::mutex& counts_lock() {
    static std::mutex m;
    return m;
}

BoxCounts& counts() {
    static BoxCounts c;
    return c;
}

void count(std::uint64_t BoxCounts::*field) {
    const std::lock_guard<std::mutex> hold(counts_lock());
    ++(counts().*field);
}

// ---- the engine and the linker, once per process ------------------------------------------------

void define_imports(wasmtime_linker_t* linker, std::string& error);

struct Engine {
    wasm_engine_t* engine = nullptr;
    wasmtime_linker_t* linker = nullptr;
    std::string error;
};

/// Made the first time a module is opened, and never torn down: a store or module still held when
/// the process ends would otherwise outlive the engine that made it.
Engine& engine() {
    static Engine* e = [] {
        auto* made = new Engine;
        wasm_config_t* cfg = wasm_config_new();
        wasmtime_config_consume_fuel_set(cfg, true);
        wasmtime_config_wasm_exceptions_set(cfg, true);
        wasmtime_config_max_wasm_stack_set(cfg, std::size_t{512} * 1024);
        // Explicit checks rather than signal handlers: the host's process keeps its own handlers
        // (a crash reporter, a test runner, a sanitizer), and nothing here installs one.
        wasmtime_config_signals_based_traps_set(cfg, false);
        made->engine = wasm_engine_new_with_config(cfg);
        made->linker = wasmtime_linker_new(made->engine);
        define_imports(made->linker, made->error);
        return made;
    }();
    return *e;
}

} // namespace

// ---- a compiled module --------------------------------------------------------------------------

class BoxModule {
public:
    explicit BoxModule(wasmtime_module_t* module) noexcept : module_(module) {}
    ~BoxModule() { wasmtime_module_delete(module_); }
    BoxModule(const BoxModule&) = delete;
    BoxModule& operator=(const BoxModule&) = delete;

    wasmtime_module_t* get() const noexcept { return module_; }

private:
    wasmtime_module_t* module_;
};

namespace {

/// The modules compiled most recently, by the SHA-256 of their bytes. An image holds its module
/// for as long as it is open; this list holds the last `kBoxModulesKept` beyond that.
struct Kept {
    std::string digest;
    std::shared_ptr<BoxModule> module;
};

std::mutex& kept_lock() {
    static std::mutex m;
    return m;
}

std::deque<Kept>& kept() {
    static auto* k = new std::deque<Kept>;
    return *k;
}

std::shared_ptr<BoxModule> find_kept(const std::string& digest) {
    const std::lock_guard<std::mutex> hold(kept_lock());
    auto& k = kept();
    for (auto it = k.begin(); it != k.end(); ++it) {
        if (it->digest == digest) {
            Kept found = std::move(*it);
            k.erase(it);
            k.push_front(found);
            return found.module;
        }
    }
    return nullptr;
}

void keep(const std::string& digest, std::shared_ptr<BoxModule> module) {
    const std::lock_guard<std::mutex> hold(kept_lock());
    auto& k = kept();
    k.push_front(Kept{digest, std::move(module)});
    while (k.size() > kBoxModulesKept) {
        k.pop_back();
    }
}

// ---- one instance -------------------------------------------------------------------------------

/// The exports a module must carry, beside its memory: the weave's descriptor, flattened by the
/// module glue (cmake/loom-wasm-glue.cpp).
struct Exports {
    wasmtime_func_t initialize{}, abi_version{}, alloc{}, free{}, create{}, destroy{}, describe{},
        snapshot{}, policy{}, revive{}, handle{}, claim_published{}, sink_deliver{};
};

struct Box {
    std::shared_ptr<BoxModule> module;
    wasmtime_store_t* store = nullptr;
    wasmtime_context_t* cx = nullptr;
    wasmtime_instance_t instance{};
    wasmtime_memory_t memory{};
    Exports fn;
    /// The weave instance, an address in the module's memory.
    std::int32_t weave = 0;
    /// The doors of the delivery in progress, and null outside one.
    const ZenHostApi* host = nullptr;
    /// The host sink `zen_host.sink_write` hands bytes to while a slot writes.
    ZenByteSink sink{};
    /// The linear memory the store may grow to; negative is wasm32's own ceiling.
    std::int64_t memory_limit = kBoxDefaultMemoryBytes;
    /// The weave's state after its last completed entry, held by the host for a revival.
    std::string last_good;
    bool have_good = false;
    /// The weave's own reload budget (`LifecyclePolicy::max_reloads`), and what it has spent.
    std::int64_t max_reloads = 0;
    std::int64_t reloads = 0;
    bool quarantined = false;
    std::string last_trap;
    /// What the module wrote to its standard output and error in this entry, and what was dropped.
    std::string log;
    std::size_t log_dropped = 0;
};

Box* box_of(wasmtime_caller_t* caller) {
    return static_cast<Box*>(wasmtime_context_get_data(wasmtime_caller_context(caller)));
}

/// Call an export; a trap or an error is false, with its words in `why`.
bool call(wasmtime_context_t* cx, const wasmtime_func_t& f, const wasmtime_val_t* args,
          std::size_t nargs, wasmtime_val_t* results, std::size_t nresults, std::string& why) {
    wasm_trap_t* t = nullptr;
    if (wasmtime_error_t* err = wasmtime_func_call(cx, &f, args, nargs, results, nresults, &t)) {
        why = error_text(err);
        return false;
    }
    if (t != nullptr) {
        why = trap_text(t);
        return false;
    }
    return true;
}

/// A span of the module's memory, checked against its current size before a byte is touched.
bool span(wasmtime_context_t* cx, const wasmtime_memory_t& mem, std::uint32_t offset,
          std::uint32_t length, std::uint8_t** out) {
    const std::size_t size = wasmtime_memory_data_size(cx, &mem);
    if (static_cast<std::uint64_t>(offset) + length > size) {
        return false;
    }
    *out = wasmtime_memory_data(cx, &mem) + offset;
    return true;
}

/// A NUL-terminated name in the module's memory, at most `kBoxMaxNameBytes` long.
bool name_at(wasmtime_context_t* cx, const wasmtime_memory_t& mem, std::uint32_t offset,
             std::string& out) {
    const std::size_t size = wasmtime_memory_data_size(cx, &mem);
    const std::uint8_t* base = wasmtime_memory_data(cx, &mem);
    for (std::size_t i = offset; i < size && i - offset <= kBoxMaxNameBytes; ++i) {
        if (base[i] == 0) {
            out.assign(reinterpret_cast<const char*>(base + offset), i - offset);
            return true;
        }
    }
    return false;
}

void refuel(Box& b) { (void)wasmtime_context_set_fuel(b.cx, kBoxStepsPerEntry); }

/// Host bytes copied into fresh module memory from the module's own allocator; 0 on failure.
std::int32_t copy_in(Box& b, const std::uint8_t* data, std::size_t len, std::string& why) {
    if (len > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        why = "the box cannot hold " + std::to_string(len) + " bytes";
        return 0;
    }
    wasmtime_val_t a = i32v(static_cast<std::int32_t>(len));
    wasmtime_val_t r;
    if (!call(b.cx, b.fn.alloc, &a, 1, &r, 1, why)) {
        return 0;
    }
    std::uint8_t* p = nullptr;
    if (r.of.i32 == 0 || !span(b.cx, b.memory, static_cast<std::uint32_t>(r.of.i32),
                               static_cast<std::uint32_t>(len), &p)) {
        why = "the module could not hold " + std::to_string(len) + " bytes";
        return 0;
    }
    if (len > 0) {
        std::memcpy(p, data, len);
    }
    return r.of.i32;
}

void free_in(Box& b, std::int32_t p) {
    std::string why;
    wasmtime_val_t a = i32v(p);
    (void)call(b.cx, b.fn.free, &a, 1, nullptr, 0, why);
}

// ---- the imports: the host's doors --------------------------------------------------------------

#define BOX_CB(name)                                                                               \
    wasm_trap_t* name(void*, wasmtime_caller_t* caller, const wasmtime_val_t* args,               \
                      std::size_t nargs, wasmtime_val_t* results, std::size_t nresults)
#define BOX_UNUSED (void)args, (void)nargs, (void)results, (void)nresults

std::uint32_t u32(const wasmtime_val_t* args, std::size_t i) {
    return static_cast<std::uint32_t>(args[i].of.i32);
}
std::uint64_t u64(const wasmtime_val_t* args, std::size_t i) {
    return static_cast<std::uint64_t>(args[i].of.i64);
}

wasm_trap_t* answer(wasmtime_val_t* results, ZenStatus status) {
    results[0] = i32v(status);
    return nullptr;
}

/// One door call's view of the module: the box, its context, and the checks a door makes on
/// what the module names. Every failed check is a trap, never a read past the memory.
struct Door {
    Box& b;
    wasmtime_context_t* cx;
    explicit Door(wasmtime_caller_t* caller)
        : b(*box_of(caller)), cx(wasmtime_caller_context(caller)) {}

    /// A door exists only while a delivery is in progress, and only if the host table has it.
    template <class Slot>
    bool open(Slot ZenHostApi::*slot) const {
        return b.host != nullptr && b.host->*slot != nullptr;
    }
    bool bytes(std::uint32_t ptr, std::uint32_t len, const std::uint8_t** out) const {
        std::uint8_t* p = nullptr;
        if (!span(cx, b.memory, ptr, len, &p)) {
            return false;
        }
        *out = p;
        return true;
    }
    /// An out-parameter: 0 is no out-parameter, anything else must hold eight bytes.
    bool out64(std::uint32_t ptr, std::uint8_t** out) const {
        *out = nullptr;
        return ptr == 0 || span(cx, b.memory, ptr, 8, out);
    }
};

void write64(std::uint8_t* at, std::uint64_t v) {
    if (at != nullptr) {
        std::memcpy(at, &v, 8);
    }
}

wasm_trap_t* outside_bytes() { return trap("the module named bytes outside its memory"); }
wasm_trap_t* outside_name() {
    return trap("the module named a name outside its memory, or longer than the box takes");
}
wasm_trap_t* outside_out() { return trap("the module named an out-parameter outside its memory"); }

BOX_CB(zh_sink_write) {
    BOX_UNUSED;
    Door d(caller);
    const std::uint8_t* p = nullptr;
    if (!d.bytes(u32(args, 1), u32(args, 2), &p)) {
        return outside_bytes();
    }
    if (u32(args, 0) == 0 && d.b.sink.write != nullptr) {
        d.b.sink.write(d.b.sink.ctx, p, u32(args, 2)); // the host's sink copies at once
    }
    return nullptr;
}

BOX_CB(zh_send) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::send)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    std::uint8_t* attempt_at = nullptr;
    if (!d.bytes(u32(args, 3), u32(args, 4), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 5), &attempt_at)) {
        return outside_out();
    }
    std::uint64_t attempt = 0;
    const ZenStatus st = d.b.host->send(d.b.host->ctx, u64(args, 0), u64(args, 1), u64(args, 2), p,
                                        u32(args, 4), attempt_at != nullptr ? &attempt : nullptr);
    write64(attempt_at, attempt);
    return answer(results, st);
}

BOX_CB(zh_publish) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::publish)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    if (!d.bytes(u32(args, 2), u32(args, 3), &p)) {
        return outside_bytes();
    }
    return answer(results,
                  d.b.host->publish(d.b.host->ctx, u64(args, 0), u64(args, 1), p, u32(args, 3)));
}

BOX_CB(zh_send_to_role) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::send_to_role)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string role;
    const std::uint8_t* p = nullptr;
    std::uint8_t* attempt_at = nullptr;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), role)) {
        return outside_name();
    }
    if (!d.bytes(u32(args, 3), u32(args, 4), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 5), &attempt_at)) {
        return outside_out();
    }
    std::uint64_t attempt = 0;
    const ZenStatus st =
        d.b.host->send_to_role(d.b.host->ctx, role.c_str(), u64(args, 1), u64(args, 2), p,
                               u32(args, 4), attempt_at != nullptr ? &attempt : nullptr);
    write64(attempt_at, attempt);
    return answer(results, st);
}

BOX_CB(zh_defer_answer) {
    BOX_UNUSED;
    Door d(caller);
    std::uint64_t token = 0;
    if (d.open(&ZenHostApi::defer_answer)) {
        token = d.b.host->defer_answer(d.b.host->ctx);
    }
    results[0] = i64v(static_cast<std::int64_t>(token));
    return nullptr;
}

BOX_CB(zh_answer_deferred) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::answer_deferred)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    if (!d.bytes(u32(args, 1), u32(args, 2), &p)) {
        return outside_bytes();
    }
    return answer(results,
                  d.b.host->answer_deferred(d.b.host->ctx, u64(args, 0), p, u32(args, 2)));
}

BOX_CB(zh_release_deferred) {
    BOX_UNUSED;
    Door d(caller);
    if (d.open(&ZenHostApi::release_deferred)) {
        d.b.host->release_deferred(d.b.host->ctx, u64(args, 0));
    }
    return nullptr;
}

BOX_CB(zh_answer) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::answer)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    if (!d.bytes(u32(args, 0), u32(args, 1), &p)) {
        return outside_bytes();
    }
    return answer(results, d.b.host->answer(d.b.host->ctx, p, u32(args, 1)));
}

BOX_CB(zh_office_send) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::office_send)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string as;
    const std::uint8_t* p = nullptr;
    std::uint8_t* attempt_at = nullptr;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), as)) {
        return outside_name();
    }
    if (!d.bytes(u32(args, 4), u32(args, 5), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 6), &attempt_at)) {
        return outside_out();
    }
    std::uint64_t attempt = 0;
    const ZenStatus st = d.b.host->office_send(d.b.host->ctx, as.c_str(), u64(args, 1),
                                               u64(args, 2), u64(args, 3), p, u32(args, 5),
                                               attempt_at != nullptr ? &attempt : nullptr);
    write64(attempt_at, attempt);
    return answer(results, st);
}

BOX_CB(zh_office_send_to_role) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::office_send_to_role)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string as;
    std::string to;
    const std::uint8_t* p = nullptr;
    std::uint8_t* attempt_at = nullptr;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), as) ||
        !name_at(d.cx, d.b.memory, u32(args, 1), to)) {
        return outside_name();
    }
    if (!d.bytes(u32(args, 4), u32(args, 5), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 6), &attempt_at)) {
        return outside_out();
    }
    std::uint64_t attempt = 0;
    const ZenStatus st = d.b.host->office_send_to_role(
        d.b.host->ctx, as.c_str(), to.c_str(), u64(args, 2), u64(args, 3), p, u32(args, 5),
        attempt_at != nullptr ? &attempt : nullptr);
    write64(attempt_at, attempt);
    return answer(results, st);
}

BOX_CB(zh_office_publish) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::office_publish)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string as;
    const std::uint8_t* p = nullptr;
    std::uint8_t* recipients_at = nullptr;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), as)) {
        return outside_name();
    }
    if (!d.bytes(u32(args, 3), u32(args, 4), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 5), &recipients_at)) {
        return outside_out();
    }
    std::uint64_t recipients = 0;
    const ZenStatus st = d.b.host->office_publish(d.b.host->ctx, as.c_str(), u64(args, 1),
                                                  u64(args, 2), p, u32(args, 4),
                                                  recipients_at != nullptr ? &recipients : nullptr);
    write64(recipients_at, recipients);
    return answer(results, st);
}

BOX_CB(zh_sense_claim) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::sense_claim)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    std::uint8_t* revision_at = nullptr;
    if (!d.bytes(u32(args, 0), u32(args, 1), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 2), &revision_at)) {
        return outside_out();
    }
    std::uint64_t revision = 0;
    const ZenStatus st = d.b.host->sense_claim(d.b.host->ctx, p, u32(args, 1),
                                               revision_at != nullptr ? &revision : nullptr);
    write64(revision_at, revision);
    return answer(results, st);
}

BOX_CB(zh_sense_office_claim) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::sense_office_claim)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string as;
    const std::uint8_t* p = nullptr;
    std::uint8_t* revision_at = nullptr;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), as)) {
        return outside_name();
    }
    if (!d.bytes(u32(args, 1), u32(args, 2), &p)) {
        return outside_bytes();
    }
    if (!d.out64(u32(args, 3), &revision_at)) {
        return outside_out();
    }
    std::uint64_t revision = 0;
    const ZenStatus st = d.b.host->sense_office_claim(d.b.host->ctx, as.c_str(), p, u32(args, 2),
                                                      revision_at != nullptr ? &revision : nullptr);
    write64(revision_at, revision);
    return answer(results, st);
}

/// A reading the host wrote: kept host-side, then handed into the module's own sink.
struct Held {
    std::string bytes;
    bool written = false;
};

void held_write(void* ctx, const std::uint8_t* data, std::size_t len) {
    auto* h = static_cast<Held*>(ctx);
    h->written = true;
    h->bytes.append(reinterpret_cast<const char*>(data), len);
}

/// Each written reading into the library's sink (0 the value, 1 the office) through
/// `zw_sink_deliver`, while the observe import is still on the stack; then the authorship.
/// "Never written" stays distinct from "written empty", so a personal claim reads as personal.
wasm_trap_t* deliver_reading(Box& b, std::uint32_t by_ptr, const ZenSenseBy& by, Held (&held)[2]) {
    for (std::int32_t h = 0; h < 2; ++h) {
        const Held& one = held[h];
        if (!one.written) {
            continue;
        }
        std::string why;
        const std::int32_t p = copy_in(
            b, reinterpret_cast<const std::uint8_t*>(one.bytes.data()), one.bytes.size(), why);
        if (p == 0) {
            return trap("a reading could not be handed to the module: " + why);
        }
        wasmtime_val_t a[3] = {i32v(h), i32v(p), i32v(static_cast<std::int32_t>(one.bytes.size()))};
        if (!call(b.cx, b.fn.sink_deliver, a, 3, nullptr, 0, why)) {
            return trap("the module's reading sink failed: " + why);
        }
        free_in(b, p);
    }
    if (by_ptr != 0) {
        std::uint8_t* at = nullptr;
        if (!span(b.cx, b.memory, by_ptr, sizeof(ZenSenseBy), &at)) {
            return trap("the module named a reading's authorship outside its memory");
        }
        static_assert(sizeof(ZenSenseBy) == 48, "ZenSenseBy has one layout on wasm32 and the host");
        std::memcpy(at, &by, sizeof(ZenSenseBy));
    }
    return nullptr;
}

BOX_CB(zh_sense_observe) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::sense_observe)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string shape;
    if (!name_at(d.cx, d.b.memory, u32(args, 1), shape)) {
        return outside_name();
    }
    Held held[2];
    ZenSenseBy by{};
    const ZenStatus st = d.b.host->sense_observe(d.b.host->ctx, u64(args, 0), shape.c_str(),
                                                 u32(args, 2), ZenByteSink{&held[0], &held_write},
                                                 ZenByteSink{&held[1], &held_write}, &by);
    if (wasm_trap_t* t = deliver_reading(d.b, u32(args, 3), by, held)) {
        return t;
    }
    return answer(results, st);
}

BOX_CB(zh_sense_observe_office) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::sense_observe_office)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    std::string role;
    std::string shape;
    if (!name_at(d.cx, d.b.memory, u32(args, 0), role) ||
        !name_at(d.cx, d.b.memory, u32(args, 1), shape)) {
        return outside_name();
    }
    Held held[2];
    ZenSenseBy by{};
    const ZenStatus st = d.b.host->sense_observe_office(
        d.b.host->ctx, role.c_str(), shape.c_str(), u32(args, 2),
        ZenByteSink{&held[0], &held_write}, ZenByteSink{&held[1], &held_write}, &by);
    if (wasm_trap_t* t = deliver_reading(d.b, u32(args, 3), by, held)) {
        return t;
    }
    return answer(results, st);
}

BOX_CB(zh_sense_offer) {
    BOX_UNUSED;
    Door d(caller);
    if (!d.open(&ZenHostApi::sense_offer)) {
        return answer(results, ZEN_ERR_REFUSED);
    }
    const std::uint8_t* p = nullptr;
    if (!d.bytes(u32(args, 1), u32(args, 2), &p)) {
        return outside_bytes();
    }
    return answer(results, d.b.host->sense_offer(d.b.host->ctx, u64(args, 0), p, u32(args, 2)));
}

// ---- the WASI calls wasi-libc makes, answered without reaching the world ------------------------

constexpr ZenStatus kWasiBadDescriptor = 8; // EBADF
constexpr ZenStatus kWasiNoSuchCall = 52;   // ENOSYS

/// An empty environment, or an empty argument list: two counts written as zero.
BOX_CB(wasi_empty_sizes) {
    BOX_UNUSED;
    Door d(caller);
    std::uint8_t* first = nullptr;
    std::uint8_t* second = nullptr;
    if (!span(d.cx, d.b.memory, u32(args, 0), 4, &first) ||
        !span(d.cx, d.b.memory, u32(args, 1), 4, &second)) {
        return trap("a WASI size query named memory outside the module");
    }
    std::memset(first, 0, 4);
    std::memset(second, 0, 4);
    return answer(results, 0);
}

/// The entries of an empty list: nothing to write.
BOX_CB(wasi_empty_list) {
    BOX_UNUSED;
    (void)caller;
    return answer(results, 0);
}

/// No clock: a timer is a reach no grant names.
BOX_CB(wasi_no_clock) {
    BOX_UNUSED;
    (void)caller;
    return answer(results, kWasiNoSuchCall);
}

/// No descriptor exists and no directory is preopened.
BOX_CB(wasi_bad_descriptor) {
    BOX_UNUSED;
    (void)caller;
    return answer(results, kWasiBadDescriptor);
}

/// Standard output and error only, kept to `kBoxLogBytes` an entry for one diagnostic line.
BOX_CB(wasi_fd_write) {
    BOX_UNUSED;
    Door d(caller);
    const std::uint32_t fd = u32(args, 0);
    if (fd != 1 && fd != 2) {
        return answer(results, kWasiBadDescriptor);
    }
    const std::uint32_t n = u32(args, 2);
    std::uint8_t* iov = nullptr;
    if (n > 1024 || !span(d.cx, d.b.memory, u32(args, 1), n * 8u, &iov)) {
        return trap("fd_write named its buffers outside the module");
    }
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        std::memcpy(&offset, iov + i * 8u, 4);
        std::memcpy(&length, iov + i * 8u + 4u, 4);
        std::uint8_t* p = nullptr;
        if (!span(d.cx, d.b.memory, offset, length, &p)) {
            return trap("fd_write named bytes outside the module");
        }
        const std::size_t room = kBoxLogBytes > d.b.log.size() ? kBoxLogBytes - d.b.log.size() : 0;
        const std::size_t keep = std::min<std::size_t>(length, room);
        d.b.log.append(reinterpret_cast<const char*>(p), keep);
        d.b.log_dropped += length - keep;
        total += length;
    }
    std::uint8_t* written = nullptr;
    if (!span(d.cx, d.b.memory, u32(args, 3), 4, &written)) {
        return trap("fd_write named its count outside the module");
    }
    std::memcpy(written, &total, 4);
    return answer(results, 0);
}

/// The module ending itself is a trap, supervised as any other.
BOX_CB(wasi_proc_exit) {
    BOX_UNUSED;
    (void)caller;
    return trap("the module called proc_exit(" + std::to_string(args[0].of.i32) + ")");
}

#undef BOX_CB
#undef BOX_UNUSED

/// One import the box defines. `params` and `returns` spell the types, `i` for i32 and `I` for
/// i64.
struct Import {
    const char* module;
    const char* name;
    const char* params;
    const char* returns;
    wasmtime_func_callback_t callback;
};

constexpr const char* kHost = "zen_host";
constexpr const char* kWasi = "wasi_snapshot_preview1";

/// The whole import set: one door per `ZenHostApi` slot and the host sink, and the WASI calls
/// wasi-libc makes, each answered without reaching the world. A module importing anything else is
/// refused when it is opened, by name.
constexpr Import kImports[] = {
    {kHost, "sink_write", "iii", "", zh_sink_write},
    {kHost, "send", "IIIiii", "i", zh_send},
    {kHost, "publish", "IIii", "i", zh_publish},
    {kHost, "send_to_role", "iIIiii", "i", zh_send_to_role},
    {kHost, "defer_answer", "", "I", zh_defer_answer},
    {kHost, "answer_deferred", "Iii", "i", zh_answer_deferred},
    {kHost, "release_deferred", "I", "", zh_release_deferred},
    {kHost, "answer", "ii", "i", zh_answer},
    {kHost, "office_send", "iIIIiii", "i", zh_office_send},
    {kHost, "office_send_to_role", "iiIIiii", "i", zh_office_send_to_role},
    {kHost, "office_publish", "iIIiii", "i", zh_office_publish},
    {kHost, "sense_claim", "iii", "i", zh_sense_claim},
    {kHost, "sense_office_claim", "iiii", "i", zh_sense_office_claim},
    {kHost, "sense_observe", "Iiii", "i", zh_sense_observe},
    {kHost, "sense_observe_office", "iiii", "i", zh_sense_observe_office},
    {kHost, "sense_offer", "Iii", "i", zh_sense_offer},
    {kWasi, "environ_sizes_get", "ii", "i", wasi_empty_sizes},
    {kWasi, "environ_get", "ii", "i", wasi_empty_list},
    {kWasi, "args_sizes_get", "ii", "i", wasi_empty_sizes},
    {kWasi, "args_get", "ii", "i", wasi_empty_list},
    {kWasi, "clock_time_get", "iIi", "i", wasi_no_clock},
    {kWasi, "fd_close", "i", "i", wasi_bad_descriptor},
    {kWasi, "fd_fdstat_get", "ii", "i", wasi_bad_descriptor},
    {kWasi, "fd_prestat_get", "ii", "i", wasi_bad_descriptor},
    {kWasi, "fd_prestat_dir_name", "iii", "i", wasi_bad_descriptor},
    {kWasi, "fd_seek", "iIii", "i", wasi_bad_descriptor},
    {kWasi, "fd_write", "iiii", "i", wasi_fd_write},
    {kWasi, "proc_exit", "i", "", wasi_proc_exit},
};

wasm_valtype_vec_t types(const char* spelled) {
    std::vector<wasm_valtype_t*> v;
    for (const char* c = spelled; *c != '\0'; ++c) {
        v.push_back(wasm_valtype_new(*c == 'I' ? WASM_I64 : WASM_I32));
    }
    wasm_valtype_vec_t out;
    wasm_valtype_vec_new(&out, v.size(), v.data());
    return out;
}

void define_imports(wasmtime_linker_t* linker, std::string& error) {
    for (const Import& im : kImports) {
        wasm_valtype_vec_t params = types(im.params);
        wasm_valtype_vec_t returns = types(im.returns);
        wasm_functype_t* ft = wasm_functype_new(&params, &returns);
        if (wasmtime_error_t* e = wasmtime_linker_define_func(
                linker, im.module, std::strlen(im.module), im.name, std::strlen(im.name), ft,
                im.callback, nullptr, nullptr)) {
            error = "the box could not define " + std::string(im.module) + "." + im.name + ": " +
                    error_text(e);
        }
        wasm_functype_delete(ft);
    }
}

bool defined(std::string_view module, std::string_view name) {
    for (const Import& im : kImports) {
        if (module == im.module && name == im.name) {
            return true;
        }
    }
    return false;
}

/// Every import of `module` the box does not define, by `module.name`, comma-separated.
std::string undefined_imports(const wasmtime_module_t* module) {
    std::string out;
    wasm_importtype_vec_t imports;
    wasmtime_module_imports(module, &imports);
    for (std::size_t i = 0; i < imports.size; ++i) {
        const wasm_name_t* m = wasm_importtype_module(imports.data[i]);
        const wasm_name_t* n = wasm_importtype_name(imports.data[i]);
        const std::string_view mv(m->data, m->size);
        const std::string_view nv(n->data, n->size);
        if (!defined(mv, nv)) {
            out += (out.empty() ? "" : ", ") + std::string(mv) + "." + std::string(nv);
        }
    }
    wasm_importtype_vec_delete(&imports);
    return out;
}

// ---- stores and instances -----------------------------------------------------------------------

void drop_store(Box& b) {
    if (b.store != nullptr) {
        wasmtime_store_delete(b.store);
        count(&BoxCounts::stores_closed);
    }
    b.store = nullptr;
    b.cx = nullptr;
    b.weave = 0;
}

void apply_limits(Box& b) {
    // One instance, one memory and one table a store; the memory as the grant gave it.
    wasmtime_store_limiter(b.store, b.memory_limit, 100'000, 1, 1, 1);
}

bool export_func(Box& b, const char* name, wasmtime_func_t& out) {
    wasmtime_extern_t ex;
    if (!wasmtime_instance_export_get(b.cx, &b.instance, name, std::strlen(name), &ex)) {
        return false;
    }
    if (ex.kind != WASMTIME_EXTERN_FUNC) {
        wasmtime_extern_delete(&ex);
        return false;
    }
    out = ex.of.func;
    return true;
}

/// A fresh store and instance of the box's module, its static initialisation run; nothing of the
/// weave is made yet. False with `why` when the module cannot be run as a weave.
bool instantiate(Box& b, std::string& why) {
    Engine& en = engine();
    b.store = wasmtime_store_new(en.engine, &b, nullptr);
    b.cx = wasmtime_store_context(b.store);
    count(&BoxCounts::stores_opened);
    apply_limits(b);
    refuel(b);
    wasm_trap_t* t = nullptr;
    if (wasmtime_error_t* err =
            wasmtime_linker_instantiate(en.linker, b.cx, b.module->get(), &b.instance, &t)) {
        why = "the module did not instantiate: " + error_text(err);
        return false;
    }
    if (t != nullptr) {
        why = "the module trapped while instantiating: " + trap_text(t);
        return false;
    }
    wasmtime_extern_t ex;
    if (!wasmtime_instance_export_get(b.cx, &b.instance, "memory", 6, &ex) ||
        ex.kind != WASMTIME_EXTERN_MEMORY) {
        why = "the module exports no memory";
        return false;
    }
    b.memory = ex.of.memory;
    struct Named {
        const char* name;
        wasmtime_func_t Exports::*slot;
    };
    static constexpr Named kTable[] = {
        {"_initialize", &Exports::initialize}, {"zw_abi_version", &Exports::abi_version},
        {"zw_alloc", &Exports::alloc},         {"zw_free", &Exports::free},
        {"zw_create", &Exports::create},       {"zw_destroy", &Exports::destroy},
        {"zw_describe", &Exports::describe},   {"zw_snapshot", &Exports::snapshot},
        {"zw_policy", &Exports::policy},       {"zw_revive", &Exports::revive},
        {"zw_handle", &Exports::handle},       {"zw_claim_published", &Exports::claim_published},
        {"zw_sink_deliver", &Exports::sink_deliver}};
    std::string missing;
    for (const Named& n : kTable) {
        if (!export_func(b, n.name, b.fn.*n.slot)) {
            missing += (missing.empty() ? "" : ", ") + std::string(n.name);
        }
    }
    if (!missing.empty()) {
        why = "the module does not export the weave table a box drives: " + missing;
        return false;
    }
    if (!call(b.cx, b.fn.initialize, nullptr, 0, nullptr, 0, why)) {
        why = "the module trapped in its static initialisation: " + why;
        return false;
    }
    return true;
}

/// The ABI version the module's weave was built against, read through its own export.
bool abi_version_of(Box& b, std::uint32_t& version, std::string& why) {
    refuel(b);
    wasmtime_val_t r;
    if (!call(b.cx, b.fn.abi_version, nullptr, 0, &r, 1, why)) {
        why = "the module trapped reporting its ABI version: " + why;
        return false;
    }
    version = static_cast<std::uint32_t>(r.of.i32);
    return true;
}

void str_write(void* ctx, const std::uint8_t* data, std::size_t len) {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
}

/// Run one of the slots that write bytes (describe, snapshot, policy) into `out`. False with
/// `why` empty when the weave answered a failing status, non-empty when the module trapped.
bool emit(Box& b, const wasmtime_func_t& f, std::string& out, std::string& why) {
    refuel(b);
    b.sink = ZenByteSink{&out, &str_write};
    wasmtime_val_t a[2] = {i32v(b.weave), i32v(0)};
    wasmtime_val_t r;
    const bool ok = call(b.cx, f, a, 2, &r, 1, why);
    b.sink = ZenByteSink{};
    return ok && r.of.i32 == ZEN_OK;
}

/// Make the weave in the box's instance; its refusal's words go to `refusal`.
bool make_weave(Box& b, ZenByteSink refusal, std::string& why) {
    refuel(b);
    b.sink = refusal;
    wasmtime_val_t a = i32v(0);
    wasmtime_val_t r;
    const bool ok = call(b.cx, b.fn.create, &a, 1, &r, 1, why);
    b.sink = ZenByteSink{};
    if (!ok) {
        why = "the module trapped while making its weave: " + why;
        return false;
    }
    b.weave = r.of.i32;
    return b.weave != 0; // zero: the weave refused its making, in its own words
}

/// The weave's own reload budget, read from its lifecycle policy through the gate. A policy the
/// gate refuses leaves no budget: the first trap quarantines. A trap reading it sets `why`.
std::int64_t max_reloads_of(Box& b, std::string& why) {
    std::string bytes;
    if (!emit(b, b.fn.policy, bytes, why)) {
        return 0;
    }
    Admission a = admit(parse(bytes), lifecycle_policy_schema());
    if (!a.ok()) {
        return 0;
    }
    return std::max<std::int64_t>(0, a.value().get("max_reloads")->as_int());
}

/// One line on the host's standard error, the box's only voice of its own.
void say(const std::string& line) { std::fputs(("[loom box] " + line + "\n").c_str(), stderr); }

void flush_log(Box& b) {
    if (b.log.empty() && b.log_dropped == 0) {
        return;
    }
    std::string line = "the module wrote: " + b.log;
    if (b.log_dropped != 0) {
        line += " [" + std::to_string(b.log_dropped) + " bytes dropped]";
    }
    say(line);
    b.log.clear();
    b.log_dropped = 0;
}

/// After a trap: a fresh instance revived from the host-held state, while the weave's budget
/// lasts; then quarantine, where every slot fails and the last good state is still said. A
/// trapped instance's memory is never read again.
void recover(Box& b) {
    drop_store(b);
    if (b.reloads >= b.max_reloads) {
        b.quarantined = true;
        say("quarantined after " + std::to_string(b.reloads) + " reload(s); the last trap: " +
            b.last_trap);
        return;
    }
    ++b.reloads;
    std::string why;
    std::string words;
    if (!instantiate(b, why) || !make_weave(b, ZenByteSink{&words, &str_write}, why)) {
        b.quarantined = true;
        drop_store(b);
        say("a reload failed, quarantined: " + why + words);
        return;
    }
    if (b.have_good) {
        refuel(b);
        const std::int32_t p = copy_in(
            b, reinterpret_cast<const std::uint8_t*>(b.last_good.data()), b.last_good.size(), why);
        wasmtime_val_t a[3] = {i32v(b.weave), i32v(p),
                               i32v(static_cast<std::int32_t>(b.last_good.size()))};
        wasmtime_val_t r;
        if (p == 0 || !call(b.cx, b.fn.revive, a, 3, &r, 1, why) || r.of.i32 != ZEN_OK) {
            b.quarantined = true;
            drop_store(b);
            say("a revival failed, quarantined: " + why);
            return;
        }
        free_in(b, p);
    }
    say("reload " + std::to_string(b.reloads) + " of " + std::to_string(b.max_reloads) +
        " after a trap: " + b.last_trap);
}

/// Take the weave's state after an entry that completed, for a revival after a later trap.
void hold_state(Box& b) {
    std::string snap;
    std::string why;
    if (emit(b, b.fn.snapshot, snap, why)) {
        b.last_good = std::move(snap);
        b.have_good = true;
    } else if (!why.empty()) {
        b.last_trap = why;
        recover(b);
    }
}

bool live(const Box& b) { return !b.quarantined && b.store != nullptr; }

// ---- the descriptor -----------------------------------------------------------------------------

void* box_slot_create(ZenByteSink refusal) {
    static constexpr std::string_view kWords =
        "a boxed weave is made through its module, not through the descriptor";
    if (refusal.write != nullptr) {
        refusal.write(refusal.ctx, reinterpret_cast<const std::uint8_t*>(kWords.data()),
                      kWords.size());
    }
    return nullptr;
}

void box_slot_destroy(void* instance) {
    auto* b = static_cast<Box*>(instance);
    if (b == nullptr) {
        return;
    }
    if (live(*b)) {
        std::string why;
        refuel(*b);
        wasmtime_val_t a = i32v(b->weave);
        (void)call(b->cx, b->fn.destroy, &a, 1, nullptr, 0, why);
    }
    drop_store(*b);
    delete b;
}

ZenStatus emit_slot(void* instance, ZenByteSink sink, wasmtime_func_t Exports::*slot) {
    auto* b = static_cast<Box*>(instance);
    if (!live(*b)) {
        // A quarantined box still says its last good state, so a reload can carry it away.
        if (slot == &Exports::snapshot && b->have_good) {
            sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(b->last_good.data()),
                       b->last_good.size());
            return ZEN_OK;
        }
        return ZEN_ERR;
    }
    std::string out;
    std::string why;
    if (!emit(*b, b->fn.*slot, out, why)) {
        if (!why.empty()) {
            b->last_trap = why;
            recover(*b);
        }
        return ZEN_ERR;
    }
    sink.write(sink.ctx, reinterpret_cast<const std::uint8_t*>(out.data()), out.size());
    return ZEN_OK;
}

ZenStatus box_slot_describe(void* i, ZenByteSink s) { return emit_slot(i, s, &Exports::describe); }
ZenStatus box_slot_snapshot(void* i, ZenByteSink s) { return emit_slot(i, s, &Exports::snapshot); }
ZenStatus box_slot_policy(void* i, ZenByteSink s) { return emit_slot(i, s, &Exports::policy); }

/// Bytes in, one status out; a trap is a failed call and a recovery.
ZenStatus bytes_slot(Box& b, const wasmtime_func_t& f, const std::uint8_t* data, std::size_t len) {
    if (!live(b)) {
        return ZEN_ERR;
    }
    refuel(b);
    std::string why;
    const std::int32_t p = copy_in(b, data, len, why);
    if (p != 0) {
        wasmtime_val_t a[3] = {i32v(b.weave), i32v(p), i32v(static_cast<std::int32_t>(len))};
        wasmtime_val_t r;
        if (call(b.cx, f, a, 3, &r, 1, why)) {
            free_in(b, p);
            return r.of.i32;
        }
    }
    b.last_trap = why;
    recover(b);
    return ZEN_ERR;
}

ZenStatus box_slot_revive(void* instance, const uint8_t* state, size_t len) {
    auto* b = static_cast<Box*>(instance);
    const ZenStatus st = bytes_slot(*b, b->fn.revive, state, len);
    if (st == ZEN_OK) {
        hold_state(*b);
    }
    return st;
}

ZenStatus box_slot_claim_published(void* instance, const uint8_t* value, size_t len) {
    auto* b = static_cast<Box*>(instance);
    const ZenStatus st = bytes_slot(*b, b->fn.claim_published, value, len);
    if (live(*b)) {
        hold_state(*b);
    }
    return st;
}

ZenStatus box_slot_handle(void* instance, uint64_t sender, uint64_t reply_to,
                          uint64_t correlation, uint32_t provenance, int64_t attested_sequence,
                          const char* authored_role, const uint8_t* payload, size_t len,
                          const ZenHostApi* host) {
    auto* b = static_cast<Box*>(instance);
    if (!live(*b)) {
        return ZEN_ERR;
    }
    refuel(*b);
    std::string why;
    const std::int32_t p = copy_in(*b, payload, len, why);
    std::int32_t role = 0;
    if (p != 0 && authored_role != nullptr) {
        role = copy_in(*b, reinterpret_cast<const std::uint8_t*>(authored_role),
                       std::strlen(authored_role) + 1, why);
    }
    ZenStatus st = ZEN_ERR;
    bool trapped = p == 0 || (authored_role != nullptr && role == 0);
    if (!trapped) {
        wasmtime_val_t a[9] = {i32v(b->weave),
                               i64v(static_cast<std::int64_t>(sender)),
                               i64v(static_cast<std::int64_t>(reply_to)),
                               i64v(static_cast<std::int64_t>(correlation)),
                               i32v(as_i32(provenance)),
                               i64v(attested_sequence),
                               i32v(role),
                               i32v(p),
                               i32v(static_cast<std::int32_t>(len))};
        wasmtime_val_t r;
        b->host = host; // the doors exist for exactly this call
        trapped = !call(b->cx, b->fn.handle, a, 9, &r, 1, why);
        b->host = nullptr;
        if (!trapped) {
            st = r.of.i32;
            free_in(*b, p);
            if (role != 0) {
                free_in(*b, role);
            }
        }
    }
    flush_log(*b);
    if (trapped) {
        b->last_trap = why;
        recover(*b);
        return ZEN_ERR; // the delivery did not complete: the bus records HandlerFailed
    }
    hold_state(*b);
    return st;
}

const ZenWeaveAbi kBoxAbi = {.abi_version     = ZEN_ABI_VERSION,
                             .create          = box_slot_create,
                             .destroy         = box_slot_destroy,
                             .describe        = box_slot_describe,
                             .snapshot        = box_slot_snapshot,
                             .policy          = box_slot_policy,
                             .revive          = box_slot_revive,
                             .handle          = box_slot_handle,
                             .claim_published = box_slot_claim_published};

bool read_file(const std::string& path, std::string& bytes, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

} // namespace

// ---- the backend's entry points -----------------------------------------------------------------

std::shared_ptr<BoxModule> box_open(const std::string& path, std::string& error) {
    Engine& en = engine();
    if (!en.error.empty()) {
        error = en.error;
        return nullptr;
    }
    std::string bytes;
    if (!read_file(path, bytes, error)) {
        return nullptr;
    }
    const std::string digest = sha256_hex_prefix(bytes, 32);
    if (std::shared_ptr<BoxModule> found = find_kept(digest)) {
        return found; // judged whole when it was first opened, and unchanged since
    }
    wasmtime_module_t* compiled = nullptr;
    if (wasmtime_error_t* err =
            wasmtime_module_new(en.engine, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                bytes.size(), &compiled)) {
        error = "the module did not compile: " + error_text(err);
        return nullptr;
    }
    count(&BoxCounts::modules_compiled);
    auto module = std::make_shared<BoxModule>(compiled);
    // Refused by name before anything is instantiated: no code of the module has run.
    const std::string undefined = undefined_imports(compiled);
    if (!undefined.empty()) {
        error = "the module imports what a box never grants: " + undefined;
        return nullptr;
    }
    // One throwaway instance, for the version its weave was built against: refused before any
    // slot of the weave runs, both versions named, as a native image's is.
    Box probe;
    probe.module = module;
    std::string why;
    std::uint32_t version = 0;
    const bool read = instantiate(probe, why) && abi_version_of(probe, version, why);
    drop_store(probe);
    if (!read) {
        error = why;
        return nullptr;
    }
    if (version != ZEN_ABI_VERSION) {
        error = "unsupported abi_version " + std::to_string(version) + " (host supports " +
                std::to_string(ZEN_ABI_VERSION) + ")";
        return nullptr;
    }
    keep(digest, module);
    return module;
}

const ZenWeaveAbi* box_abi() noexcept { return &kBoxAbi; }

void* box_create(const std::shared_ptr<BoxModule>& module, ZenByteSink refusal) {
    auto b = std::make_unique<Box>();
    b->module = module;
    std::string why;
    std::string words;
    bool made = instantiate(*b, why) && make_weave(*b, ZenByteSink{&words, &str_write}, why);
    if (made) {
        b->max_reloads = max_reloads_of(*b, why);
        if (!why.empty()) {
            why = "the module trapped saying its lifecycle policy: " + why;
            made = false;
        }
    }
    if (!made) {
        const std::string said = why.empty() ? words : "the box: " + why;
        if (refusal.write != nullptr && !said.empty()) {
            refusal.write(refusal.ctx, reinterpret_cast<const std::uint8_t*>(said.data()),
                          said.size());
        }
        drop_store(*b);
        return nullptr;
    }
    hold_state(*b);
    if (!live(*b)) {
        return nullptr; // its first snapshot trapped and its budget was spent
    }
    return b.release();
}

bool box_mount(void* instance, const Grant& grant, std::string& why) {
    auto* b = static_cast<Box*>(instance);
    std::string refused;
    auto refuse = [&refused](const std::string& part) {
        refused += (refused.empty() ? "" : "; ") + part;
    };
    if (grant.os_capabilities() != os_cap::None) {
        std::string caps;
        if (grant.has_os_capability(os_cap::Network)) {
            caps = "network";
        }
        if (grant.has_os_capability(os_cap::SpawnProcess)) {
            caps += std::string(caps.empty() ? "" : " and ") + "process spawning";
        }
        refuse("it gives OS capabilities (" + (caps.empty() ? std::string("unnamed") : caps) +
               "), and a box reaches nothing but the host's doors");
    }
    if (grant.filesystem() != FsAccess::None) {
        refuse(std::string("it gives filesystem access (") + fs_access_name(grant.filesystem()) +
               "), and a box has no filesystem");
    }
    const ResourceLimits& res = grant.resources();
    if (res.cpu_weight != 0) {
        refuse("it gives a cpu weight (" + std::to_string(res.cpu_weight) +
               "), and a box runs on the host's own thread");
    }
    std::int64_t limit = kBoxDefaultMemoryBytes;
    if (res.unlimited_memory) {
        limit = -1; // wasm32's own ceiling
    } else if (res.memory_bytes > 0) {
        limit = res.memory_bytes;
    }
    if (live(*b) && limit >= 0) {
        const std::size_t held = wasmtime_memory_data_size(b->cx, &b->memory);
        if (static_cast<std::uint64_t>(held) > static_cast<std::uint64_t>(limit)) {
            refuse("its memory (" + std::to_string(limit) + " bytes) is less than the module " +
                   "already holds (" + std::to_string(held) + " bytes)");
        }
    }
    if (!refused.empty()) {
        why = "a box cannot carry this grant, and narrows none: " + refused;
        return false;
    }
    b->memory_limit = limit;
    if (b->store != nullptr) {
        apply_limits(*b);
    }
    return true;
}

BoxCounts box_counts() noexcept {
    const std::lock_guard<std::mutex> hold(counts_lock());
    return counts();
}

} // namespace loom::detail
