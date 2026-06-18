#ifndef ZEN_ISOLATION_PROTOCOL_HPP
#define ZEN_ISOLATION_PROTOCOL_HPP

// The parent<->child wire protocol for out-of-process Shard hosting. Frames are
// length-prefixed: [u32 payload_len][u8 op][payload]. The payload sub-fields are
// little-endian and read through a bounds-checked Cursor, so a hostile or
// truncated frame is rejected, never over-read.
//
// Zen's serialized values are the IPC currency (exactly as for persistence and
// the DLL boundary): every Value/message/snapshot/policy crosses as bytes and is
// re-admitted host-side through the one gate.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zen::isolation {

enum class Op : std::uint8_t {
    // child -> parent
    Hello = 1,    ///< [bytes manifest][bytes policy][bytes snapshot] (the handshake)
    Emit = 2,     ///< [u8 kind][u64 target][u64 reply_to][u64 correlation][bytes payload]
    Snapshot = 3, ///< [bytes snapshot] (refreshed state, after each handle/revive)
    // parent -> child
    Deliver = 16,  ///< [u64 sender][u64 reply_to][u64 correlation][bytes payload]
    Revive = 17,   ///< [bytes state]
    Shutdown = 18, ///< (empty)
};

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

} // namespace zen::isolation

#endif // ZEN_ISOLATION_PROTOCOL_HPP
