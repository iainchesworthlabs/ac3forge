#include "ac3/sendspin/pairing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3::sendspin::pairing {

namespace {

[[nodiscard]] Bytes bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

[[nodiscard]] std::optional<Digest32> code_digest(std::string_view label, const Digest32& h,
                                                  const Key32& nonce_a, const Key32& nonce_b) {
    Digest32 digest{};
    if (!crypto::sha256({bytes_of(label), Bytes(h), Bytes(nonce_a), Bytes(nonce_b)}, digest)) {
        return std::nullopt;
    }
    return digest;
}

void append_u32be(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

[[nodiscard]] crypto::Aead aead_of(noise::Suite suite) {
    return suite == noise::Suite::kChaChaPolySha256 ? crypto::Aead::kChaCha20Poly1305
                                                    : crypto::Aead::kAes256Gcm;
}

constexpr std::array<std::uint8_t, crypto::kAeadNonceBytes> kZeroNonce{};

}  // namespace

std::optional<Digest32> commit(const Key32& nonce_b) {
    Digest32 out{};
    if (!crypto::sha256({bytes_of("sendspin-pair-commit-v1"), Bytes(nonce_b)}, out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> derive_digits(Dialect dialect, const Digest32& h,
                                         const Key32& nonce_a, const Key32& nonce_b,
                                         int digits) {
    const bool specification = dialect == Dialect::kSpecification;
    if (specification ? digits != kSpecificationCodeDigits
                      : (digits < kMinimumCodeDigits || digits > kMaximumCodeDigits)) {
        return std::nullopt;
    }
    const std::optional<Digest32> digest =
        code_digest(specification ? "sendspin-pairing-code-derive-v1" : "sendspin-pin-derive-v1",
                    h, nonce_a, nonce_b);
    if (!digest) {
        return std::nullopt;
    }
    std::uint64_t modulus = 1;
    for (int i = 0; i < digits; ++i) {
        modulus *= 10;
    }
    // The digest as a big-endian integer, reduced a byte at a time; below 10^12 * 256
    // every intermediate fits in 64 bits.
    std::uint64_t remainder = 0;
    for (const std::uint8_t byte : *digest) {
        remainder = ((remainder * 256U) + byte) % modulus;
    }
    std::string code(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0 && remainder > 0; --i) {
        code[static_cast<std::size_t>(i)] = static_cast<char>('0' + (remainder % 10));
        remainder /= 10;
    }
    return code;
}

std::optional<std::array<std::uint8_t, 24>> derive_qr_code(const Digest32& h,
                                                           const Key32& nonce_a,
                                                           const Key32& nonce_b) {
    const std::optional<Digest32> digest =
        code_digest("sendspin-pairing-code-derive-v1", h, nonce_a, nonce_b);
    if (!digest) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 24> code{};
    std::copy_n(digest->begin(), code.size(), code.begin());
    return code;
}

std::vector<std::uint8_t> pake_sid(Dialect dialect, const Digest32& h,
                                   std::uint32_t pairing_index, std::uint32_t round) {
    const Bytes label = bytes_of("sendspin-pair-pake-v1");
    std::vector<std::uint8_t> sid(label.begin(), label.end());
    sid.insert(sid.end(), h.begin(), h.end());
    append_u32be(sid, pairing_index);
    if (dialect == Dialect::kSpecification) {
        append_u32be(sid, round);
    }
    return sid;
}

std::optional<Key32> wrap_key(Wrapped what, Bytes sid, const Digest64& isk) {
    const std::string_view label = what == Wrapped::kLongTermPsk ? "sendspin-pair-psk-wrap-v1"
                                                                 : "sendspin-pair-nonce-wrap-v1";
    Key32 key{};
    if (!crypto::sha256({bytes_of(label), sid, Bytes(isk)}, key)) {
        return std::nullopt;
    }
    return key;
}

std::optional<std::array<std::uint8_t, kWrappedBytes>> wrap(noise::Suite suite, const Key32& key,
                                                             const Key32& value) {
    const std::optional<crypto::AeadKey> aead = crypto::AeadKey::create(aead_of(suite), key);
    std::array<std::uint8_t, kWrappedBytes> out{};
    if (!aead || !aead->encrypt(kZeroNonce, {}, value, out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<Key32> unwrap(noise::Suite suite, const Key32& key, Bytes wrapped) {
    if (wrapped.size() != kWrappedBytes) {
        return std::nullopt;
    }
    const std::optional<crypto::AeadKey> aead = crypto::AeadKey::create(aead_of(suite), key);
    Key32 value{};
    if (!aead || !aead->decrypt(kZeroNonce, {}, wrapped, value)) {
        crypto::wipe(value);
        return std::nullopt;
    }
    return value;
}

}  // namespace ac3::sendspin::pairing
