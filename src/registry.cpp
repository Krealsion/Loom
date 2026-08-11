// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/registry.hpp>

#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace loom {

namespace {
std::string conflict_what(const std::string& name, std::uint32_t version) {
    return "schema '" + name + "' v" + std::to_string(version) +
           " is already published with a different shape (published schemas are immutable)";
}
} // namespace

SchemaConflict::SchemaConflict(std::string name, std::uint32_t version, ContentId existing,
                               ContentId incoming)
    : std::runtime_error(conflict_what(name, version)), name_(std::move(name)), version_(version),
      existing_(existing), incoming_(incoming) {}

namespace detail {

/// THE TWO HALVES, AND WHY THEY ARE SEPARATE.
///
/// `current` is the published immutable snapshot: what readers traverse
/// lock-free, and the only thing `lookup` can see. `claims` is the private
/// bookkeeping that decides how long an entry stays in it.
///
/// Keeping the counts OUT of the published map is what makes a shared schema
/// cheap. Two weaves that both accept `Greet v1` produce two claims and ONE
/// population — and the second claim copies nothing, because the snapshot did
/// not change. If the count lived in the snapshot, every claim and every release
/// would be a whole-map copy, which is the cost claim-scoped retention narrows.
///
/// Invariant: the key sets of `current` and `claims` are equal, and every count
/// is >= 1. An entry reaching zero leaves both, together, in one publication.
struct RegistryCore {
    using Key = SchemaKey;
    using Map = std::map<Key, std::shared_ptr<const Schema>>;

    mutable std::shared_mutex mtx;
    std::shared_ptr<const Map> current{std::make_shared<const Map>()};
    std::map<Key, std::size_t> claims;

    std::shared_ptr<const Map> snapshot() const {
        std::shared_lock<std::shared_mutex> lk(mtx);
        return current;
    }

    /// Validate the whole request, then commit it. Nothing is mutated until
    /// every entry has been checked, so a conflict anywhere leaves the Registry
    /// exactly as it was (the transactional half of LIFE-08).
    void acquire(const std::vector<std::shared_ptr<const Schema>>& schemas,
                 std::vector<Key>& out_keys) {
        // ---- validate; no mutation below this point until the commit ----
        // `wanted` is also the de-duplicator: the same shape twice in one
        // request is one claim, so one release balances it.
        std::map<Key, std::shared_ptr<const Schema>> wanted;
        for (const std::shared_ptr<const Schema>& s : schemas) {
            if (!s) {
                throw std::invalid_argument("Registry::claim requires non-null schemas");
            }
            Key key{s->name(), s->version()};
            if (auto seen = wanted.find(key); seen != wanted.end()) {
                if (seen->second->content_id() != s->content_id()) {
                    throw SchemaConflict(s->name(), s->version(), seen->second->content_id(),
                                         s->content_id());
                }
                continue; // the same shape asked for twice
            }
            wanted.emplace(std::move(key), s);
        }
        if (wanted.empty()) {
            return;
        }

        std::unique_lock<std::shared_mutex> lk(mtx);
        std::size_t additions = 0;
        for (const auto& [key, schema] : wanted) {
            auto it = current->find(key);
            if (it == current->end()) {
                ++additions;
                continue;
            }
            if (it->second->content_id() != schema->content_id()) {
                throw SchemaConflict(schema->name(), schema->version(), it->second->content_id(),
                                     schema->content_id());
            }
        }

        // ---- commit: one publication for the whole set ----
        // Grown here, where a failure still costs nothing: past this point a
        // throw could leave a claim the scope does not know it holds.
        out_keys.reserve(out_keys.size() + wanted.size());
        if (additions != 0) {
            auto next = std::make_shared<Map>(*current);
            for (const auto& [key, schema] : wanted) {
                next->emplace(key, schema); // emplace: an existing canonical entry wins
            }
            current = std::move(next);
        }
        for (const auto& [key, schema] : wanted) {
            ++claims[key];
            out_keys.push_back(key);
        }
    }

    /// Claim keys already published, skipping the rest. Publishes nothing — the
    /// population cannot grow here — so there is no snapshot copy at all.
    void acquire_known(const std::vector<Key>& keys, std::vector<Key>& out_keys) {
        if (keys.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lk(mtx);
        out_keys.reserve(out_keys.size() + keys.size());
        std::map<Key, bool> taken; // de-duplicate: one claim per distinct key
        for (const Key& key : keys) {
            if (!taken.emplace(key, true).second) {
                continue;
            }
            if (current->find(key) == current->end()) {
                continue; // nothing to keep alive
            }
            ++claims[key];
            out_keys.push_back(key);
        }
    }

    /// Drop these claims and, for every schema whose last one this was, remove
    /// it from lookup — in ONE publication however many schemas that is.
    ///
    /// Everything that can throw (the tally, the snapshot copy) happens before
    /// anything is mutated, and the commit below allocates nothing. So a caller
    /// that swallows an exception here gets claims retained, never a Registry
    /// whose two halves disagree.
    void release(const std::vector<Key>& keys) {
        if (keys.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lk(mtx);

        std::map<Key, std::size_t> dropping;
        for (const Key& key : keys) {
            ++dropping[key];
        }
        bool population_changes = false;
        for (const auto& [key, n] : dropping) {
            auto it = claims.find(key);
            if (it != claims.end() && it->second <= n) {
                population_changes = true;
                break;
            }
        }
        std::shared_ptr<Map> next;
        if (population_changes) {
            next = std::make_shared<Map>(*current);
        }

        // ---- commit; no allocation, no throw ----
        for (const auto& [key, n] : dropping) {
            auto it = claims.find(key);
            if (it == claims.end()) {
                continue; // released twice, or claimed against a Registry that is gone
            }
            if (it->second <= n) {
                claims.erase(it);
                next->erase(key);
            } else {
                it->second -= n;
            }
        }
        if (next) {
            current = std::move(next);
        }
    }
};

} // namespace detail

// ---- SchemaClaimScope ------------------------------------------------------

SchemaClaimScope::SchemaClaimScope() noexcept = default;

SchemaClaimScope::~SchemaClaimScope() { release(); }

SchemaClaimScope::SchemaClaimScope(SchemaClaimScope&& other) noexcept
    : core_(std::move(other.core_)), keys_(std::move(other.keys_)) {
    other.keys_.clear(); // a moved-from scope claims nothing and releases nothing
}

SchemaClaimScope& SchemaClaimScope::operator=(SchemaClaimScope&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // ORDER MATTERS AND IS THE POINT (LIFE-08, replacement overlap). The incoming
    // claims were acquired BEFORE this assignment ran, so a schema both scopes
    // claim is at count two here and drops to one below — it never leaves the
    // Registry, and there is no instant during a handoff when the vocabulary a
    // successor needs is unresolvable.
    release();
    core_ = std::move(other.core_);
    keys_ = std::move(other.keys_);
    other.keys_.clear();
    return *this;
}

void SchemaClaimScope::release() noexcept {
    if (keys_.empty()) {
        return;
    }
    // A scope may outlive its Registry (a test fixture's teardown order, a host
    // shutting down). `lock()` failing means there is nothing left to tell.
    if (const std::shared_ptr<detail::RegistryCore> core = core_.lock()) {
        try {
            core->release(keys_);
        } catch (...) {
            // Allocation failure during republication. `release` mutates nothing
            // until it can finish, so the claims are still held and the Registry
            // is intact; the schemas simply stay discoverable. A destructor may
            // not throw, and losing the Registry's consistency to say so would be
            // the worse trade.
            return;
        }
    }
    keys_.clear();
}

// ---- Registry --------------------------------------------------------------

Registry::Registry() : core_(std::make_shared<detail::RegistryCore>()) {}

Registry::~Registry() = default;

Registry::Registration Registry::register_schema(std::shared_ptr<const Schema> schema) {
    if (!schema) {
        throw std::invalid_argument("Registry::register_schema requires a non-null schema");
    }
    detail::RegistryCore::Key key{schema->name(), schema->version()};

    std::unique_lock<std::shared_mutex> lk(core_->mtx);
    if (auto it = core_->current->find(key); it != core_->current->end()) {
        const std::shared_ptr<const Schema>& existing = it->second;
        if (existing->content_id() == schema->content_id()) {
            // Identical re-registration is a no-op on the population — but it is
            // still a claim nobody will release, which is what keeps a schema
            // published this way from being reclaimed under a caller who never
            // asked for a lifetime.
            ++core_->claims[key];
            return {existing, false};
        }
        throw SchemaConflict(schema->name(), schema->version(), existing->content_id(),
                             schema->content_id());
    }

    // Copy-on-write: publish a new immutable snapshot with the addition, then
    // swap it in. Existing reader snapshots keep the old map alive untouched.
    auto next = std::make_shared<detail::RegistryCore::Map>(*core_->current);
    (*next)[key] = schema;
    core_->current = std::move(next);
    ++core_->claims[key];
    return {std::move(schema), true};
}

SchemaClaimScope Registry::claim(const std::vector<std::shared_ptr<const Schema>>& schemas) {
    SchemaClaimScope scope;
    claim(scope, schemas);
    return scope;
}

namespace {
/// Bind a scope to this Registry, or confirm it is already ours. A scope may only
/// ever hold claims on one Registry: `keys_` means nothing anywhere else.
void bind(SchemaClaimScope& scope, const std::shared_ptr<detail::RegistryCore>& core,
          std::weak_ptr<detail::RegistryCore>& scope_core, bool scope_has_claims) {
    const std::shared_ptr<detail::RegistryCore> owner = scope_core.lock();
    if (owner == nullptr) {
        if (scope_has_claims) {
            throw std::invalid_argument(
                "Registry::claim: this scope's Registry is gone; its claims cannot move");
        }
        scope_core = core;
        return;
    }
    if (owner != core) {
        throw std::invalid_argument("Registry::claim: this scope belongs to another Registry");
    }
    (void)scope;
}
} // namespace

void Registry::claim(SchemaClaimScope& scope,
                     const std::vector<std::shared_ptr<const Schema>>& schemas) {
    bind(scope, core_, scope.core_, !scope.keys_.empty());
    core_->acquire(schemas, scope.keys_);
}

void Registry::claim_known(SchemaClaimScope& scope, const std::vector<detail::SchemaKey>& keys) {
    bind(scope, core_, scope.core_, !scope.keys_.empty());
    core_->acquire_known(keys, scope.keys_);
}

std::shared_ptr<const Schema> Registry::lookup(std::string_view name, std::uint32_t version) const {
    std::shared_ptr<const detail::RegistryCore::Map> snap = core_->snapshot();
    auto it = snap->find(detail::RegistryCore::Key{std::string(name), version});
    if (it == snap->end()) {
        return nullptr;
    }
    return it->second;
}

bool Registry::contains(std::string_view name, std::uint32_t version) const {
    return lookup(name, version) != nullptr;
}

std::size_t Registry::size() const { return core_->snapshot()->size(); }

} // namespace loom
