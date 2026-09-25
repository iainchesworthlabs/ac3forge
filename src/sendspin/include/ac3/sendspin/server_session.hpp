#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac3/sendspin/transport.hpp"

// One connection from a Sendspin server to a client: the server half of src/sendspin
// (planning/hearth-reference-player.md, A4), one per client in ac3hearth's engine.
//
// The session runs the handshake as the Noise initiator, sends server/hello, reads the
// client's client/hello (and with it the client's dialect, planning/hearth-sendspin-extension.md,
// Music Assistant and aiosendspin 9.1.1), answers client/time the moment it arrives, and
// passes client/state, client/goodbye and client/leave to its listener. Everything else is the
// engine's to decide and the session's to carry out: the activation, the streams of player@v1 and
// _ac3forge_player@v1 (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1) and
// their chunks, group updates, commands, pairing, unpairing and re-handshakes.
//
// The calls that send check what the specification requires of them at that moment - an
// activation's roles and activities against the client and the PSK, no stream to an
// unavailable client, no chunk before the player's state or outside a stream, only commands
// the player listed, and for the extension role only data types, sample rates and settings its
// support object admits - and refuse otherwise.
//
// A pairing activation runs one attempt of the method it names (pairing_flow::ServerPairing):
// the session hands the client's pairing messages to it, the operator's code through
// enter_code(), and once the listener has persisted the record it acknowledges and
// re-handshakes to the new long-term PSK in the same output (C6). An attempt that does not
// start within kPairingStartTimeout, or finish within kPairingAttemptTimeout of starting, is
// cancelled.
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

    // Pairing (pairing.md). The client holds the attempt back, with its sentence for the
    // operator if it sent one: text from an unauthenticated device, shown as such.
    virtual void on_pairing_held_back(const std::optional<std::string>& message) = 0;
    // The attempt waits for the operator's code: ServerSession::enter_code().
    virtual void on_pairing_code_wanted() = 0;
    // Persist the pairing record binding `long_term_psk` to `client_key`, replacing any record
    // for that client, and discard the operator's approval of unpaired access for it (pairing.md,
    // Unpaired Access). On true the session acknowledges and re-handshakes to the PSK; on false,
    // when the record could not be stored, the connection closes and nothing is paired.
    [[nodiscard]] virtual bool on_paired(const crypto::Key32& client_key, const crypto::Key32& long_term_psk) = 0;
    // The attempt ended without pairing: the pair/abort reason sent or received, attempt_timeout
    // when the session's own timeout cancelled it, none on a protocol error, which closes the
    // connection.
    virtual void on_pairing_ended(std::optional<pairing_messages::AbortReason> reason) = 0;

    // The other roles, which a server that does not activate them never hears from.
    //
    // controller@v1: a command the controller state last sent lists, a seek already checked
    // against its seek_max_ms.
    virtual void on_controller_command(const controller::CommandMessage& /*command*/) {}
    // source@v1: the client opened its input stream after a start, or changed its format; one chunk
    // of it, captured from `timestamp_us` on the server clock; and the stream's end.
    virtual void on_source_stream_start(const messages::ClientStreamStart& /*start*/) {}
    virtual void on_source_audio(std::int64_t /*timestamp_us*/, std::span<const std::uint8_t> /*frame*/) {}
    virtual void on_source_stream_end() {}
};

enum class Refusal : std::uint8_t {
    kNotReady,          // the phase does not allow the call
    kBadActivation,     // roles or activities the client or the PSK does not allow
    kUnavailable,       // the client reports available: false
    kNoPlayerState,     // the player role is not active, or its client/state has not arrived
    kNoStream,          // no stream of the role is running
    kFormatNotListed,   // a stream format, or a data type and sample rate, the client did not list
    kFormatChange,      // aiosendspin 9.1.1 does not take a format change in place (C17)
    kCommandNotListed,  // a player command the client did not list
    kBadBurst,          // a burst whose Pc and Pd do not fit its payload or the running stream
    kBadSettings,       // settings the client's support object does not admit
    kNoRole,            // the role is not active, or the client/state it needs has not arrived
    kScheduled,         // a role's first state since its activation with a future timestamp
    kTransfer,          // an artwork announce while a transfer is in flight, or a part outside one
    kBadFrame,          // a visualizer frame of a type, time or size its stream does not take
    kBufferFull,        // a visualizer frame past the client's buffer_capacity
    kNotPaired,         // server/unpair on a connection without a long-term PSK
    kNoAttempt,         // no pairing attempt is waiting for this
    kBadCode,           // not a code of the shape the attempt takes
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
    // Handshake, hello and pairing timeouts. Call at least every next_tick_us() microseconds.
    [[nodiscard]] SessionOutput tick();
    [[nodiscard]] std::int64_t next_tick_us() const { return 1'000'000; }

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

    // _ac3forge_player@v1's stream, as player@v1's: a data type and sample rate the client listed.
    [[nodiscard]] std::expected<SessionOutput, Refusal> start_burst_stream(const ac3forge::StreamStart& stream);
    // One burst of the running stream, to be played from `timestamp_us` on the server clock: its
    // Pc and Pd as ac3::iec61937 writes them, and the payload they describe. send_ahead is taken
    // just before the chunk is sealed.
    [[nodiscard]] std::expected<SessionOutput, Refusal> send_burst(std::int64_t timestamp_us, std::uint16_t pc,
                                                                   std::uint16_t pd,
                                                                   std::span<const std::uint8_t> payload);
    [[nodiscard]] std::expected<SessionOutput, Refusal> clear_burst_stream();
    [[nodiscard]] std::expected<SessionOutput, Refusal> end_burst_stream();
    // A command the role's state lists; settings also checked against the support object
    // (ac3forge::check_settings).
    [[nodiscard]] std::expected<SessionOutput, Refusal> ac3forge_command(const ac3forge::CommandMessage& command);

    // metadata@v1, controller@v1 and color@v1: each object present only for an active role. The
    // first state of a role since its activation must not be scheduled in the future (messaging.md,
    // server/state).
    [[nodiscard]] std::expected<SessionOutput, Refusal> send_state(const messages::ServerState& state);

    // artwork@v1: a stream in the channels the client's latest state declares. Starting it again
    // after the client changed them cancels a transfer on a channel whose configuration changed and
    // clears a channel whose source became none, before the new stream/start.
    [[nodiscard]] std::expected<SessionOutput, Refusal> start_artwork_stream();
    // One image as an announce, then parts, at most one transfer in flight across the channels:
    // announce_artwork() with the image's size, then send_artwork_part() with the next bytes, of at
    // most artwork::kMaxMessageBytes - 2 each, until the size is reached. A size of 0 clears the
    // channel and completes at once.
    [[nodiscard]] std::expected<SessionOutput, Refusal> announce_artwork(std::size_t channel, std::int64_t timestamp_us,
                                                                         std::uint32_t total_size);
    [[nodiscard]] std::expected<SessionOutput, Refusal> send_artwork_part(std::span<const std::uint8_t> data);
    // Discards the channel's pending image, and ends a transfer in flight on it.
    [[nodiscard]] std::expected<SessionOutput, Refusal> cancel_artwork(std::size_t channel);
    [[nodiscard]] std::expected<SessionOutput, Refusal> end_artwork_stream();
    // Whether an artwork transfer is in flight, and its channel.
    [[nodiscard]] std::optional<std::size_t> artwork_transfer() const;
    [[nodiscard]] bool artwork_streaming() const { return artwork_stream_.has_value(); }
    // Whether `role` is active on the connection, and whether a server/state object has gone out for
    // it since, for the three state roles.
    [[nodiscard]] bool role_active(std::string_view role) const;
    [[nodiscard]] bool state_sent(std::string_view role) const;

    // visualizer@v1: a stream derived from the client's latest state; frames of the stream's types in
    // non-decreasing timestamp order, each periodic type at no more than its rate, and within the
    // client's buffer_capacity counting frames not yet due.
    [[nodiscard]] std::expected<SessionOutput, Refusal> start_visualizer_stream(const visualizer::StreamStart& start);
    [[nodiscard]] std::expected<SessionOutput, Refusal> send_visualizer_frame(const visualizer::Frame& frame);
    [[nodiscard]] std::expected<SessionOutput, Refusal> clear_visualizer_stream();
    [[nodiscard]] std::expected<SessionOutput, Refusal> end_visualizer_stream();

    // source@v1: start only once the role's client/state has arrived and the client is available.
    [[nodiscard]] std::expected<SessionOutput, Refusal> source_command(source::Command command);
    [[nodiscard]] std::expected<SessionOutput, Refusal> unpair();
    // Runs a new handshake inside the channel, naming `choice`: to the pairing PSK before a
    // pairing_psk activation, or to rotate keys. Refused while a pairing attempt is running.
    [[nodiscard]] std::expected<SessionOutput, Refusal> rehandshake(const handshake::PskChoice& choice);

    // The code the operator entered: its digits with any separators removed, or a decoded
    // SP:1 token's 24 bytes.
    [[nodiscard]] std::expected<SessionOutput, Refusal> enter_code(const pairing_flow::Code& code);
    // The operator cancelled pairing: pair/abort user_cancelled if an attempt is running, then
    // an activation declaring nothing. An aiosendspin 9.1.1 client refuses that activation on
    // the pairing PSK, so that connection closes instead (C11).
    [[nodiscard]] std::expected<SessionOutput, Refusal> cancel_pairing();

    // Five minutes for the client to start an attempt, which may wait for a gesture on the
    // device; the client's own attempt timeout is two minutes from its first message.
    static constexpr std::int64_t kPairingStartTimeout = 300'000'000;
    static constexpr std::int64_t kPairingAttemptTimeout = 150'000'000;

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
    [[nodiscard]] bool burst_streaming() const { return burst_stream_.has_value(); }
    // A pairing activity is declared on the connection.
    [[nodiscard]] bool pairing() const;
    // A pairing attempt is running: activated and not yet ended. The activity stays declared
    // after an attempt ends without pairing, until the next activation.
    [[nodiscard]] bool pairing_attempt_running() const { return attempt_running(); }
    // A pairing attempt waits for the operator's code.
    [[nodiscard]] bool pairing_wants_code() const { return attempt_ && attempt_->wants_code(); }

   private:
    [[nodiscard]] SessionOutput close_silently();
    [[nodiscard]] bool seal_json(std::string_view json, SessionOutput& out);
    [[nodiscard]] SessionOutput on_handshake_text(std::string_view text);
    [[nodiscard]] SessionOutput on_json(std::string_view text, std::int64_t arrival);
    [[nodiscard]] SessionOutput on_pairing_message(std::string_view type, json::Value payload, std::int64_t arrival);
    [[nodiscard]] SessionOutput established(handshake::Initiator& initiator);
    [[nodiscard]] std::expected<SessionOutput, Refusal> begin_rehandshake(const handshake::PskChoice& choice);
    [[nodiscard]] std::optional<messages::PairingActivation> pairing_parameters(
        const std::optional<messages::PairingActivation>& requested) const;
    // What the attempt's last step asks for; `wanted` is whether it wanted a code before.
    [[nodiscard]] SessionOutput pairing_step(pairing_flow::Step step, bool wanted);
    [[nodiscard]] SessionOutput paired(pairing_flow::Step step);
    void leave_pairing(SessionOutput& out);
    [[nodiscard]] bool attempt_running() const { return attempt_ && !attempt_->finished(); }
    [[nodiscard]] bool player_active() const;
    [[nodiscard]] bool ac3forge_active() const;
    [[nodiscard]] SessionOutput on_source_chunk(std::span<const std::uint8_t> message);
    // What removing roles owes the client before the activation: each removed stream role's
    // stream/end, a cancel before an artwork one, and a null state for each removed state role
    // that has had one.
    [[nodiscard]] bool end_removed_roles(const std::vector<std::string>& roles, SessionOutput& out);
    [[nodiscard]] bool seal_binary(std::span<const std::uint8_t> message, SessionOutput& out);
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
    bool ac3forge_state_received_ = false;
    std::vector<messages::Activity> activities_;
    std::vector<std::string> active_roles_;
    std::optional<messages::PlayerStream> stream_;
    std::optional<ac3forge::StreamStart> burst_stream_;

    // The state roles: whether a state has gone out since each was activated, and the controller
    // state last sent, whose commands and seek range a client's command is checked against.
    bool metadata_sent_ = false;
    bool controller_sent_ = false;
    bool color_sent_ = false;
    std::optional<controller::State> controller_state_;
    bool artwork_state_received_ = false;
    bool visualizer_state_received_ = false;
    bool source_state_received_ = false;
    std::optional<artwork::Channels> artwork_stream_;
    struct ArtworkTransfer {
        std::size_t channel = 0;
        std::uint32_t remaining = 0;
    };
    std::optional<ArtworkTransfer> artwork_transfer_;
    std::optional<visualizer::StreamStart> visualizer_stream_;
    // Visualizer frames sent: the latest timestamp, each periodic type's last, and each frame not
    // yet due with its size.
    std::int64_t visualizer_last_ = 0;
    std::array<std::optional<std::int64_t>, 5> visualizer_type_last_{};
    std::vector<std::pair<std::int64_t, std::size_t>> visualizer_pending_;
    // source@v1: a start sent and not stopped, and the client's input stream open.
    bool source_started_ = false;
    bool source_stream_open_ = false;

    // Pairing activations since the last handshake (pairing.md, Pairing index).
    std::uint32_t pairing_index_ = 0;
    std::unique_ptr<pairing_flow::ServerPairing> attempt_;
    // When the attempt was activated, then when its first message arrived.
    std::int64_t attempt_since_ = 0;
};

}  // namespace ac3::sendspin
