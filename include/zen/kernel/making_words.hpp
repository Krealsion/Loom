// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_MAKING_WORDS_HPP
#define ZEN_KERNEL_MAKING_WORDS_HPP

// What a weave says when it refuses its making: the sink a host hands `ZenWeaveAbi::create`, kept to
// a bound, and the load's refusal that says it. Header-only, so the kernel and the isolated host's
// child, which links nothing of Loom, keep and say the words alike.

#include <zen/kernel/abi.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace loom {

/// How many bytes of a refused making's words a host keeps; the rest is cut, and the refusal says so.
/// docs/reference/bounds.md
inline constexpr std::size_t kMaxMakingWordsBytes = 1024;

/// The words a weave wrote while refusing its making: the first `kMaxMakingWordsBytes`, and whether
/// it wrote more. Filled through `sink()`, which never throws into the library's frames.
struct MakingWords {
    std::string said;
    bool cut = false;

    ZenByteSink sink() { return ZenByteSink{this, &MakingWords::write}; }

    static void write(void* ctx, const std::uint8_t* data, std::size_t len) {
        auto* words = static_cast<MakingWords*>(ctx);
        if (data == nullptr) {
            return;
        }
        const std::size_t room = kMaxMakingWordsBytes - words->said.size();
        if (len > room) {
            words->cut = true;
            len = room;
        }
        try {
            words->said.append(reinterpret_cast<const char*>(data), len);
        } catch (...) {
            words->cut = true;
        }
    }
};

/// The load's refusal for a making refused: in the weave's words when it wrote any, else that
/// `create` made nothing.
inline std::string making_refused(const MakingWords& words) {
    if (words.said.empty()) {
        return words.cut ? std::string("library create() refused, and its words could not be kept")
                         : std::string("library create() returned null");
    }
    std::string refusal = "library create() refused: " + words.said;
    if (words.cut) {
        refusal += " (cut at " + std::to_string(kMaxMakingWordsBytes) + " bytes)";
    }
    return refusal;
}

} // namespace loom

#endif // ZEN_KERNEL_MAKING_WORDS_HPP
