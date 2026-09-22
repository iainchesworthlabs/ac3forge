#include "ac3/sendspin/noise.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"

namespace ac3::sendspin::noise {

namespace {

using Bytes = std::span<const std::uint8_t>;

constexpr std::size_t kHashBytes = 32;

[[nodiscard]] crypto::Aead aead_of(Suite suite) {
    return suite == Suite::kChaChaPolySha256 ? crypto::Aead::kChaCha20Poly1305
                                             : crypto::Aead::kAes256Gcm;
}

// Noise §12.2 and §12.3: 32 zero bits, then the 64-bit nonce little-endian for
// ChaChaPoly and big-endian for AESGCM.
[[nodiscard]] std::array<std::uint8_t, crypto::kAeadNonceBytes> nonce_bytes(Suite suite,
                                                                            std::uint64_t n) {
    std::array<std::uint8_t, crypto::kAeadNonceBytes> nonce{};
    for (std::size_t i = 0; i < 8; ++i) {
        const auto byte = static_cast<std::uint8_t>((n >> (8U * i)) & 0xFFU);
        nonce[suite == Suite::kChaChaPolySha256 ? 4 + i : 11 - i] = byte;
    }
    return nonce;
}

// Noise §4.3 HKDF with SHA-256: two or three outputs.
[[nodiscard]] bool hkdf(const Digest32& chaining_key, Bytes input, Digest32& first,
                        Digest32& second, Digest32* third) {
    Digest32 temp{};
    static constexpr std::array<std::uint8_t, 3> kCounters{0x01, 0x02, 0x03};
    bool ok = crypto::hmac_sha256(chaining_key, {input}, temp) &&
              crypto::hmac_sha256(temp, {Bytes(kCounters.data(), 1)}, first) &&
              crypto::hmac_sha256(temp, {Bytes(first), Bytes(kCounters.data() + 1, 1)}, second);
    if (ok && third != nullptr) {
        ok = crypto::hmac_sha256(temp, {Bytes(second), Bytes(kCounters.data() + 2, 1)}, *third);
    }
    crypto::wipe(temp);
    return ok;
}

[[nodiscard]] bool encrypt_with(Suite suite, const Key32& key, std::uint64_t n, Bytes ad,
                                Bytes plaintext, std::vector<std::uint8_t>& out) {
    if (n == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const std::optional<crypto::AeadKey> aead = crypto::AeadKey::create(aead_of(suite), key);
    if (!aead) {
        return false;
    }
    const std::size_t start = out.size();
    out.resize(start + plaintext.size() + crypto::kAeadTagBytes);
    const auto nonce = nonce_bytes(suite, n);
    if (!aead->encrypt(nonce, ad, plaintext, std::span<std::uint8_t>(out).subspan(start))) {
        out.resize(start);
        return false;
    }
    return true;
}

[[nodiscard]] bool decrypt_with(Suite suite, const Key32& key, std::uint64_t n, Bytes ad,
                                Bytes ciphertext, std::vector<std::uint8_t>& out) {
    if (n == std::numeric_limits<std::uint64_t>::max() ||
        ciphertext.size() < crypto::kAeadTagBytes) {
        return false;
    }
    const std::optional<crypto::AeadKey> aead = crypto::AeadKey::create(aead_of(suite), key);
    if (!aead) {
        return false;
    }
    const std::size_t start = out.size();
    out.resize(start + ciphertext.size() - crypto::kAeadTagBytes);
    const auto nonce = nonce_bytes(suite, n);
    if (!aead->decrypt(nonce, ad, ciphertext, std::span<std::uint8_t>(out).subspan(start))) {
        crypto::wipe(std::span<std::uint8_t>(out).subspan(start));
        out.resize(start);
        return false;
    }
    return true;
}

}  // namespace

// --- KeyPair ------------------------------------------------------------------

KeyPair::~KeyPair() { crypto::wipe(private_); }

std::optional<KeyPair> KeyPair::generate() {
    Key32 secret{};
    if (!crypto::random_bytes(secret)) {
        return std::nullopt;
    }
    std::optional<KeyPair> pair = from_private(secret);
    crypto::wipe(secret);
    return pair;
}

std::optional<KeyPair> KeyPair::from_private(const Key32& private_key) {
    KeyPair pair;
    pair.private_ = private_key;
    if (!crypto::x25519_public_key(pair.private_, pair.public_)) {
        return std::nullopt;
    }
    return pair;
}

// --- CipherState --------------------------------------------------------------

std::optional<CipherState> CipherState::create(Suite suite, const Key32& key) {
    std::optional<crypto::AeadKey> aead = crypto::AeadKey::create(aead_of(suite), key);
    if (!aead) {
        return std::nullopt;
    }
    CipherState state;
    state.key_ = std::move(*aead);
    state.suite_ = suite;
    return state;
}

bool CipherState::encrypt(std::span<const std::uint8_t> plaintext,
                          std::vector<std::uint8_t>& out) {
    if (!valid() || nonce_ == std::numeric_limits<std::uint64_t>::max() ||
        plaintext.size() + crypto::kAeadTagBytes > kMaxMessageBytes) {
        return false;
    }
    const std::size_t start = out.size();
    out.resize(start + plaintext.size() + crypto::kAeadTagBytes);
    const auto nonce = nonce_bytes(suite_, nonce_);
    if (!key_.encrypt(nonce, {}, plaintext, std::span<std::uint8_t>(out).subspan(start))) {
        out.resize(start);
        return false;
    }
    ++nonce_;
    return true;
}

bool CipherState::decrypt(std::span<const std::uint8_t> ciphertext,
                          std::vector<std::uint8_t>& out) {
    if (!valid() || nonce_ == std::numeric_limits<std::uint64_t>::max() ||
        ciphertext.size() < crypto::kAeadTagBytes || ciphertext.size() > kMaxMessageBytes) {
        return false;
    }
    const std::size_t start = out.size();
    out.resize(start + ciphertext.size() - crypto::kAeadTagBytes);
    const auto nonce = nonce_bytes(suite_, nonce_);
    if (!key_.decrypt(nonce, {}, ciphertext, std::span<std::uint8_t>(out).subspan(start))) {
        out.resize(start);
        return false;
    }
    ++nonce_;
    return true;
}

// --- Handshake ----------------------------------------------------------------

Handshake::Handshake(Suite suite, Role role, const KeyPair& local_static,
                     const Key32& remote_static, std::span<const std::uint8_t> prologue)
    : suite_(suite), role_(role), s_(local_static), rs_(remote_static) {
    // §5.2 InitializeSymmetric: a protocol name of at most HASHLEN bytes is h
    // itself, zero-padded; a longer one is hashed. "Noise_KKpsk2_25519_AESGCM_SHA256"
    // is exactly 32 bytes and takes the first branch.
    const std::string_view name = protocol_name(suite);
    if (name.size() <= kHashBytes) {
        std::copy(name.begin(), name.end(), h_.begin());
    } else {
        const Bytes whole(static_cast<const std::uint8_t*>(static_cast<const void*>(name.data())),
                          name.size());
        if (!crypto::sha256({whole}, h_)) {
            fail();
            return;
        }
    }
    ck_ = h_;
    // The prologue, then the pre-message statics in pattern order: the
    // initiator's "-> s", then the responder's "<- s".
    const Key32& initiator_static = role == Role::kInitiator ? s_.public_key() : rs_;
    const Key32& responder_static = role == Role::kInitiator ? rs_ : s_.public_key();
    if (!mix_hash(prologue) || !mix_hash(initiator_static) || !mix_hash(responder_static)) {
        fail();
    }
}

Handshake::~Handshake() {
    crypto::wipe(ck_);
    crypto::wipe(k_);
}

void Handshake::set_ephemeral(const KeyPair& ephemeral) {
    e_ = ephemeral;
    has_e_ = true;
}

bool Handshake::fail() {
    step_ = Step::kFailed;
    crypto::wipe(ck_);
    crypto::wipe(k_);
    has_k_ = false;
    return false;
}

bool Handshake::mix_hash(std::span<const std::uint8_t> data) {
    const Digest32 previous = h_;
    return crypto::sha256({Bytes(previous), data}, h_);
}

bool Handshake::mix_key(std::span<const std::uint8_t> input) {
    Digest32 temp_k{};
    const Digest32 previous = ck_;
    if (!hkdf(previous, input, ck_, temp_k, nullptr)) {
        return false;
    }
    k_ = temp_k;
    crypto::wipe(temp_k);
    has_k_ = true;
    n_ = 0;
    return true;
}

bool Handshake::mix_key_and_hash(std::span<const std::uint8_t> input) {
    Digest32 temp_h{};
    Digest32 temp_k{};
    const Digest32 previous = ck_;
    if (!hkdf(previous, input, ck_, temp_h, &temp_k) || !mix_hash(temp_h)) {
        crypto::wipe(temp_k);
        return false;
    }
    k_ = temp_k;
    crypto::wipe(temp_k);
    has_k_ = true;
    n_ = 0;
    return true;
}

bool Handshake::mix_dh(const Key32& private_key, const Key32& public_key) {
    Key32 shared{};
    const bool ok = crypto::x25519(private_key, public_key, shared) && mix_key(shared);
    crypto::wipe(shared);
    return ok;
}

bool Handshake::encrypt_and_hash(std::span<const std::uint8_t> plaintext,
                                 std::vector<std::uint8_t>& out) {
    const std::size_t start = out.size();
    if (has_k_) {
        if (!encrypt_with(suite_, k_, n_, h_, plaintext, out)) {
            return false;
        }
        ++n_;
    } else {
        out.insert(out.end(), plaintext.begin(), plaintext.end());
    }
    return mix_hash(Bytes(out).subspan(start));
}

bool Handshake::decrypt_and_hash(std::span<const std::uint8_t> ciphertext,
                                 std::vector<std::uint8_t>& out) {
    if (has_k_) {
        if (!decrypt_with(suite_, k_, n_, h_, ciphertext, out)) {
            return false;
        }
        ++n_;
    } else {
        out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    }
    return mix_hash(ciphertext);
}

// The "e" token in a PSK handshake: MixHash then MixKey of the public key (§9.2).
bool Handshake::write_ephemeral(std::vector<std::uint8_t>& message) {
    if (!has_e_) {
        std::optional<KeyPair> generated = KeyPair::generate();
        if (!generated) {
            return false;
        }
        e_ = *generated;
        has_e_ = true;
    }
    const Key32& e = e_.public_key();
    message.insert(message.end(), e.begin(), e.end());
    return mix_hash(e) && mix_key(e);
}

bool Handshake::read_ephemeral(std::span<const std::uint8_t> message) {
    if (message.size() < re_.size()) {
        return false;
    }
    std::copy_n(message.begin(), re_.size(), re_.begin());
    return mix_hash(re_) && mix_key(re_);
}

bool Handshake::write_message_1(std::span<const std::uint8_t> payload,
                                std::vector<std::uint8_t>& message) {
    if (role_ != Role::kInitiator || step_ != Step::kMessage1) {
        return false;
    }
    const std::size_t start = message.size();
    // -> e, es, ss
    if (!write_ephemeral(message) || !mix_dh(e_.private_key(), rs_) ||
        !mix_dh(s_.private_key(), rs_) || !encrypt_and_hash(payload, message) ||
        message.size() - start > kMaxMessageBytes) {
        message.resize(start);
        return fail();
    }
    step_ = Step::kMessage2;
    return true;
}

bool Handshake::read_message_1(std::span<const std::uint8_t> message,
                               std::vector<std::uint8_t>& payload) {
    if (role_ != Role::kResponder || step_ != Step::kMessage1 ||
        message.size() > kMaxMessageBytes || message.size() < 32 + crypto::kAeadTagBytes) {
        return fail();
    }
    const std::size_t start = payload.size();
    if (!read_ephemeral(message) || !mix_dh(s_.private_key(), re_) ||
        !mix_dh(s_.private_key(), rs_) || !decrypt_and_hash(message.subspan(32), payload)) {
        payload.resize(start);
        return fail();
    }
    step_ = Step::kMessage2;
    return true;
}

bool Handshake::write_message_2(const Key32& psk, std::span<const std::uint8_t> payload,
                                std::vector<std::uint8_t>& message) {
    if (role_ != Role::kResponder || step_ != Step::kMessage2) {
        return false;
    }
    const std::size_t start = message.size();
    // <- e, ee, se, psk
    if (!write_ephemeral(message) || !mix_dh(e_.private_key(), re_) ||
        !mix_dh(e_.private_key(), rs_) || !mix_key_and_hash(psk) ||
        !encrypt_and_hash(payload, message) || message.size() - start > kMaxMessageBytes) {
        message.resize(start);
        return fail();
    }
    step_ = Step::kDone;
    return true;
}

bool Handshake::read_message_2(const Key32& psk, std::span<const std::uint8_t> message,
                               std::vector<std::uint8_t>& payload) {
    if (role_ != Role::kInitiator || step_ != Step::kMessage2 ||
        message.size() > kMaxMessageBytes || message.size() < 32 + crypto::kAeadTagBytes) {
        return fail();
    }
    const std::size_t start = payload.size();
    if (!read_ephemeral(message) || !mix_dh(e_.private_key(), re_) ||
        !mix_dh(s_.private_key(), re_) || !mix_key_and_hash(psk) ||
        !decrypt_and_hash(message.subspan(32), payload)) {
        payload.resize(start);
        return fail();
    }
    step_ = Step::kDone;
    return true;
}

std::optional<Handshake::Transport> Handshake::split() const {
    if (step_ != Step::kDone) {
        return std::nullopt;
    }
    Digest32 first{};
    Digest32 second{};
    if (!hkdf(ck_, {}, first, second, nullptr)) {
        return std::nullopt;
    }
    const Key32& send_key = role_ == Role::kInitiator ? first : second;
    const Key32& receive_key = role_ == Role::kInitiator ? second : first;
    std::optional<CipherState> send = CipherState::create(suite_, send_key);
    std::optional<CipherState> receive = CipherState::create(suite_, receive_key);
    crypto::wipe(first);
    crypto::wipe(second);
    if (!send || !receive) {
        return std::nullopt;
    }
    Transport transport;
    transport.send = std::move(*send);
    transport.receive = std::move(*receive);
    transport.handshake_hash = h_;
    return transport;
}

}  // namespace ac3::sendspin::noise
