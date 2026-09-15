#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/transport.hpp"

// One connection from a Sendspin server to a client: the server half of src/sendspin
// (planning/hearth-reference-player.md, A4), one per client in ac3hearth's engine.
//
// The session runs the handshake as the Noise initiator, sends server/hello, reads the
// client's client/hello (and with it the client's dialect, planning/hearth-sendspin-extension.md,
// Music Assistant and aiosendspin 9.1.1), answers client/time the moment it arrives, and
// passes client/state, client/goodbye and client/leave to its listener. Everything else is the
// engine's to decide and the session's to carry out: the activation, the player@v1 stream and
// its chunks, group updates, commands, unpairing and re-handshakes.
//
// The calls that send check what the specification requires of them at that moment - an
// activation's roles and activities against the client and the PSK, no stream to an
// unavailable client, no chunk before the player's state or outside a stream, only commands
// the player listed - and refuse otherwise.
//
// Not synchronised: the engine serialises every call on one session, receive() and tick()
// included.

namespace ac3::sendspin {

struct ServerConfig {
    noise::KeyPair identity;
    std::string name;
    std::vector<std::string> languages;
    // Bounds one reassembled message, ID included.
    std::size_t max_message_bytes = 4 * 1024 * 1024;
};

class ServerListener {
   public:
    ServerListener() = default;
    virtual ~ServerListener() = default;
    ServerListener(const ServerListener&) = delete;
    ServerListener& operator=(const ServerListener&) = delete;
    ServerListener(ServerListener&&) = delete;
    ServerListener& operator=(ServerListener&&) = delete;

    // client/hello arrived: the engine answers with ServerSession::activate().
    virtual void on_hello(const messages::ClientHello& hello) = 0;
    virtual void on_state(const messages::ClientState& state) = 0;
    virtual void on_goodbye(messages::GoodbyeReason reason) = 0;
    virtual void on_leave() = 0;
};

enum class Refusal : std::uint8_t {
    kNotReady,          // the phase does not allow the call
    kBadActivation,     // roles or activities the client or the PSK does not allow
    kUnavailable,       // the client reports available: false
    kNoPlayerState,     // player@v1 is not active, or its client/state has not arrived
    kNoStream,          // no player stream is running
    kFormatNotListed,   // a stream format the client did not list
    kFormatChange,      // aiosendspin 9.1.1 does not take a format change in place (C17)
    kCommandNotListed,  // a player command the client did not list
    kNotPaired,         // server/unpair on a connection without a long-term PSK
    kCrypto,            // the channel failed
};

class ServerSession {
   public:
    ServerSession(ServerConfig config, const handshake::ServerKeyring& keyring, ServerListener& listener,
                  const Clock& clock);
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;
    ServerSession(ServerSession&&) = delete;
    ServerSession& operator=(ServerSession&&) = delete;

    [[nodiscard]] SessionOutput receive(const transport::Frame& frame);
    // Handshake and hello timeouts. Call at least once a second.
    [[nodiscard]] SessionOutput tick();

    [[nodiscard]] std::expected<SessionOutput, Refusal> activate(const messages::Activate& activate);
    [[nodiscard]] std::expected<SessionOutput, Refusal> start_stream(const messages::PlayerStream& stream);
    // One chunk of the running player stream: `frame` to be played from `timestamp_us` on
    // the server clock. send_ahead is taken just before the chunk is sealed.
    [[nodiscard]] std::expected<SessionOutput, Refusal> send_audio(std::int64_t timestamp_us,
                                                                   std::span<const std::uint8_t> frame);
    [[nodiscard]] std::expected<SessionOutput, Refusal> clear_stream();
    [[nodiscard]] std::expected<SessionOutput, Refusal> end_stream();
    [[nodiscard]] std::expected<SessionOutput, Refusal> update_group(const messages::GroupUpdate& update);
    [[nodiscard]] std::expected<SessionOutput, Refusal> command(const messages::PlayerCommandMessage& command);
    [[nodiscard]] std::expected<SessionOutput, Refusal> unpair();
    // Runs a new handshake inside the channel, naming `choice`, as after a pairing.
    [[nodiscard]] std::expected<SessionOutput, Refusal> rehandshake(const handshake::PskChoice& choice);

    enum class Phase : std::uint8_t {
        kHandshake,
        kRehandshake,  // message 1 sent inside the channel, waiting for message 2
        kHello,        // server/hello sent, waiting for client/hello
        kReady,        // client/hello received; nothing activated yet
        kActive,
        kClosed,
    };

    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] Dialect dialect() const { return dialect_; }
    [[nodiscard]] const crypto::Key32& client_key() const { return client_key_; }
    [[nodiscard]] handshake::PskCategory psk_category() const { return category_; }
    // The client could not use the PSK named and completed under the Sentinel, while this
    // server holds a record for it: no roles or playback until it pairs again
    // (connection.md, Sentinel Fallback).
    [[nodiscard]] bool credential_mismatch() const { return mismatch_; }
    [[nodiscard]] const std::optional<messages::ClientHello>& hello() const { return hello_; }
    [[nodiscard]] const std::optional<messages::ClientState>& state() const { return state_; }
    [[nodiscard]] const std::vector<std::string>& active_roles() const { return active_roles_; }
    [[nodiscard]] bool streaming() const { return stream_.has_value(); }

   private:
    [[nodiscard]] SessionOutput close_silently();
    [[nodiscard]] bool seal_json(std::string_view json, SessionOutput& out);
    [[nodiscard]] SessionOutput on_handshake_text(std::string_view text);
    [[nodiscard]] SessionOutput on_json(std::string_view text, std::int64_t arrival);
    [[nodiscard]] SessionOutput established(handshake::Initiator& initiator);
    [[nodiscard]] bool player_active() const;
    [[nodiscard]] std::expected<SessionOutput, Refusal> sent(SessionOutput out, bool ok);

    ServerConfig config_;
    ServerListener* listener_;
    const Clock* clock_;

    Phase phase_ = Phase::kHandshake;
    std::int64_t phase_started_ = 0;
    std::unique_ptr<handshake::Initiator> initiator_;
    std::optional<Channel> channel_;
    Dialect dialect_ = Dialect::kSpecification;
    crypto::Key32 client_key_{};
    noise::Suite suite_ = noise::Suite::kChaChaPolySha256;
    handshake::PskCategory category_ = handshake::PskCategory::kSentinel;
    bool mismatch_ = false;

    std::optional<messages::ClientHello> hello_;
    std::optional<messages::ClientState> state_;
    bool player_state_received_ = false;
    std::vector<messages::Activity> activities_;
    std::vector<std::string> active_roles_;
    std::optional<messages::PlayerStream> stream_;
};

}  // namespace ac3::sendspin
