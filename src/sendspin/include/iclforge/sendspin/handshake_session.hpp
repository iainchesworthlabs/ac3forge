#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"

// Sendspin's handshake phase as two state machines (connection.md, Encryption): Initiator is
// the server's side and Responder the client's, whichever side dialled. Each is fed the text
// frames it receives and answers with the frames to send, so the same code runs under a
// thread that blocks on a transport::Connection and inside an ESP32's WebSocket handler.
//
// A first handshake runs over cleartext text frames: client/init, server/init (or
// server/error), then noise/handshake twice. A re-handshake runs the two noise/handshake
// messages inside the encrypted channel instead, with the previous handshake's h as the
// prologue; the session carries them there, and the machines see the same texts.
//
// PSK selection follows the specification: the server names a PSK in message 1's payload;
// the client picks the candidate with that psk_id and category, checks a long-term PSK's
// stored server_id, and on a miss in a first handshake completes under the Sentinel PSK. A
// server whose message 2 fails under the PSK it named tries the Sentinel before failing,
// which is the authenticated credential-mismatch signal.
//
// Neither machine times anything: the driver closes a connection whose next handshake
// message does not arrive in time (about 30 s).

namespace ac3::sendspin::handshake {

// A PSK a client holds, tagged with its category.
struct PskCandidate {
    crypto::Key32 psk{};
    PskCategory category = PskCategory::kSentinel;
    // For a long-term PSK, the server_id its pairing record binds it to.
    crypto::Key32 server_key{};
};

class ClientKeyring {
   public:
    ClientKeyring() = default;
    virtual ~ClientKeyring() = default;
    ClientKeyring(const ClientKeyring&) = delete;
    ClientKeyring& operator=(const ClientKeyring&) = delete;
    ClientKeyring(ClientKeyring&&) = delete;
    ClientKeyring& operator=(ClientKeyring&&) = delete;

    // The PSK whose psk_id is `id`, held under `category`; with no category, as aiosendspin
    // 9.1.1's message 1 names none, held under any (planning/hearth-sendspin-extension.md,
    // C3). The Sentinel PSK need not be held: the machine knows it.
    [[nodiscard]] virtual std::optional<PskCandidate> find(const crypto::Digest32& id,
                                                           std::optional<PskCategory> category) const = 0;
};

struct PskChoice {
    crypto::Key32 psk{};
    PskCategory category = PskCategory::kSentinel;
};

class ServerKeyring {
   public:
    ServerKeyring() = default;
    virtual ~ServerKeyring() = default;
    ServerKeyring(const ServerKeyring&) = delete;
    ServerKeyring& operator=(const ServerKeyring&) = delete;
    ServerKeyring(ServerKeyring&&) = delete;
    ServerKeyring& operator=(ServerKeyring&&) = delete;

    // The PSK the server names to `client_key`: the long-term PSK of its pairing record, a
    // pairing PSK the operator has entered for that client, or the Sentinel.
    [[nodiscard]] virtual PskChoice choose(const crypto::Key32& client_key) const = 0;
};

[[nodiscard]] PskChoice sentinel_choice();

enum class Outcome : std::uint8_t {
    kContinue,     // waiting for the next message
    kEstablished,  // done: send the replies, then take the keys
    kFailed,       // send the replies, if any, then close
};

struct Step {
    Outcome outcome = Outcome::kContinue;
    // Frames to send, in order.
    std::vector<std::string> replies;
};

enum class Failure : std::uint8_t {
    kNone,
    kUnexpected,       // a message the machine was not waiting for
    kBadInit,          // client/init refused (the server sent server/error) or server/init unreadable
    kServerError,      // the server refused client/init
    kBadHandshake,     // noise/handshake or its payload unreadable, or a Noise failure
    kServerMismatch,   // a long-term PSK bound to a different server_id
    kPskMiss,          // no candidate in a re-handshake, where there is no fallback
    kCrypto,           // the crypto backend failed
};

class Responder {
   public:
    // A first handshake, over a WebSocket that has just opened.
    Responder(noise::KeyPair identity, noise::Suite suite, const ClientKeyring& keyring);
    // A re-handshake with `server_key`, whose previous handshake ended with `previous_hash`.
    Responder(noise::KeyPair identity, noise::Suite suite, const crypto::Key32& server_key,
              const crypto::Digest32& previous_hash, const ClientKeyring& keyring);

    // client/init, exactly as it must go on the wire; empty for a re-handshake.
    [[nodiscard]] const std::string& client_init() const { return client_init_; }

    // server/init or server/error, then noise/handshake carrying message 1.
    [[nodiscard]] Step receive(std::string_view text);

    [[nodiscard]] Failure failure() const { return failure_; }
    [[nodiscard]] std::optional<InitError> server_error() const { return server_error_; }

    // Once established.
    [[nodiscard]] const crypto::Key32& server_key() const { return server_key_; }
    // The category the handshake completed under: kSentinel after a fallback.
    [[nodiscard]] PskCategory category() const { return category_; }
    // A psk_id miss completed under the Sentinel PSK.
    [[nodiscard]] bool fell_back() const { return fell_back_; }
    // aiosendspin 9.1.1 when message 1 carried no psk_category.
    [[nodiscard]] Dialect dialect() const { return dialect_; }
    [[nodiscard]] std::optional<noise::Handshake::Transport> take_keys();

   private:
    enum class State : std::uint8_t { kServerInit, kMessage1, kDone };

    Step fail(Failure failure);

    noise::KeyPair identity_;
    noise::Suite suite_;
    const ClientKeyring* keyring_;
    bool rehandshake_ = false;
    State state_ = State::kServerInit;
    std::string client_init_;
    std::optional<noise::Handshake> handshake_;
    Failure failure_ = Failure::kNone;
    std::optional<InitError> server_error_;
    crypto::Key32 server_key_{};
    PskCategory category_ = PskCategory::kSentinel;
    bool fell_back_ = false;
    Dialect dialect_ = Dialect::kSpecification;
};

class Initiator {
   public:
    // A first handshake, over a WebSocket that has just opened.
    Initiator(noise::KeyPair identity, const ServerKeyring& keyring);
    // A re-handshake with `client_key` over `suite`, whose previous handshake ended with
    // `previous_hash`, naming `choice`.
    Initiator(noise::KeyPair identity, const crypto::Key32& client_key, noise::Suite suite,
              const crypto::Digest32& previous_hash, PskChoice choice);

    // A re-handshake's first frame, noise/handshake carrying message 1. Nothing for a first
    // handshake, whose message 1 follows client/init.
    [[nodiscard]] Step start();

    // client/init, then noise/handshake carrying message 2.
    [[nodiscard]] Step receive(std::string_view text);

    [[nodiscard]] Failure failure() const { return failure_; }
    // The reason sent in server/error, when client/init was refused.
    [[nodiscard]] std::optional<InitError> init_error() const { return init_error_; }

    // Known once client/init has been read.
    [[nodiscard]] const crypto::Key32& client_key() const { return client_key_; }
    [[nodiscard]] noise::Suite suite() const { return suite_; }

    // Once established: the category the session runs under, kSentinel after a mismatch.
    [[nodiscard]] PskCategory category() const { return category_; }
    // Message 2 validated only under the Sentinel PSK, though another PSK was named.
    [[nodiscard]] bool credential_mismatch() const { return mismatch_; }
    [[nodiscard]] std::optional<noise::Handshake::Transport> take_keys();

   private:
    enum class State : std::uint8_t { kClientInit, kMessage2, kDone };

    Step fail(Failure failure);
    [[nodiscard]] std::optional<std::string> message_1();

    noise::KeyPair identity_;
    const ServerKeyring* keyring_ = nullptr;
    State state_ = State::kClientInit;
    std::optional<noise::Handshake> handshake_;
    Failure failure_ = Failure::kNone;
    std::optional<InitError> init_error_;
    crypto::Key32 client_key_{};
    noise::Suite suite_ = noise::Suite::kChaChaPolySha256;
    PskChoice choice_;
    PskCategory category_ = PskCategory::kSentinel;
    bool mismatch_ = false;
};

}  // namespace ac3::sendspin::handshake
