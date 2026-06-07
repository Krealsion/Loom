#include "detail/binary.hpp"

#include <cstring>

namespace zen::detail {

// ---- Writers --------------------------------------------------------------

void put_u8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }

void put_u16le(std::string& out, std::uint16_t v) {
    put_u8(out, static_cast<std::uint8_t>(v & 0xFF));
    put_u8(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u32le(std::string& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        put_u8(out, static_cast<std::uint8_t>((v >> s) & 0xFF));
    }
}

void put_u64le(std::string& out, std::uint64_t v) {
    for (int s = 0; s < 64; s += 8) {
        put_u8(out, static_cast<std::uint8_t>((v >> s) & 0xFF));
    }
}

void put_f64le(std::string& out, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    // Normalize every NaN to the one canonical pattern; this also leaves ±0.0,
    // ±inf, and finite values exactly as their IEEE bits (so −0.0 is preserved).
    const std::uint64_t exp = bits & 0x7FF0000000000000ULL;
    const std::uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;
    if (exp == 0x7FF0000000000000ULL && mant != 0) {
        bits = kCanonicalNaNBits;
    }
    put_u64le(out, bits);
}

void put_uvarint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        put_u8(out, static_cast<std::uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    put_u8(out, static_cast<std::uint8_t>(v));
}

void put_zigzag(std::string& out, std::int64_t v) {
    const std::uint64_t sign = (v < 0) ? ~0ULL : 0ULL;
    const std::uint64_t zz = (static_cast<std::uint64_t>(v) << 1) ^ sign;
    put_uvarint(out, zz);
}

// ---- Reader ---------------------------------------------------------------

bool BinReader::u8(std::uint8_t& out) noexcept {
    if (remaining() < 1) {
        return false;
    }
    out = p_[i_++];
    return true;
}

bool BinReader::u16le(std::uint16_t& out) noexcept {
    if (remaining() < 2) {
        return false;
    }
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(p_[i_]) |
                                     (static_cast<std::uint16_t>(p_[i_ + 1]) << 8));
    i_ += 2;
    return true;
}

bool BinReader::u32le(std::uint32_t& out) noexcept {
    if (remaining() < 4) {
        return false;
    }
    std::uint32_t v = 0;
    for (int b = 0; b < 4; ++b) {
        v |= static_cast<std::uint32_t>(p_[i_ + static_cast<std::size_t>(b)]) << (8 * b);
    }
    i_ += 4;
    out = v;
    return true;
}

bool BinReader::u64le(std::uint64_t& out) noexcept {
    if (remaining() < 8) {
        return false;
    }
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b) {
        v |= static_cast<std::uint64_t>(p_[i_ + static_cast<std::size_t>(b)]) << (8 * b);
    }
    i_ += 8;
    out = v;
    return true;
}

bool BinReader::f64le(double& out) noexcept {
    std::uint64_t bits = 0;
    if (!u64le(bits)) {
        return false;
    }
    const std::uint64_t exp = bits & 0x7FF0000000000000ULL;
    const std::uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;
    if (exp == 0x7FF0000000000000ULL && mant != 0 && bits != kCanonicalNaNBits) {
        return false; // a non-canonical NaN payload is not a valid encoding
    }
    double d = 0;
    std::memcpy(&d, &bits, sizeof(d));
    out = d;
    return true;
}

bool BinReader::uvarint(std::uint64_t& out) noexcept {
    std::uint64_t result = 0;
    int shift = 0;
    for (int idx = 0;; ++idx) {
        if (idx >= 10) {
            return false; // more bytes than a 64-bit value can need
        }
        std::uint8_t b = 0;
        if (!u8(b)) {
            return false; // truncated
        }
        if (idx == 9 && b > 0x01) {
            return false; // the 10th byte may carry only the top bit (no overflow)
        }
        result |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) {
            if (idx > 0 && b == 0) {
                return false; // non-minimal: a trailing all-zero group
            }
            out = result;
            return true;
        }
    }
}

bool BinReader::zigzag(std::int64_t& out) noexcept {
    std::uint64_t zz = 0;
    if (!uvarint(zz)) {
        return false;
    }
    const std::uint64_t mag = zz >> 1;
    const std::uint64_t neg = ~(zz & 1ULL) + 1ULL; // 0 or 0xFFFF...FF
    out = static_cast<std::int64_t>(mag ^ neg);
    return true;
}

bool BinReader::take(std::size_t count, std::string_view& out) noexcept {
    if (remaining() < count) {
        return false;
    }
    out = std::string_view(reinterpret_cast<const char*>(p_ + i_), count);
    i_ += count;
    return true;
}

} // namespace zen::detail
