#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"

// The Noise Protocol Framework (revision 34) as Sendspin uses it
// (connection.md, Encryption): the KKpsk2 pattern with X25519, SHA-256 and either
// ChaCha20-Poly1305 or AES-256-GCM. The server is always the initiator.
//
//     KKpsk2:
//       -> s
//       <- s
//       ...
//       -> e, es, ss
//       <- e, ee, se, psk
//
// KKpsk2 is the only pattern implemented; nothing here is general.
//
// The PSK is an argument to the second message rather than to the constructor.
// Sendspin binds it late: the responder learns which PSK the initiator means
// only by decrypting message 1's payload, which the PSK does not yet protect,
// and an initiator whose message 2 fails under the PSK it named tries the
// Sentinel PSK on a copy of the state (connection.md, Sentinel Fallback). A
// Handshake is therefore an ordinary copyable value.

namespace ac3::sendspin::noise {

using crypto::Digest32;
using crypto::Key32;

enum class Suite : std::uint8_t {
    kChaChaPolySha256,
    kAesGcmSha256,
};

// "25519_ChaChaPoly_SHA256" and "25519_AESGCM_SHA256", as client/init names them.
[[nodiscard]] std::string_view suite_name(Suite suite);
[[nodiscard]] std::optional<Suite> parse_suite(std::string_view name);
// "Noise_KKpsk2_" followed by the suite name.
[[nodiscard]] std::string_view protocol_name(Suite suite);

inline constexpr std::size_t kMaxMessageBytes = 65535;

// A Curve25519 key pair. The private half is wiped when the value is destroyed.
class KeyPair {
   public:
    KeyPair() = default;
    ~KeyPair();
    KeyPair(const KeyPair&) = default;
    KeyPair& operator=(const KeyPair&) = default;
    KeyPair(KeyPair&&) = default;
    KeyPair& operator=(KeyPair&&) = default;

    [[nodiscard]] static std::optional<KeyPair> generate();
    [[nodiscard]] static std::optional<KeyPair> from_private(const Key32& private_key);

    [[nodiscard]] const Key32& private_key() const { return private_; }
    [[nodiscard]] const Key32& public_key() const { return public_; }

   private:
    Key32 private_{};
    Key32 public_{};
};

// One direction of a transport-mode session: a key and its nonce counter.
class CipherState {
   public:
    CipherState() = default;

    [[nodiscard]] static std::optional<CipherState> create(Suite suite, const Key32& key);

    [[nodiscard]] bool valid() const { return key_.valid(); }
    [[nodiscard]] std::uint64_t nonce() const { return nonce_; }

    // Appends the ciphertext and tag to `out`, with empty associated data as
    // Noise's transport messages have.
    [[nodiscard]] bool encrypt(std::span<const std::uint8_t> plaintext,
                               std::vector<std::uint8_t>& out);
    // Appends the plaintext to `out`. A failure leaves the nonce where it was;
    // Sendspin closes the connection on any.
    [[nodiscard]] bool decrypt(std::span<const std::uint8_t> ciphertext,
                               std::vector<std::uint8_t>& out);

   private:
    crypto::AeadKey key_;
    Suite suite_ = Suite::kChaChaPolySha256;
    std::uint64_t nonce_ = 0;
};

enum class Role : std::uint8_t {
    kInitiator,
    kResponder,
};

class Handshake {
   public:
    // `local_static` is this side's key pair; `remote_static` the other side's
    // public key. The prologue is copied into the handshake hash at once.
    Handshake(Suite suite, Role role, const KeyPair& local_static, const Key32& remote_static,
              std::span<const std::uint8_t> prologue);
    ~Handshake();
    Handshake(const Handshake&) = default;
    Handshake& operator=(const Handshake&) = default;
    Handshake(Handshake&&) = default;
    Handshake& operator=(Handshake&&) = default;

    // The ephemeral key the next message this side writes uses; without it one is
    // generated. For test vectors.
    void set_ephemeral(const KeyPair& ephemeral);

    // The initiator's first message, and the responder reading it.
    [[nodiscard]] bool write_message_1(std::span<const std::uint8_t> payload,
                                       std::vector<std::uint8_t>& message);
    [[nodiscard]] bool read_message_1(std::span<const std::uint8_t> message,
                                      std::vector<std::uint8_t>& payload);

    // The responder's second message under `psk`, and the initiator reading it.
    [[nodiscard]] bool write_message_2(const Key32& psk, std::span<const std::uint8_t> payload,
                                       std::vector<std::uint8_t>& message);
    [[nodiscard]] bool read_message_2(const Key32& psk, std::span<const std::uint8_t> message,
                                      std::vector<std::uint8_t>& payload);

    struct Transport {
        CipherState send;
        CipherState receive;
        // h at the end of the handshake: a re-handshake's prologue.
        Digest32 handshake_hash{};
    };

    // Once message 2 has been written or read.
    [[nodiscard]] std::optional<Transport> split() const;

    [[nodiscard]] bool failed() const { return step_ == Step::kFailed; }

   private:
    enum class Step : std::uint8_t { kMessage1, kMessage2, kDone, kFailed };

    [[nodiscard]] bool mix_hash(std::span<const std::uint8_t> data);
    [[nodiscard]] bool mix_key(std::span<const std::uint8_t> input);
    [[nodiscard]] bool mix_key_and_hash(std::span<const std::uint8_t> input);
    [[nodiscard]] bool mix_dh(const Key32& private_key, const Key32& public_key);
    [[nodiscard]] bool encrypt_and_hash(std::span<const std::uint8_t> plaintext,
                                        std::vector<std::uint8_t>& out);
    [[nodiscard]] bool decrypt_and_hash(std::span<const std::uint8_t> ciphertext,
                                        std::vector<std::uint8_t>& out);
    [[nodiscard]] bool write_ephemeral(std::vector<std::uint8_t>& message);
    [[nodiscard]] bool read_ephemeral(std::span<const std::uint8_t> message);
    bool fail();

    Suite suite_;
    Role role_;
    Step step_ = Step::kMessage1;
    KeyPair s_;
    Key32 rs_{};
    KeyPair e_;
    bool has_e_ = false;
    Key32 re_{};
    Digest32 ck_{};
    Digest32 h_{};
    Key32 k_{};
    bool has_k_ = false;
    std::uint64_t n_ = 0;
};

}  // namespace ac3::sendspin::noise
