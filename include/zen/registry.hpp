// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_REGISTRY_HPP
#define ZEN_REGISTRY_HPP

#include <zen/schema.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

class Registry;

/// Thrown when a (name, version) already published with one shape is registered
/// again with a different shape. A published schema is immutable: to change a
/// shape, publish a new version.
class SchemaConflict : public std::runtime_error {
public:
    SchemaConflict(std::string name, std::uint32_t version, ContentId existing, ContentId incoming);

    const std::string& schema_name() const noexcept { return name_; }
    std::uint32_t schema_version() const noexcept { return version_; }
    ContentId existing_id() const noexcept { return existing_; }
    ContentId incoming_id() const noexcept { return incoming_; }

private:
    std::string name_;
    std::uint32_t version_;
    ContentId existing_;
    ContentId incoming_;
};

namespace detail {
/// The Registry's interior, held by shared_ptr so a claim scope can hold a weak
/// one. Deliberately opaque here: a scope needs to reach its Registry's
/// bookkeeping without the header publishing that bookkeeping.
struct RegistryCore;

/// A schema's identity in a Registry: the (name, version) pair everything keys by.
using SchemaKey = std::pair<std::string, std::uint32_t>;
} // namespace detail

/// A LIVE CLAIM on a set of canonical schemas.
///
/// The Registry retains a schema while at least one live claim requires it, and
/// removes it from lookup when the final claim goes. This object IS a claim: it
/// is what a weave record, a loaded artifact, or a mounted child holds to say
/// "these shapes must still resolve because I still exist". Destroying it — by
/// scope exit, by erasing the record that owns it, by an exception unwinding a
/// half-built load — releases exactly the claims it holds and nothing else.
///
/// Move-only, so a claim has one owner and cleanup is structural: there is no
/// `unregister_schema` for a failure exit to forget to call.
///
/// A claim is about LIFETIME, not authorship. It answers "why must this
/// vocabulary still resolve?", never "who wrote this schema?" — several
/// unrelated owners may legitimately claim one canonical definition, and the
/// Registry keeps one definition and counts the claims.
///
/// Releasing the last claim removes the schema from THIS Registry's current
/// lookup. It does not free memory on any schedule: values already admitted own
/// their schema strongly, and a reader holding an older snapshot keeps that
/// snapshot's entries alive. Reclamation here is a statement about
/// discoverability, not about the heap.
class SchemaClaimScope {
public:
    /// An unbound scope, claiming nothing. Registry::claim binds it.
    SchemaClaimScope() noexcept;
    ~SchemaClaimScope();

    SchemaClaimScope(SchemaClaimScope&& other) noexcept;
    SchemaClaimScope& operator=(SchemaClaimScope&& other) noexcept;
    SchemaClaimScope(const SchemaClaimScope&) = delete;
    SchemaClaimScope& operator=(const SchemaClaimScope&) = delete;

    /// Does this scope hold no claims? True for a fresh scope, a moved-from one,
    /// and a released one alike.
    bool empty() const noexcept { return keys_.empty(); }

    /// Release every claim this scope holds, now rather than at destruction.
    /// Idempotent; the scope is reusable (Registry::claim may bind it again).
    ///
    /// Refuses rather than corrupts: if the republication cannot be allocated,
    /// the claims are simply kept and the Registry is left exactly as it was.
    void release() noexcept;

private:
    friend class Registry;

    std::weak_ptr<detail::RegistryCore> core_;
    std::vector<detail::SchemaKey> keys_; ///< one entry per claim held
};

/// The kernel's grammar store: the schemas the system knows. Holds them as the
/// canonical shared owners that values reference, so a value's schema can never
/// dangle. Supports registering schemas discovered at runtime (the DLL case).
///
/// Lifetime: a schema is discoverable while something live claims it (BL-0).
/// There are two doors, and the difference between them is the whole model.
///
/// ```text
/// register_schema(s)   a claim with no stated end — the schema stays for the
///                      lifetime of this Registry. What an application means
///                      when it publishes its own vocabulary at startup.
///
/// claim(schemas)       a claim with an owner — a SchemaClaimScope. The schemas
///                      resolve while that scope lives and stop resolving when
///                      the last scope claiming them dies.
/// ```
///
/// "Permanent" is therefore ordinary lifetime rather than an exceptional bit:
/// `register_schema` is a claim nobody ever releases.
///
/// Threading: copy-on-write. The map is an immutable snapshot swapped wholesale
/// whenever the POPULATION changes; readers take a shared lock only long enough
/// to copy the snapshot pointer, then traverse it lock-free. Claim counts live
/// beside the snapshot rather than inside it, so a second claim on a schema
/// already present costs no copy at all. Registration takes an exclusive lock
/// and is expected to be rare. (On a toolchain with std::atomic<std::shared_ptr>
/// the snapshot load becomes wait-free with no API change; GCC 11 lacks that
/// specialization, hence the shared_mutex.)
class Registry {
public:
    Registry();
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

    /// The outcome of a registration.
    struct Registration {
        std::shared_ptr<const Schema> schema; ///< the canonical owner (use this)
        bool inserted; ///< true if newly added; false if identical content already present
    };

    /// Publish a schema for this Registry's whole lifetime. Idempotent when the
    /// same (name, version) is already present with identical content (returns
    /// the existing canonical schema, inserted == false). Throws SchemaConflict
    /// on a same-key/different-content collision. Throws std::invalid_argument
    /// if `schema` is null.
    ///
    /// The claim this takes is never released. Reach for `claim` instead
    /// wherever some live object's lifetime is the honest answer to "how long
    /// must this shape resolve?".
    Registration register_schema(std::shared_ptr<const Schema> schema);

    /// Claim a set of schemas, transactionally, and hand back the claim.
    ///
    /// ALL OR NOTHING. The complete set is validated against the current
    /// population before anything is published, so a conflict on the last
    /// schema leaves no claim on the first: Registry state is exactly what it
    /// was before the attempt. Duplicates within one request are one claim.
    ///
    /// Throws SchemaConflict if any requested (name, version) is already
    /// published with different content — the existing definition is untouched.
    /// Throws std::invalid_argument if any entry is null.
    ///
    /// One publication per call, whatever the set's size.
    SchemaClaimScope claim(const std::vector<std::shared_ptr<const Schema>>& schemas);

    /// The same acquisition, appended to a scope that already exists — for a
    /// caller that must claim in stages because decoding one schema needs the
    /// previous one resolvable (a manifest's nested references). Each call is
    /// its own transaction; the scope releases everything it accumulated.
    ///
    /// Throws std::invalid_argument if the scope belongs to another Registry.
    void claim(SchemaClaimScope& scope, const std::vector<std::shared_ptr<const Schema>>& schemas);

    /// Claim schemas this Registry ALREADY KNOWS, by identity, offering no
    /// definition — for a claimant that depends on a vocabulary without owning
    /// it. A producer is the case: it names the shapes it may emit, and its
    /// bytes stay decodable while it lives, even after the weave that first
    /// published those shapes goes away.
    ///
    /// A key that is not currently published is SKIPPED, not an error and not a
    /// conflict: there is no definition to keep alive, and a claimant that has
    /// none can only ever have been told "this Loom does not know that shape".
    ///
    /// One publication at most; nothing new can ever be published by this call.
    void claim_known(SchemaClaimScope& scope, const std::vector<detail::SchemaKey>& keys);

    /// Resolve a schema by its identity key, or nullptr if not present. Safe to
    /// call concurrently with registration.
    ///
    /// The returned pointer is a STRONG owner: it stays valid after the last
    /// claim is released and the key leaves this Registry.
    std::shared_ptr<const Schema> lookup(std::string_view name, std::uint32_t version) const;

    bool contains(std::string_view name, std::uint32_t version) const;

    /// How many schemas are currently discoverable. This is the population
    /// claims bound — not a lifetime total of everything ever registered.
    std::size_t size() const;

private:
    std::shared_ptr<detail::RegistryCore> core_;
};

} // namespace loom

#endif // ZEN_REGISTRY_HPP
