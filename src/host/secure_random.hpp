// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_SECURE_RANDOM_HPP
#define ZEN_HOST_SECURE_RANDOM_HPP

// THE TWO SECRETS A SESSION MINTS, AND HOW THEY ARE COMPARED.
//
// A session host mints a CLIENT KEY when it starts (written to a file only its owner should read)
// and a run manager mints a ONE-TIME CREDENTIAL for each worker it starts. Both come from the
// operating system's own generator -- `rand_s` (RtlGenRandom) on Windows, `/dev/urandom` elsewhere
// -- never from `std::random_device`, whose quality is the standard library's to choose and has
// been deterministic on some MinGW toolchains. A failure to read randomness is a failure to mint,
// said out loud, never a weak key.
//
// WHAT THEY ARE, HONESTLY: shared secrets on one machine, as private as the files they live in and
// the processes that hold them. They are not transport security (the bridge has none, and a
// session listens on loopback only) and they authenticate no person.

#ifdef _WIN32
// Declared here rather than by defining _CRT_RAND_S, which only works when it precedes the
// translation unit's FIRST <stdlib.h> -- a fact about include order no header can promise. The
// function itself is the C runtime's (RtlGenRandom underneath), on MinGW-w64 and MSVC alike.
extern "C" int __cdecl rand_s(unsigned int* random_value);
#else
#include <cstdio>
#endif

#include "../detail/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loom::host {

/// `bytes` bytes of OS randomness as lowercase hex, or an empty string when the OS would not
/// give any (the caller refuses to mint rather than using a weaker source).
inline std::string secure_random_hex(std::size_t bytes) {
    static const char* const hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
#ifdef _WIN32
    std::size_t produced = 0;
    while (produced < bytes) {
        unsigned int word = 0;
        if (rand_s(&word) != 0) {
            return {};
        }
        for (int i = 0; i < 4 && produced < bytes; ++i, ++produced) {
            const unsigned b = (word >> (i * 8)) & 0xFFu;
            out.push_back(hex[(b >> 4) & 0xF]);
            out.push_back(hex[b & 0xF]);
        }
    }
#else
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) {
        return {};
    }
    for (std::size_t i = 0; i < bytes; ++i) {
        const int c = std::fgetc(f);
        if (c == EOF) {
            std::fclose(f);
            return {};
        }
        const unsigned b = static_cast<unsigned>(c) & 0xFFu;
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    std::fclose(f);
#endif
    return out;
}

/// Equal, without an early exit that would say how much of a guess was right.
inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

/// The SHA-256 of a credential, as the 64 hex characters a run manager tells the door. The bus
/// (and so the host's history) carries this, never the credential.
inline std::string credential_digest(std::string_view credential) {
    return loom::detail::sha256_hex_prefix(credential, 32);
}

} // namespace loom::host

#endif // ZEN_HOST_SECURE_RANDOM_HPP
