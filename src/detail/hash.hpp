#ifndef ZEN_DETAIL_HASH_HPP
#define ZEN_DETAIL_HASH_HPP

// Internal to zen-core. FNV-1a, 64-bit. Chosen for being tiny, dependency-free,
// and deterministic across platforms and runs (no seeding) — the content
// identity of a schema must be the same everywhere, forever. This is NOT a
// cryptographic hash: it identifies schemas, it does not authenticate bytes.
//
// The algorithm and seed/prime below are frozen: the 64-bit content id they
// produce appears in the wire format, so changing them would silently
// reinterpret every persisted value's identity.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zen::detail {

using Hash64 = std::uint64_t;

inline constexpr Hash64 kFnvOffset = 1469598103934665603ULL;
inline constexpr Hash64 kFnvPrime = 1099511628211ULL;

/// Streaming FNV-1a so a hash can be folded over structured input.
class Fnv1a {
public:
    constexpr Fnv1a() = default;

    constexpr Fnv1a& byte(std::uint8_t b) noexcept {
        h_ ^= static_cast<Hash64>(b);
        h_ *= kFnvPrime;
        return *this;
    }

    constexpr Fnv1a& bytes(std::string_view s) noexcept {
        for (char c : s) {
            byte(static_cast<std::uint8_t>(c));
        }
        return *this;
    }

    /// Fold in a length-prefixed string. The length prefix prevents
    /// concatenation ambiguity: hashing "ab"+"c" must differ from "a"+"bc".
    constexpr Fnv1a& field(std::string_view s) noexcept {
        u64(static_cast<Hash64>(s.size()));
        return bytes(s);
    }

    constexpr Fnv1a& u64(Hash64 v) noexcept {
        for (int i = 0; i < 8; ++i) {
            byte(static_cast<std::uint8_t>(v & 0xFFU));
            v >>= 8;
        }
        return *this;
    }

    constexpr Hash64 value() const noexcept { return h_; }

private:
    Hash64 h_ = kFnvOffset;
};

} // namespace zen::detail

#endif // ZEN_DETAIL_HASH_HPP
