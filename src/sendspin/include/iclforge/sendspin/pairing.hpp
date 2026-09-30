#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/noise.hpp"

// The values Sendspin's pairing flows compute around CPace (pairing.md): pairing
// tokens, the dynamic pairing code, the commitment to nonce_B, the CPace session
// id and the wrapping of the long-term PSK and nonce_B.
//
// Where a value differs by dialect (dialect.hpp), its function takes one: aiosendspin
// 9.1.1, which Music Assistant speaks, differs from the specification in the code's
// derivation label and length, the session id (no round) and what it wraps
// (planning/hearth-sendspin-extension.md, C21 to C23).

namespace ac3::sendspin::pairing {

using crypto::Bytes;
using crypto::Digest32;
using crypto::Digest64;
using crypto::Key32;

// --- Pairing tokens (pairing.md, Pairing Token) -------------------------------

enum class TokenVersion : char {
    kPairingPsk = '0',   // client_key (32) || pairing_psk (32)
    kDynamicCode = '1',  // the 24-byte qr_code pairing code
};

// "SP:" || version || base32 without padding, with 2 transliterated to 9.
[[nodiscard]] std::string encode_token(TokenVersion version, Bytes payload);

struct Token {
    TokenVersion version = TokenVersion::kPairingPsk;
    std::vector<std::uint8_t> payload;
};

// Decodes operator input leniently, as the specification requires: surrounding
// whitespace and case are ignored and the SP: prefix is optional. Refuses an
// unknown version, characters outside the alphabet, and a payload shorter than
// its version defines; bytes past that are kept for the caller to ignore.
[[nodiscard]] std::optional<Token> decode_token(std::string_view text);

struct PairingPskToken {
    Key32 client_key{};
    Key32 pairing_psk{};
};

[[nodiscard]] std::optional<PairingPskToken> decode_pairing_psk_token(std::string_view text);

// --- The dynamic pairing code ----------------------------------------------------

// SHA-256("sendspin-pair-commit-v1" || nonce_B), the same in both dialects.
[[nodiscard]] std::optional<Digest32> commit(const Key32& nonce_b);

inline constexpr int kSpecificationCodeDigits = 6;
inline constexpr int kMinimumCodeDigits = 4;
inline constexpr int kMaximumCodeDigits = 12;

// The digits the client emits and the operator enters: SHA-256(label || h ||
// nonce_A || nonce_B) as a big-endian integer modulo 10^digits, zero-padded. The
// specification uses "sendspin-pairing-code-derive-v1" and six digits; aiosendspin
// 9.1.1 uses "sendspin-pin-derive-v1" and the pin_length its server chose, from 4
// to 12. Nothing for a length outside that range, or six in the specification's
// dialect.
[[nodiscard]] std::optional<std::string> derive_digits(Dialect dialect, const Digest32& h,
                                                       const Key32& nonce_a,
                                                       const Key32& nonce_b, int digits);

// The specification's qr_code format: the first 24 bytes of the same digest.
[[nodiscard]] std::optional<std::array<std::uint8_t, 24>> derive_qr_code(const Digest32& h,
                                                                        const Key32& nonce_a,
                                                                        const Key32& nonce_b);

// --- CPace inputs and wrapping ---------------------------------------------------

inline constexpr std::string_view kServerAd = "server";
inline constexpr std::string_view kClientAd = "client";

// "sendspin-pair-pake-v1" || h || u32be(pairing_index), then || u32be(round) in the
// specification's dialect only.
[[nodiscard]] std::vector<std::uint8_t> pake_sid(Dialect dialect, const Digest32& h,
                                                 std::uint32_t pairing_index,
                                                 std::uint32_t round);

enum class Wrapped : std::uint8_t {
    kLongTermPsk,  // "sendspin-pair-psk-wrap-v1"
    kNonceB,       // "sendspin-pair-nonce-wrap-v1", the specification's dialect only
};

// K_wrap = SHA-256(label || sid || ISK).
[[nodiscard]] std::optional<Key32> wrap_key(Wrapped what, Bytes sid, const Digest64& isk);

inline constexpr std::size_t kWrappedBytes = 48;

// The connection suite's AEAD under K_wrap, an all-zero nonce and no associated data.
[[nodiscard]] std::optional<std::array<std::uint8_t, kWrappedBytes>> wrap(noise::Suite suite,
                                                                          const Key32& key,
                                                                          const Key32& value);
[[nodiscard]] std::optional<Key32> unwrap(noise::Suite suite, const Key32& key, Bytes wrapped);

}  // namespace ac3::sendspin::pairing
