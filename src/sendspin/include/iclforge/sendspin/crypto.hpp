#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>

// The cryptography Sendspin needs, as a seam: X25519, SHA-256, SHA-512, the two
// AEADs of its Noise suites, and randomness. One backend exists,
// src/crypto/psa/, over the PSA Crypto API, which both the mbedTLS 3.6 vcpkg
// supplies and the mbedTLS 4 ESP-IDF bundles provide; a second backend would be
// a sibling directory, chosen by CMake, never a preprocessor branch.
//
// HMAC and HKDF are built here over the hashes (hmac.cpp), so a backend supplies
// only primitives and nothing keyed but the AEAD.
//
// Every backend call is serialised behind one mutex: mbedTLS's PSA key store is
// not thread-safe unless the library is built with threading, which vcpkg's
// default port is not, and Sendspin's rates (a chunk every 32 ms per connection)
// make the lock's cost irrelevant.

namespace ac3::sendspin::crypto {

using Key32 = std::array<std::uint8_t, 32>;
using Digest32 = std::array<std::uint8_t, 32>;
using Digest64 = std::array<std::uint8_t, 64>;

inline constexpr std::size_t kAeadTagBytes = 16;
inline constexpr std::size_t kAeadNonceBytes = 12;

using Bytes = std::span<const std::uint8_t>;

// Overwrites `bytes` with zeros in a way the optimiser keeps.
void wipe(std::span<std::uint8_t> bytes);

// Fills `out` from the platform's CSPRNG.
[[nodiscard]] bool random_bytes(std::span<std::uint8_t> out);

// RFC 7748 X25519. `private_key` is clamped as the RFC requires; the peer's
// public value has its top bit ignored.
[[nodiscard]] bool x25519_public_key(const Key32& private_key, Key32& public_key);
// False when the result would be the all-zero value of a low-order peer point,
// or the backend fails.
[[nodiscard]] bool x25519(const Key32& private_key, const Key32& peer_public, Key32& shared);

// Hashes of the concatenation of `parts`.
[[nodiscard]] bool sha256(std::span<const Bytes> parts, Digest32& out);
[[nodiscard]] bool sha512(std::span<const Bytes> parts, Digest64& out);

// RFC 2104 HMAC over the concatenation of `parts`, at most kMaxHmacParts of them.
inline constexpr std::size_t kMaxHmacParts = 7;
[[nodiscard]] bool hmac_sha256(Bytes key, std::span<const Bytes> parts, Digest32& out);
[[nodiscard]] bool hmac_sha512(Bytes key, std::span<const Bytes> parts, Digest64& out);

[[nodiscard]] inline bool sha256(std::initializer_list<Bytes> parts, Digest32& out) {
    return sha256(std::span<const Bytes>(parts.begin(), parts.size()), out);
}
[[nodiscard]] inline bool sha512(std::initializer_list<Bytes> parts, Digest64& out) {
    return sha512(std::span<const Bytes>(parts.begin(), parts.size()), out);
}
[[nodiscard]] inline bool hmac_sha256(Bytes key, std::initializer_list<Bytes> parts,
                                      Digest32& out) {
    return hmac_sha256(key, std::span<const Bytes>(parts.begin(), parts.size()), out);
}
[[nodiscard]] inline bool hmac_sha512(Bytes key, std::initializer_list<Bytes> parts,
                                      Digest64& out) {
    return hmac_sha512(key, std::span<const Bytes>(parts.begin(), parts.size()), out);
}

enum class Aead : std::uint8_t {
    kChaCha20Poly1305,
    kAes256Gcm,
};

// One AEAD key held by the backend, for as many operations as its owner needs.
// Move-only; the backend's copy is destroyed with it.
class AeadKey {
   public:
    AeadKey() = default;
    ~AeadKey();
    AeadKey(const AeadKey&) = delete;
    AeadKey& operator=(const AeadKey&) = delete;
    AeadKey(AeadKey&& other) noexcept;
    AeadKey& operator=(AeadKey&& other) noexcept;

    [[nodiscard]] static std::optional<AeadKey> create(Aead aead, const Key32& key);

    [[nodiscard]] bool valid() const { return handle_ != 0; }

    // `out` holds plaintext.size() + kAeadTagBytes bytes: the ciphertext then the
    // tag. `out` must not overlap `plaintext`.
    [[nodiscard]] bool encrypt(std::span<const std::uint8_t, kAeadNonceBytes> nonce, Bytes ad,
                               Bytes plaintext, std::span<std::uint8_t> out) const;
    // `out` holds ciphertext.size() - kAeadTagBytes bytes. False when the tag does
    // not verify, in which case `out` holds nothing to use.
    [[nodiscard]] bool decrypt(std::span<const std::uint8_t, kAeadNonceBytes> nonce, Bytes ad,
                               Bytes ciphertext, std::span<std::uint8_t> out) const;

   private:
    void release();

    std::uint32_t handle_ = 0;
    Aead aead_ = Aead::kChaCha20Poly1305;
};

}  // namespace ac3::sendspin::crypto
