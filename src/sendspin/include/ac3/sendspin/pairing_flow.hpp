#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ac3/sendspin/cpace.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"

// Sendspin's pairing flows (pairing.md): the Pairing PSK Flow, the Dynamic Pairing Code Flow
// with its rounds, and the Static Pairing Code Flow with its window, from the client's side
// (ClientPairing) and the server's (ServerPairing), in both dialects
// (planning/hearth-sendspin-extension.md, C20 to C27).
//
// Each object is one pairing attempt on one connection. It is given the pairing messages the
// peer sends and the operator's or device's actions, and answers with the message texts to
// send and what happened; the session seals the texts into its channel, closes the connection
// when told to, and runs the re-handshake to the new long-term PSK once both sides have
// persisted it. Neither object persists anything: PairingEvents::on_paired() does.

namespace ac3::sendspin::pairing_flow {

using crypto::Digest32;
using crypto::Key32;

// What happens to the connection after a step.
enum class After : std::uint8_t {
    kContinue,  // the attempt goes on
    kEnded,     // the attempt ended without pairing; the connection stays open
    kClose,     // a protocol error, or pair/abort with concurrent_attempt: close the connection
    kPaired,    // both sides have the record: the server re-handshakes to it
};

struct Step {
    After after = After::kContinue;
    std::vector<std::string> messages;
};

// A dynamic pairing code as the client emits it: digits, or the qr_code format's 24 bytes.
using Code = std::variant<std::string, std::array<std::uint8_t, 24>>;

// --- The client -----------------------------------------------------------------------

// What outlives one attempt on the client (pairing.md, Rounds and Pairing Window): the rounds
// since the last verified server_kc, counted across attempts and servers, and the static
// code's window. One per client, shared by its connections, which the client's owner
// serialises as it does every call on them.
struct ClientPairingState {
    // The Dynamic Pairing Code Flow's round limit. A round counts from its server/pair-init,
    // when the code is emitted, whether it ends in a failed server_kc, with the attempt or
    // abandoned; a verified server_kc resets the count.
    static constexpr std::uint32_t kRoundLimit = 20;
    std::uint32_t rounds_since_verified = 0;

    // The static code's window: open from a gesture until kWindowLifetime passes, five
    // failed attempts, a completed pairing, the operator closing it, or the drop of the
    // connection that carried its first attempt, which alone may carry more.
    static constexpr std::int64_t kWindowLifetime = 300'000'000;
    static constexpr std::uint32_t kWindowFailures = 5;
    std::optional<std::int64_t> window_opened_at;
    std::uint32_t window_failures = 0;
    std::optional<std::uint64_t> window_connection;

    // The identifier the next connection takes (ClientPairingConfig::connection).
    std::uint64_t next_connection = 1;

    // The round limit holds attempts back until the operator acts.
    [[nodiscard]] bool holding_back() const { return rounds_since_verified >= kRoundLimit; }

    // Whether an attempt on `connection` may start now.
    [[nodiscard]] bool window_open(std::int64_t now, std::uint64_t connection) const {
        return window_opened_at && now - *window_opened_at < kWindowLifetime && window_failures < kWindowFailures &&
               (!window_connection || *window_connection == connection);
    }

    // The operator's gesture: a new window, bound to no connection yet.
    void open_window(std::int64_t now) {
        window_opened_at = now;
        window_failures = 0;
        window_connection.reset();
    }
    void close_window() {
        window_opened_at.reset();
        window_connection.reset();
    }
    // The operator's action at the round limit.
    void reset_rounds() { rounds_since_verified = 0; }
    // `connection` dropped.
    void dropped(std::uint64_t connection) {
        if (window_connection == connection) {
            close_window();
        }
    }
};

struct ClientPairingConfig {
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
    Dialect dialect = Dialect::kSpecification;
    // The handshake hash of the connection's current keys.
    Digest32 handshake_hash{};
    // The PSK the connection's handshake matched.
    handshake::PskCategory matched = handshake::PskCategory::kSentinel;
    // The methods this client offers, as its client/hello listed them.
    std::vector<messages::PairMethodDescriptor> offered;
    // The static pairing code, eight ASCII digits, when static_code is offered.
    std::string static_code;
    // This connection, as ClientPairingState::next_connection numbered it.
    std::uint64_t connection = 0;
};

class ClientPairingEvents {
   public:
    ClientPairingEvents() = default;
    virtual ~ClientPairingEvents() = default;
    ClientPairingEvents(const ClientPairingEvents&) = delete;
    ClientPairingEvents& operator=(const ClientPairingEvents&) = delete;
    ClientPairingEvents(ClientPairingEvents&&) = delete;
    ClientPairingEvents& operator=(ClientPairingEvents&&) = delete;

    // Show or speak the dynamic code; called again on each round, which uses the same code.
    virtual void on_code(const Code& code) = 0;
    // The attempt is held back until the operator acts: a gesture for the static code, a reset
    // of the round limit for the dynamic one. ClientPairing::resume() follows the action.
    virtual void on_held_back() = 0;
    // Persist the pairing record (pairing.md, Pairing Records), and hold the PSK among the
    // key ring's candidates for the re-handshake that follows.
    virtual void on_paired(const Key32& long_term_psk) = 0;
};

class ClientPairing {
   public:
    // The pairing server/activate arrived with `activation`, the `pairing_index`th such
    // activation since the last handshake.
    ClientPairing(ClientPairingConfig config, ClientPairingState& state, ClientPairingEvents& events);
    ~ClientPairing();
    ClientPairing(const ClientPairing&) = delete;
    ClientPairing& operator=(const ClientPairing&) = delete;
    ClientPairing(ClientPairing&&) = delete;
    ClientPairing& operator=(ClientPairing&&) = delete;

    [[nodiscard]] Step start(const messages::PairingActivation& activation, std::uint32_t pairing_index,
                             std::int64_t now);
    // A pairing message from the server: `type` and its payload.
    [[nodiscard]] Step receive(std::string_view type, json::Value payload, std::int64_t now);
    // The operator acted on the shared state (a gesture, or a reset of the round limit): a
    // held-back attempt starts if it now may.
    [[nodiscard]] Step resume(std::int64_t now);
    // The operator cancelled the attempt on the device.
    [[nodiscard]] Step cancel();
    // Ends the attempt with pair/abort `reason`, as a client refusing a concurrent attempt does.
    [[nodiscard]] Step abort(pairing_messages::AbortReason reason);
    // A server/activate ended the attempt: nothing is sent, and nothing persists.
    void abandon();
    // The attempt timeout (pairing.md, Entering and leaving pairing: two minutes).
    [[nodiscard]] Step tick(std::int64_t now);

    static constexpr std::int64_t kAttemptTimeout = 120'000'000;

    [[nodiscard]] bool finished() const { return state_ == State::kDone; }
    [[nodiscard]] bool held_back() const { return state_ == State::kPending; }
    // In progress from its first message until it ends (pairing.md, Entering and leaving
    // pairing): neither held back nor finished.
    [[nodiscard]] bool in_progress() const { return state_ != State::kDone && state_ != State::kPending; }
    // The pair/abort reason sent or received, once the attempt has ended with one.
    [[nodiscard]] std::optional<pairing_messages::AbortReason> aborted() const { return aborted_; }

   private:
    enum class State : std::uint8_t {
        kPending,         // held back, waiting for the operator
        kServerInit,      // dynamic: waiting for server/pair-init
        kAuth,            // waiting for server/pair-auth
        kConfirm,         // waiting for server/pair-confirm
        kFinalize,        // client/pair-finalize sent, waiting for server/pair-finalize
        kDone,
    };

    [[nodiscard]] bool may_start(std::int64_t now) const;
    [[nodiscard]] Step hold_back();
    [[nodiscard]] Step begin_attempt(std::int64_t now);
    [[nodiscard]] Step protocol_error();
    [[nodiscard]] std::vector<std::uint8_t> prs() const;

    ClientPairingConfig config_;
    ClientPairingState* shared_;
    ClientPairingEvents* events_;
    State state_ = State::kDone;
    std::optional<pairing_messages::AbortReason> aborted_;
    messages::PairMethod method_ = messages::PairMethod::kPairingPsk;
    std::optional<messages::CodeFormat> format_;
    std::int32_t digits_ = 6;
    std::uint32_t pairing_index_ = 0;
    std::uint32_t round_ = 0;
    std::optional<std::int64_t> attempt_started_;
    Key32 nonce_b_{};
    std::optional<Code> code_;
    std::unique_ptr<cpace::Party> party_;
    std::vector<std::uint8_t> sid_;
    Key32 long_term_psk_{};
};

// --- The server -----------------------------------------------------------------------

struct ServerPairingConfig {
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
    Dialect dialect = Dialect::kSpecification;
    Digest32 handshake_hash{};
    handshake::PskCategory matched = handshake::PskCategory::kSentinel;
};

class ServerPairing {
   public:
    explicit ServerPairing(ServerPairingConfig config);
    ~ServerPairing();
    ServerPairing(const ServerPairing&) = delete;
    ServerPairing& operator=(const ServerPairing&) = delete;
    ServerPairing(ServerPairing&&) = delete;
    ServerPairing& operator=(ServerPairing&&) = delete;

    // The pairing activation this attempt answers, sent as the `pairing_index`th since the last
    // handshake.
    void activated(const messages::PairingActivation& activation, std::uint32_t pairing_index);

    // A pairing message from the client.
    [[nodiscard]] Step receive(std::string_view type, json::Value payload);
    // The code the operator entered: digits, or a decoded SP:1 token's 24 bytes.
    [[nodiscard]] Step enter_code(const Code& code);
    // The operator cancelled the attempt.
    [[nodiscard]] Step cancel();

    // Waiting for the operator to enter the code.
    [[nodiscard]] bool wants_code() const { return state_ == State::kCode; }
    // The attempt's first message has arrived (pairing.md, Entering and leaving pairing).
    [[nodiscard]] bool started() const { return state_ != State::kInit; }
    [[nodiscard]] bool finished() const { return state_ == State::kDone; }
    // The client said it is holding the attempt back, and has not started it yet.
    [[nodiscard]] bool held_back() const { return held_back_ && state_ == State::kInit; }
    // The sentence the client sent with client/pair-pending, if any.
    [[nodiscard]] const std::optional<std::string>& pending() const { return pending_; }
    // Once kPaired: the long-term PSK to persist with the client's key, then re-handshake to.
    [[nodiscard]] const Key32& long_term_psk() const { return long_term_psk_; }
    [[nodiscard]] std::optional<pairing_messages::AbortReason> aborted() const { return aborted_; }

   private:
    enum class State : std::uint8_t {
        kInit,       // waiting for client/pair-init (code flows) or client/pair-finalize (PSK)
        kCode,       // waiting for the operator's code
        kClientAuth, // server/pair-auth sent
        kConfirm,    // server/pair-confirm sent, waiting for client/pair-confirm
        kFinalize,   // confirmed, waiting for client/pair-finalize
        kDone,
    };

    [[nodiscard]] Step protocol_error();
    [[nodiscard]] Step mismatch();
    [[nodiscard]] Step finalize(const pairing_messages::ClientPairFinalize& finalize);
    [[nodiscard]] std::vector<std::uint8_t> prs() const;

    ServerPairingConfig config_;
    State state_ = State::kInit;
    messages::PairMethod method_ = messages::PairMethod::kPairingPsk;
    std::optional<messages::CodeFormat> format_;
    std::int32_t digits_ = 6;
    std::uint32_t pairing_index_ = 0;
    std::uint32_t round_ = 0;
    std::optional<Digest32> commit_b_;
    Key32 nonce_a_{};
    std::optional<Code> code_;
    std::unique_ptr<cpace::Party> party_;
    std::vector<std::uint8_t> sid_;
    bool held_back_ = false;
    std::optional<std::string> pending_;
    std::optional<pairing_messages::AbortReason> aborted_;
    Key32 long_term_psk_{};
};

}  // namespace ac3::sendspin::pairing_flow
