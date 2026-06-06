#include "detail/base64.hpp"

#include <array>

namespace zen::detail {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Reverse lookup: ASCII -> 6-bit value, or 0xFF if not a base64 symbol.
std::array<std::uint8_t, 256> make_reverse() {
    std::array<std::uint8_t, 256> r{};
    r.fill(0xFF);
    for (std::uint8_t i = 0; i < 64; ++i) {
        r[static_cast<std::uint8_t>(kAlphabet[i])] = i;
    }
    return r;
}

const std::array<std::uint8_t, 256>& reverse_table() {
    static const std::array<std::uint8_t, 256> table = make_reverse();
    return table;
}

} // namespace

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
    }

    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool base64_decode(std::string_view in, std::vector<std::uint8_t>& out) {
    out.clear();
    if (in.size() % 4 != 0) {
        return false;
    }
    out.reserve(in.size() / 4 * 3);
    const auto& rev = reverse_table();

    for (std::size_t i = 0; i < in.size(); i += 4) {
        const char c0 = in[i];
        const char c1 = in[i + 1];
        const char c2 = in[i + 2];
        const char c3 = in[i + 3];

        // Padding may appear only in the final quad, in the last one or two slots.
        const bool pad2 = (c2 == '=');
        const bool pad3 = (c3 == '=');
        const bool last_quad = (i + 4 == in.size());
        if ((pad2 || pad3) && !last_quad) {
            return false;
        }
        if (pad2 && !pad3) {
            return false; // "x=y" is illegal; padding fills from the right
        }

        const std::uint8_t v0 = rev[static_cast<std::uint8_t>(c0)];
        const std::uint8_t v1 = rev[static_cast<std::uint8_t>(c1)];
        if (v0 == 0xFF || v1 == 0xFF) {
            return false;
        }

        if (pad3 && pad2) { // one output byte
            out.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
            // Bits that would belong to the dropped byte must be zero.
            if ((v1 & 0x0F) != 0) {
                return false;
            }
            continue;
        }

        const std::uint8_t v2 = rev[static_cast<std::uint8_t>(c2)];
        if (v2 == 0xFF) {
            return false;
        }
        if (pad3) { // two output bytes
            out.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
            out.push_back(static_cast<std::uint8_t>((v1 << 4) | (v2 >> 2)));
            if ((v2 & 0x03) != 0) {
                return false;
            }
            continue;
        }

        const std::uint8_t v3 = rev[static_cast<std::uint8_t>(c3)];
        if (v3 == 0xFF) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<std::uint8_t>((v1 << 4) | (v2 >> 2)));
        out.push_back(static_cast<std::uint8_t>((v2 << 6) | v3));
    }
    return true;
}

} // namespace zen::detail
