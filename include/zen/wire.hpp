// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WIRE_HPP
#define ZEN_WIRE_HPP

// THE WIRE PRIMITIVES TWO PROTOCOLS SHARE: little-endian integer and length-prefixed byte
// writers, a bounds-checked reader, the per-frame cap, and the two send kinds. Portable,
// header-only, POSIX-free.
//
// They were born in the isolation protocol header and the bridge protocol borrowed them from
// there, with a note that a neutral home was a clean future factoring. The two-host crossing is
// the trigger: `loom::bridge` is exported and the isolation protocol is not, so the bridge's
// header could no longer include the isolation's and remain self-contained in the installed
// package. This file is that home; both protocol headers include it, and neither includes the
// other.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loom {

inline constexpr std::uint32_t kMaxFrameLen = 64u * 1024u * 1024u; ///< hard cap per frame
inline constexpr std::uint8_t kEmitSend = 0;
inline constexpr std::uint8_t kEmitPublish = 1;

// ---- little-endian append helpers -----------------------------------------

inline void put_u8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
inline void put_u32(std::string& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<char>((v >> s) & 0xFFU));
    }
}
inline void put_u64(std::string& out, std::uint64_t v) {
    for (int s = 0; s < 64; s += 8) {
        out.push_back(static_cast<char>((v >> s) & 0xFFU));
    }
}
inline void put_bytes(std::string& out, std::string_view b) {
    put_u32(out, static_cast<std::uint32_t>(b.size()));
    out.append(b);
}

// ---- a bounds-checked reader over a frame payload -------------------------

class Cursor {
public:
    explicit Cursor(std::string_view data) noexcept : data_(data) {}

    bool u8(std::uint8_t& v) noexcept {
        if (remaining() < 1) {
            return false;
        }
        v = static_cast<std::uint8_t>(data_[i_++]);
        return true;
    }
    bool u32(std::uint32_t& v) noexcept {
        if (remaining() < 4) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(data_[i_++])) << s;
        }
        return true;
    }
    bool u64(std::uint64_t& v) noexcept {
        if (remaining() < 8) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 64; s += 8) {
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(data_[i_++])) << s;
        }
        return true;
    }
    bool bytes(std::string_view& v) noexcept {
        std::uint32_t n = 0;
        if (!u32(n) || remaining() < n) {
            return false;
        }
        v = data_.substr(i_, n);
        i_ += n;
        return true;
    }
    std::string_view rest() noexcept {
        std::string_view r = data_.substr(i_);
        i_ = data_.size();
        return r;
    }
    std::size_t remaining() const noexcept { return data_.size() - i_; }

private:
    std::string_view data_;
    std::size_t i_ = 0;
};

} // namespace loom

#endif // ZEN_WIRE_HPP
