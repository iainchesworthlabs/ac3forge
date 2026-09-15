#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/pairing.hpp"

// Pairing tokens (pairing.md, Pairing Token): RFC 4648 base32 without padding,
// with 2 transliterated to 9 so a token needs only QR alphanumeric characters.
// Needs no crypto, so it builds in the dependency-free core.

namespace ac3::sendspin::pairing {

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::string_view kPrefix = "SP:";

[[nodiscard]] int base32_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '2' && c <= '7') {
        return c - '2' + 26;
    }
    return -1;
}

[[nodiscard]] std::size_t payload_bytes(TokenVersion version) {
    return version == TokenVersion::kPairingPsk ? 64 : 24;
}

[[nodiscard]] bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

}  // namespace

std::string encode_token(TokenVersion version, Bytes payload) {
    std::string out(kPrefix);
    out.push_back(static_cast<char>(version));
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const std::uint8_t byte : payload) {
        buffer = (buffer << 8U) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(buffer >> bits) & 0x1FU]);
        }
        buffer &= (1U << bits) - 1U;
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1FU]);
    }
    std::replace(out.begin() + static_cast<std::ptrdiff_t>(kPrefix.size() + 1), out.end(), '2',
                 '9');
    return out;
}

std::optional<Token> decode_token(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    std::string_view rest = upper;
    if (rest.starts_with(kPrefix)) {
        rest.remove_prefix(kPrefix.size());
    }
    if (rest.empty() || (rest.front() != '0' && rest.front() != '1')) {
        return std::nullopt;
    }
    Token token;
    token.version = static_cast<TokenVersion>(rest.front());
    rest.remove_prefix(1);
    while (!rest.empty() && rest.back() == '=') {
        rest.remove_suffix(1);
    }
    // A base32 text without padding has a length of 0, 2, 4, 5 or 7 modulo 8.
    const std::size_t tail = rest.size() % 8;
    if (tail == 1 || tail == 3 || tail == 6) {
        return std::nullopt;
    }
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (char c : rest) {
        if (c == '9') {
            c = '2';
        }
        const int value = base32_value(c);
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 5U) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            token.payload.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFU));
        }
        buffer &= (1U << bits) - 1U;
    }
    if (token.payload.size() < payload_bytes(token.version)) {
        return std::nullopt;
    }
    return token;
}

std::optional<PairingPskToken> decode_pairing_psk_token(std::string_view text) {
    const std::optional<Token> token = decode_token(text);
    if (!token || token->version != TokenVersion::kPairingPsk) {
        return std::nullopt;
    }
    PairingPskToken out;
    std::copy_n(token->payload.begin(), 32, out.client_key.begin());
    std::copy_n(token->payload.begin() + 32, 32, out.pairing_psk.begin());
    return out;
}

}  // namespace ac3::sendspin::pairing
