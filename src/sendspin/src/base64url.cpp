#include "ac3/sendspin/base64url.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac3::sendspin::base64url {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

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
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

// Bytes `text` decodes to, or nothing when its length cannot occur.
[[nodiscard]] std::optional<std::size_t> decoded_size(std::size_t characters) {
    if (characters % 4 == 1) {
        return std::nullopt;
    }
    return (characters * 3) / 4;
}

[[nodiscard]] bool decode_into(std::string_view text, std::span<std::uint8_t> out) {
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    std::size_t written = 0;
    for (const char c : text) {
        const int value = sextet(c);
        if (value < 0) {
            return false;
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = static_cast<std::uint8_t>(buffer >> bits);
            buffer &= (1U << bits) - 1U;
        }
    }
    // Whatever is left over is the unused tail of the final character.
    return buffer == 0;
}

}  // namespace

void encode_to(std::span<const std::uint8_t> bytes, std::string& out) {
    out.reserve(out.size() + encoded_size(bytes.size()));
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t group = (std::uint32_t{bytes[i]} << 16U) |
                                    (std::uint32_t{bytes[i + 1]} << 8U) | bytes[i + 2];
        out.push_back(kAlphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[group & 0x3FU]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t group = std::uint32_t{bytes[i]} << 16U;
        out.push_back(kAlphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12U) & 0x3FU]);
    } else if (rest == 2) {
        const std::uint32_t group = (std::uint32_t{bytes[i]} << 16U) |
                                    (std::uint32_t{bytes[i + 1]} << 8U);
        out.push_back(kAlphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6U) & 0x3FU]);
    }
}

std::string encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    encode_to(bytes, out);
    return out;
}

std::optional<std::vector<std::uint8_t>> decode(std::string_view text) {
    const std::optional<std::size_t> size = decoded_size(text.size());
    if (!size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(*size);
    if (!decode_into(text, out)) {
        return std::nullopt;
    }
    return out;
}

bool decode_exact(std::string_view text, std::span<std::uint8_t> out) {
    const std::optional<std::size_t> size = decoded_size(text.size());
    return size && *size == out.size() && decode_into(text, out);
}

}  // namespace ac3::sendspin::base64url
