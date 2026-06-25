#ifndef ZEN_DETAIL_BASE64_HPP
#define ZEN_DETAIL_BASE64_HPP

// Internal to loom. Standard base64 (RFC 4648, '+' '/' alphabet, '='
// padding) for the Bytes kind. Decoding is strict and total: any malformed
// input is rejected, never crashes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace loom::detail {

std::string base64_encode(const std::vector<std::uint8_t>& data);

/// Strict decode. Returns true and fills `out` on success; returns false and
/// leaves `out` unspecified on any malformed input (bad length, illegal
/// character, misplaced padding).
bool base64_decode(std::string_view in, std::vector<std::uint8_t>& out);

} // namespace loom::detail

#endif // ZEN_DETAIL_BASE64_HPP
