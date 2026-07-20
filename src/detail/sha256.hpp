#ifndef ZEN_DETAIL_SHA256_HPP
#define ZEN_DETAIL_SHA256_HPP

// Internal to loom. A self-contained SHA-256 (FIPS 180-4), dependency-free: no
// external crypto library is linked anywhere in the project, and this adds none —
// it is one file of standard, auditable code, pinned to NIST known-answer vectors
// (see test_policy.cpp). It sits beside detail/hash.hpp's FNV-1a but answers a
// different question: FNV names a build cheaply (schema content-ids), SHA-256 names
// one *collision-resistantly*, which is what an above-floor grant key needs (audit
// F-1 — FNV-1a's ~32-bit birthday resistance was too weak to key a security-relevant
// identity). This is content-addressing (a second-preimage-resistant name), NOT
// authentication: it is not a MAC and not constant-time; a *signed* author identity
// is still the identity phase's job.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loom::detail {

// Streaming SHA-256. One-shot for our use: feed bytes with update(), read digest()
// once (digest() finalizes in place; do not update() or digest() again after).
class Sha256 {
public:
    Sha256() = default;

    Sha256& update(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i) {
            buffer_[buffer_len_++] = p[i];
            if (buffer_len_ == 64) {
                transform(buffer_.data());
                bit_len_ += 512;
                buffer_len_ = 0;
            }
        }
        return *this;
    }
    Sha256& update(std::string_view s) { return update(s.data(), s.size()); }

    std::array<std::uint8_t, 32> digest() {
        const std::uint64_t total_bits = bit_len_ + static_cast<std::uint64_t>(buffer_len_) * 8;
        // Pad: a 0x80 byte, then zeros, then the 64-bit big-endian message length.
        std::size_t i = buffer_len_;
        buffer_[i++] = 0x80;
        if (i > 56) {
            while (i < 64) {
                buffer_[i++] = 0x00;
            }
            transform(buffer_.data());
            i = 0;
        }
        while (i < 56) {
            buffer_[i++] = 0x00;
        }
        for (int b = 7; b >= 0; --b) {
            buffer_[i++] = static_cast<std::uint8_t>((total_bits >> (b * 8)) & 0xFF);
        }
        transform(buffer_.data());

        std::array<std::uint8_t, 32> out{};
        for (std::size_t j = 0; j < 8; ++j) {
            out[j * 4 + 0] = static_cast<std::uint8_t>((state_[j] >> 24) & 0xFF);
            out[j * 4 + 1] = static_cast<std::uint8_t>((state_[j] >> 16) & 0xFF);
            out[j * 4 + 2] = static_cast<std::uint8_t>((state_[j] >> 8) & 0xFF);
            out[j * 4 + 3] = static_cast<std::uint8_t>(state_[j] & 0xFF);
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) noexcept {
        return (x >> n) | (x << (32 - n));
    }

    void transform(const std::uint8_t* p) noexcept {
        static const std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};

        std::uint32_t w[64];
        for (int t = 0; t < 16; ++t) {
            w[t] = (static_cast<std::uint32_t>(p[t * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(p[t * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[t * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(p[t * 4 + 3]);
        }
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + S1 + ch + K[t] + w[t];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_len_ = 0;
    std::uint64_t bit_len_ = 0;
    std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

/// Lowercase-hex of the first `n_bytes` of SHA-256(data). `n_bytes` is clamped to 32.
/// Truncating to 16 bytes yields a 128-bit content name: ~2^128 second-preimage and
/// ~2^64 collision resistance — ample for a grant key, and worlds past FNV-1a's ~2^32.
inline std::string sha256_hex_prefix(std::string_view data, std::size_t n_bytes) {
    Sha256 h;
    h.update(data);
    const std::array<std::uint8_t, 32> d = h.digest();
    if (n_bytes > d.size()) {
        n_bytes = d.size();
    }
    static const char* const hex = "0123456789abcdef";
    std::string out(n_bytes * 2, '0');
    for (std::size_t i = 0; i < n_bytes; ++i) {
        out[i * 2 + 0] = hex[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[d[i] & 0xF];
    }
    return out;
}

} // namespace loom::detail

#endif // ZEN_DETAIL_SHA256_HPP
