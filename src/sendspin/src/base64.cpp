#include "ac3/sendspin/base64.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac3::sendspin::base64 {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] constexpr int sextet(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(encoded_size(bytes.size()));
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t count = bytes.size() - i < 3 ? bytes.size() - i : 3;
        std::uint32_t group = std::uint32_t{bytes[i]} << 16U;
        if (count > 1) {
            group |= std::uint32_t{bytes[i + 1]} << 8U;
        }
        if (count > 2) {
            group |= bytes[i + 2];
        }
        out.push_back(kAlphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12U) & 0x3FU]);
        out.push_back(count > 1 ? kAlphabet[(group >> 6U) & 0x3FU] : '=');
        out.push_back(count > 2 ? kAlphabet[group & 0x3FU] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=') {
        padding = text[text.size() - 2] == '=' ? 2 : 1;
    }
    const std::string_view body = text.substr(0, text.size() - padding);
    std::vector<std::uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const char c : body) {
        const int value = sextet(c);
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(buffer >> bits));
            buffer &= (1U << bits) - 1U;
        }
    }
    // The unused tail of the last character must be zero.
    if (buffer != 0) {
        return std::nullopt;
    }
    return out;
}

}  // namespace ac3::sendspin::base64
