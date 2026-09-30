#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RFC 4648 §4 Base64 with padding: how player@v1 writes `codec_header`
// (roles/player/v1.md). Sendspin's identities and keys use base64url.hpp instead.
//
// Decoding is strict in the same way as base64url's: a length that is a multiple of four,
// padding only at the end and only as much as the last group needs, nothing outside the
// alphabet, and zero bits past the last whole byte.

namespace ac3::sendspin::base64 {

// Characters for `bytes` bytes: 4 * ceil(n / 3).
[[nodiscard]] constexpr std::size_t encoded_size(std::size_t bytes) {
    return ((bytes + 2) / 3) * 4;
}

[[nodiscard]] std::string encode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode(std::string_view text);

}  // namespace ac3::sendspin::base64
