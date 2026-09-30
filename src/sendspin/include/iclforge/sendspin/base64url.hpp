#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RFC 4648 §5 base64url without padding: how Sendspin writes identities
// (client_id, server_id), psk_id and Noise handshake messages.
//
// Decoding is strict, so every byte string has exactly one accepted text: no
// padding, no whitespace, nothing outside the alphabet, no length of 1 mod 4, and
// the bits a final character carries beyond the last whole byte must be zero. A
// client_id compared as text then agrees with the key compared as bytes.

namespace ac3::sendspin::base64url {

// Characters for `bytes` bytes: ceil(4n / 3).
[[nodiscard]] constexpr std::size_t encoded_size(std::size_t bytes) {
    return ((bytes * 4) + 2) / 3;
}

void encode_to(std::span<const std::uint8_t> bytes, std::string& out);
[[nodiscard]] std::string encode(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode(std::string_view text);

// Decodes into `out`; true only when `text` is valid and holds exactly out.size()
// bytes. For fixed-size values such as a 32-byte key.
[[nodiscard]] bool decode_exact(std::string_view text, std::span<std::uint8_t> out);

}  // namespace ac3::sendspin::base64url
