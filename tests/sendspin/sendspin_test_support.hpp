#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Hex helpers for the Sendspin tests' byte-for-byte vectors.

namespace ac3::sendspin::test {

inline std::vector<std::uint8_t> from_hex(std::string_view hex) {
    const auto nibble = [](char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[(2 * i) + 1]));
    }
    return out;
}

inline std::array<std::uint8_t, 32> key_from_hex(std::string_view hex) {
    std::array<std::uint8_t, 32> key{};
    const std::vector<std::uint8_t> bytes = from_hex(hex);
    for (std::size_t i = 0; i < key.size() && i < bytes.size(); ++i) {
        key[i] = bytes[i];
    }
    return key;
}

inline std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4U]);
        out.push_back(kDigits[b & 0x0FU]);
    }
    return out;
}

inline std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

}  // namespace ac3::sendspin::test
