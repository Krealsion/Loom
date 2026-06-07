#ifndef ZEN_DETAIL_BINARY_HPP
#define ZEN_DETAIL_BINARY_HPP

// Internal to zen-core. The low-level primitives of the native binary wire
// format: little-endian fixed integers, canonical (minimal) LEB128 varints,
// zigzag for signed ints, and a canonical IEEE-754 binary64 codec. Writers
// produce the one canonical encoding; readers are total and bounds-checked —
// every read is validated against the remaining input before it touches memory,
// so any byte string is safe to feed in.
//
// These bytes are a permanent, on-the-wire commitment. The layout and the
// canonicality rules here are frozen.

#include <cstdint>
#include <string>
#include <string_view>

namespace zen::detail {

// Named caps. Bound recursion and any length/count read from untrusted bytes.
inline constexpr int kMaxBinaryDepth = 64;            // nested message/list depth
inline constexpr std::uint64_t kMaxListCount = 1u << 20; // elements in one list
inline constexpr std::uint64_t kMaxFieldBytes = 1u << 28; // bytes in one Text/Bytes

// The single canonical quiet-NaN bit pattern (sign 0, exponent all ones, only the
// most-significant mantissa bit set). Encode normalizes every NaN to this; decode
// rejects any other NaN payload, keeping the format content-addressable.
inline constexpr std::uint64_t kCanonicalNaNBits = 0x7FF8000000000000ULL;

// ---- Writers (append the one canonical encoding) --------------------------

void put_u8(std::string& out, std::uint8_t v);
void put_u16le(std::string& out, std::uint16_t v);
void put_u32le(std::string& out, std::uint32_t v);
void put_u64le(std::string& out, std::uint64_t v);
void put_f64le(std::string& out, double v);       // NaN→canonical, −0.0 preserved
void put_uvarint(std::string& out, std::uint64_t v);
void put_zigzag(std::string& out, std::int64_t v);

// ---- Reader (total, bounds-checked) ---------------------------------------

class BinReader {
public:
    explicit BinReader(std::string_view bytes) noexcept
        : p_(reinterpret_cast<const unsigned char*>(bytes.data())), n_(bytes.size()) {}

    std::size_t remaining() const noexcept { return n_ - i_; }
    bool at_end() const noexcept { return i_ >= n_; }

    bool u8(std::uint8_t& out) noexcept;
    bool u16le(std::uint16_t& out) noexcept;
    bool u32le(std::uint32_t& out) noexcept;
    bool u64le(std::uint64_t& out) noexcept;

    /// Reads 8 bytes as binary64. Rejects any NaN that is not the canonical
    /// pattern; accepts ±0, ±inf, and all finite values.
    bool f64le(double& out) noexcept;

    /// Minimal unsigned LEB128. Rejects non-minimal (trailing zero group) and
    /// overlong (> 64-bit) encodings, and truncation.
    bool uvarint(std::uint64_t& out) noexcept;

    /// Minimal zigzag-encoded signed varint.
    bool zigzag(std::int64_t& out) noexcept;

    /// Borrow `count` bytes as a view into the underlying buffer (no copy) and
    /// advance. Fails if fewer than `count` bytes remain.
    bool take(std::size_t count, std::string_view& out) noexcept;

private:
    const unsigned char* p_;
    std::size_t n_;
    std::size_t i_ = 0;
};

} // namespace zen::detail

#endif // ZEN_DETAIL_BINARY_HPP
