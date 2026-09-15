#include <psa/crypto.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>

#include "ac3/sendspin/crypto.hpp"

// The crypto seam over the PSA Crypto API: mbedTLS 3.6 from vcpkg on a computer,
// the mbedTLS 4 ESP-IDF bundles on a board. Only calls both provide are used.
//
// AeadKey stores the PSA key identifier as a uint32_t. That holds while mbedTLS
// is built without MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER, the default in both;
// the static_assert below says so if a configuration differs.

namespace ac3::sendspin::crypto {

namespace {

static_assert(std::is_same_v<mbedtls_svc_key_id_t, psa_key_id_t> &&
                  sizeof(psa_key_id_t) == sizeof(std::uint32_t),
              "AeadKey keeps a PSA key id in a uint32_t");

std::mutex& backend_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Called with the lock held.
[[nodiscard]] bool initialised() {
    static bool done = false;
    if (!done) {
        done = psa_crypto_init() == PSA_SUCCESS;
    }
    return done;
}

// A transient X25519 private key in the key store, destroyed on scope exit.
class TransientX25519 {
   public:
    explicit TransientX25519(const Key32& private_key) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_bits(&attributes, 255);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
        if (psa_import_key(&attributes, private_key.data(), private_key.size(), &id_) !=
            PSA_SUCCESS) {
            id_ = PSA_KEY_ID_NULL;
        }
        psa_reset_key_attributes(&attributes);
    }
    ~TransientX25519() {
        if (id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(id_);
        }
    }
    TransientX25519(const TransientX25519&) = delete;
    TransientX25519& operator=(const TransientX25519&) = delete;

    [[nodiscard]] bool valid() const { return id_ != PSA_KEY_ID_NULL; }
    [[nodiscard]] psa_key_id_t id() const { return id_; }

   private:
    psa_key_id_t id_ = PSA_KEY_ID_NULL;
};

template <class Digest>
[[nodiscard]] bool hash(psa_algorithm_t algorithm, std::span<const Bytes> parts, Digest& out) {
    const std::lock_guard lock(backend_mutex());
    if (!initialised()) {
        return false;
    }
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, algorithm) != PSA_SUCCESS) {
        return false;
    }
    for (const Bytes part : parts) {
        if (!part.empty() &&
            psa_hash_update(&operation, part.data(), part.size()) != PSA_SUCCESS) {
            psa_hash_abort(&operation);
            return false;
        }
    }
    std::size_t length = 0;
    if (psa_hash_finish(&operation, out.data(), out.size(), &length) != PSA_SUCCESS) {
        psa_hash_abort(&operation);
        return false;
    }
    return length == out.size();
}

[[nodiscard]] psa_algorithm_t algorithm_of(Aead aead) {
    return aead == Aead::kChaCha20Poly1305 ? PSA_ALG_CHACHA20_POLY1305 : PSA_ALG_GCM;
}

}  // namespace

bool random_bytes(std::span<std::uint8_t> out) {
    const std::lock_guard lock(backend_mutex());
    return initialised() && psa_generate_random(out.data(), out.size()) == PSA_SUCCESS;
}

bool x25519_public_key(const Key32& private_key, Key32& public_key) {
    const std::lock_guard lock(backend_mutex());
    if (!initialised()) {
        return false;
    }
    const TransientX25519 key(private_key);
    std::size_t length = 0;
    return key.valid() &&
           psa_export_public_key(key.id(), public_key.data(), public_key.size(), &length) ==
               PSA_SUCCESS &&
           length == public_key.size();
}

bool x25519(const Key32& private_key, const Key32& peer_public, Key32& shared) {
    const std::lock_guard lock(backend_mutex());
    if (!initialised()) {
        return false;
    }
    const TransientX25519 key(private_key);
    std::size_t length = 0;
    if (!key.valid() ||
        psa_raw_key_agreement(PSA_ALG_ECDH, key.id(), peer_public.data(), peer_public.size(),
                              shared.data(), shared.size(), &length) != PSA_SUCCESS ||
        length != shared.size()) {
        wipe(shared);
        return false;
    }
    // RFC 7748 §6.1: an all-zero result means a low-order point. mbedTLS refuses
    // those points itself; this check does not rely on it.
    std::uint8_t any = 0;
    for (const std::uint8_t byte : shared) {
        any |= byte;
    }
    return any != 0;
}

bool sha256(std::span<const Bytes> parts, Digest32& out) {
    return hash(PSA_ALG_SHA_256, parts, out);
}

bool sha512(std::span<const Bytes> parts, Digest64& out) {
    return hash(PSA_ALG_SHA_512, parts, out);
}

AeadKey::~AeadKey() { release(); }

AeadKey::AeadKey(AeadKey&& other) noexcept : handle_(other.handle_), aead_(other.aead_) {
    other.handle_ = 0;
}

AeadKey& AeadKey::operator=(AeadKey&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        aead_ = other.aead_;
        other.handle_ = 0;
    }
    return *this;
}

void AeadKey::release() {
    if (handle_ != 0) {
        const std::lock_guard lock(backend_mutex());
        psa_destroy_key(static_cast<psa_key_id_t>(handle_));
        handle_ = 0;
    }
}

std::optional<AeadKey> AeadKey::create(Aead aead, const Key32& key) {
    const std::lock_guard lock(backend_mutex());
    if (!initialised()) {
        return std::nullopt;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes,
                     aead == Aead::kChaCha20Poly1305 ? PSA_KEY_TYPE_CHACHA20 : PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, algorithm_of(aead));
    psa_key_id_t id = PSA_KEY_ID_NULL;
    const psa_status_t status = psa_import_key(&attributes, key.data(), key.size(), &id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return std::nullopt;
    }
    AeadKey result;
    result.handle_ = id;
    result.aead_ = aead;
    return result;
}

bool AeadKey::encrypt(std::span<const std::uint8_t, kAeadNonceBytes> nonce, Bytes ad,
                      Bytes plaintext, std::span<std::uint8_t> out) const {
    if (!valid() || out.size() < plaintext.size() + kAeadTagBytes) {
        return false;
    }
    const std::lock_guard lock(backend_mutex());
    std::size_t length = 0;
    return psa_aead_encrypt(static_cast<psa_key_id_t>(handle_), algorithm_of(aead_),
                            nonce.data(), nonce.size(), ad.data(), ad.size(), plaintext.data(),
                            plaintext.size(), out.data(), out.size(), &length) == PSA_SUCCESS &&
           length == plaintext.size() + kAeadTagBytes;
}

bool AeadKey::decrypt(std::span<const std::uint8_t, kAeadNonceBytes> nonce, Bytes ad,
                      Bytes ciphertext, std::span<std::uint8_t> out) const {
    if (!valid() || ciphertext.size() < kAeadTagBytes ||
        out.size() < ciphertext.size() - kAeadTagBytes) {
        return false;
    }
    const std::lock_guard lock(backend_mutex());
    std::size_t length = 0;
    return psa_aead_decrypt(static_cast<psa_key_id_t>(handle_), algorithm_of(aead_),
                            nonce.data(), nonce.size(), ad.data(), ad.size(), ciphertext.data(),
                            ciphertext.size(), out.data(), out.size(), &length) == PSA_SUCCESS &&
           length == ciphertext.size() - kAeadTagBytes;
}

}  // namespace ac3::sendspin::crypto
