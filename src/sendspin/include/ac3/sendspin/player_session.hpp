#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/clock_sync.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac3/sendspin/transport.hpp"

// One connection from a Sendspin client with the player role to a server: the player half of
// src/sendspin (planning/hearth-reference-player.md, A4), as the test sink and hearth_sink use
// it.
//
// The session runs the handshake as the Noise responder, answers server/hello with its
// client/hello, checks each server/activate as messaging.md's admissibility rules say,
// synchronises its clock once activated, reports client/state, and hands the player@v1 stream
// and the server's commands to its listener. With an aiosendspin 9.1.1 server, which it
// recognises from Noise message 1, it speaks 9.1.1's forms
// (planning/hearth-sendspin-extension.md, Music Assistant and aiosendspin 9.1.1).
//
// A pairing activation runs one attempt of the method it names (pairing_flow::ClientPairing).
// From that activation until the next, or until the re-handshake after a pairing, the session
// sends only pairing messages: no clock exchanges and no client/state, which aiosendspin 9.1.1
// requires (C6, C25).
//
// Admission between servers is the owner's (connection.md, Multiple servers; Arbiter in
// arbiter.hpp decides it): the session asks its listener about every admissible activation
// before acting on it, refuses one the owner rejects, and leaves when displace() says another
// server has taken the client.
//
// _ac3forge_player@v1 (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1) runs
// as player@v1 does, when the player lists it: its stream, its burst chunks on the same clock,
// its commands and its state object. A chunk of a data type other than the stream's, or one
// whose header does not fit its payload, goes to the listener to be counted in invalid_chunks.
//
// The other roles run for a client that lists them, which Hearth's sinks do not: the server/state
// objects of metadata@v1, controller@v1 and color@v1 and the controller's commands; artwork@v1's
// stream and image messages, a malformed one closing the connection as the role requires;
// visualizer@v1's stream and frames on the player's clock; and source@v1's commands, input stream
// and chunks.

namespace ac3::sendspin {

struct PlayerConfig {
    noise::KeyPair identity;
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
    std::string name;
    messages::DeviceInfo device_info;
    // Written to client/hello as they are, in order; player@v1 must be among them, and
    // _ac3forge_player@v1 first when `ac3forge_support` is set.
    std::vector<std::string> supported_roles{"player@v1"};
    messages::PlayerSupport player_support;
    std::optional<ac3forge::Support> ac3forge_support;
    // The pairing methods offered: pairing_psk, and at most one code method.
    std::vector<messages::PairMethodDescriptor> pair_methods;
    // The static pairing code, eight ASCII digits, when pair_methods offers static_code.
    std::string static_code;
    bool unpaired_access = false;
    // The player state reported while nothing has changed it: volume, mute, delay, timing
    // and commands.
    messages::PlayerState player_state;
    // _ac3forge_player@v1's, likewise.
    ac3forge::State ac3forge_state;
    // Bounds one reassembled message, ID included.
    std::size_t max_message_bytes = 4 * 1024 * 1024;
    // The other roles' support objects, offered with the roles listed, and their states reported
    // while each role is active.
    std::optional<source::Support> source_support = std::nullopt;
    std::optional<visualizer::Support> visualizer_support = std::nullopt;
    source::State source_state{};
    artwork::Channels artwork_state{};
    visualizer::State visualizer_state{};
};

class PlayerListener {
   public:
    PlayerListener() = default;
    virtual ~PlayerListener() = default;
    PlayerListener(const PlayerListener&) = delete;
    PlayerListener& operator=(const PlayerListener&) = delete;
    PlayerListener(PlayerListener&&) = delete;
    PlayerListener& operator=(PlayerListener&&) = delete;

    // An admissible server/activate, before the session acts on it: the connection's first,
    // which ends its provisional state, or a later one. Returns whether to admit it
    // (connection.md, Multiple servers); an owner with one connection admits them all. The
    // session answers a rejected one with client/goodbye concurrent_attempt, or pair/abort
    // concurrent_attempt for a pairing activation, and closes.
    [[nodiscard]] virtual bool on_activation(const crypto::Key32& server_key, const messages::Activate& activate,
                                             bool first) = 0;
    // A pairing attempt began or ended on this connection; the owner does not let another
    // server displace one in progress.
    virtual void on_pairing_attempt(bool in_progress) = 0;

    // player@v1's stream began, or changed format in place.
    virtual void on_stream_start(const messages::PlayerStream& stream) = 0;
    // Drop everything buffered and carry on with chunks received after this.
    virtual void on_stream_clear() = 0;
    virtual void on_stream_end() = 0;
    // One encoded frame to play from `local_time`: the chunk's timestamp mapped to local time,
    // less the output delay.
    virtual void on_audio(std::span<const std::uint8_t> frame, std::int64_t local_time) = 0;
    // A volume, mute or output delay command the player listed. The listener applies it and
    // reports the new state through PlayerSession::set_state().
    virtual void on_command(const messages::PlayerCommandMessage& command) = 0;
    virtual void on_group(const messages::GroupUpdate& update) = 0;
    // The server unpaired this player: the listener removes the pairing record for
    // `server_key`.
    virtual void on_unpaired(const crypto::Key32& server_key) = 0;

    // Pairing (pairing.md). Show or speak the dynamic code; called again on each round.
    virtual void on_pairing_code(const pairing_flow::Code& code) = 0;
    // The attempt waits for the operator: a gesture opening the static code's window, or an
    // action at the round limit. After changing the shared pairing state, the owner calls
    // PlayerSession::resume_pairing().
    virtual void on_pairing_held_back() = 0;
    // Persist the pairing record binding `long_term_psk` to `server_key`, replacing any record
    // for that server, and hold the PSK among the key ring's candidates: the server
    // re-handshakes to it next.
    virtual void on_paired(const crypto::Key32& server_key, const crypto::Key32& long_term_psk) = 0;
    // The attempt ended without pairing, with the pair/abort reason sent or received; none when
    // a server/activate or a re-handshake superseded it, or a protocol error closed the
    // connection.
    virtual void on_pairing_ended(std::optional<pairing_messages::AbortReason> reason) = 0;

    // _ac3forge_player@v1, which a player that does not list the role never hears from.
    //
    // The role's stream began, or changed in place: a data type and sample rate the player listed.
    virtual void on_burst_stream_start(const ac3forge::StreamStart& /*stream*/) {}
    // Drop every buffered chunk and any decoded audio not yet played, reset the decoder, and carry
    // on with chunks received after this.
    virtual void on_burst_stream_clear() {}
    // Stop output, and drop the buffers and the decoder.
    virtual void on_burst_stream_end() {}
    // One burst chunk to play from `local_time`: its timestamp mapped to local time, less the
    // role's output delay. The payload is valid during the call.
    virtual void on_burst(const BurstChunk& /*chunk*/, std::int64_t /*local_time*/) {}
    // A chunk the player rejects, dropped without closing: counted in invalid_chunks.
    virtual void on_invalid_burst() {}
    // A command the role's state listed. The listener applies it and reports the new state through
    // PlayerSession::set_ac3forge_state(); a settings command at the next burst boundary.
    virtual void on_ac3forge_command(const ac3forge::CommandMessage& /*command*/) {}
    // A settings command the reader refused, with the revision it named: report settings_error.
    virtual void on_settings_refused(const ac3forge::SettingsError& /*error*/) {}

    // The other roles, for a client that lists them.
    //
    // server/state's objects for the roles that are active, as the message carries them.
    virtual void on_server_state(const messages::ServerState& /*state*/) {}
    // artwork@v1's stream began or changed; one of its messages, in order; and its end.
    virtual void on_artwork_stream_start(const artwork::Channels& /*channels*/) {}
    virtual void on_artwork_message(const artwork::Message& /*message*/) {}
    virtual void on_artwork_stream_end() {}
    // visualizer@v1's stream began or changed; one frame, to show at `local_time`; a clear; its end.
    virtual void on_visualizer_stream_start(const visualizer::StreamStart& /*start*/) {}
    virtual void on_visualizer_frame(const visualizer::Frame& /*frame*/, std::int64_t /*local_time*/) {}
    virtual void on_visualizer_stream_clear() {}
    virtual void on_visualizer_stream_end() {}
    // source@v1: the server's start, which arrives only while the client is available, or stop.
    virtual void on_source_command(source::Command /*command*/) {}
};

class PlayerSession {
   public:
    // `pairing` is shared by every session of the same client and outlives them. Destroying
    // a session is the drop of its connection, which closes a pairing window bound to it.
    PlayerSession(PlayerConfig config, const handshake::ClientKeyring& keyring,
                  pairing_flow::ClientPairingState& pairing, PlayerListener& listener, const Clock& clock);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;
    PlayerSession(PlayerSession&&) = delete;
    PlayerSession& operator=(PlayerSession&&) = delete;

    // The WebSocket has opened: client/init.
    [[nodiscard]] SessionOutput open();
    // A frame arrived.
    [[nodiscard]] SessionOutput receive(const transport::Frame& frame);
    // Timers: handshake and provisional timeouts, clock exchanges, and the available: true
    // report once the clock has converged. Call at least every next_tick_us() microseconds.
    [[nodiscard]] SessionOutput tick();
    [[nodiscard]] std::int64_t next_tick_us() const;

    // The player's own state changed, or a command was applied: report it.
    [[nodiscard]] SessionOutput set_state(const messages::PlayerState& state);
    // The same for _ac3forge_player@v1. A sink sends fresh levels at most ten times a second.
    [[nodiscard]] SessionOutput set_ac3forge_state(const ac3forge::State& state);
    // The other roles' states, reported at once while the role is active.
    [[nodiscard]] SessionOutput set_artwork_state(const artwork::Channels& channels);
    [[nodiscard]] SessionOutput set_visualizer_state(const visualizer::State& state);
    [[nodiscard]] SessionOutput set_source_state(const source::State& state);
    // controller@v1's command, sent only while the role is active.
    [[nodiscard]] SessionOutput send_command(const controller::CommandMessage& command);
    // source@v1's input stream: opened only after the server's start while available, its chunks,
    // and its end.
    [[nodiscard]] SessionOutput start_source_stream(const messages::ClientStreamStart& start);
    [[nodiscard]] SessionOutput send_source_audio(std::int64_t timestamp_us, std::span<const std::uint8_t> frame);
    [[nodiscard]] SessionOutput end_source_stream();
    // The player's output was taken by something outside Sendspin, or given back.
    [[nodiscard]] SessionOutput set_external_source(bool external);
    [[nodiscard]] SessionOutput goodbye(messages::GoodbyeReason reason);

    // The owner changed the shared pairing state for the operator (a gesture, or a reset of
    // the round limit): an attempt this session holds back starts if it now may.
    [[nodiscard]] SessionOutput resume_pairing();
    // The operator cancelled the attempt on the device.
    [[nodiscard]] SessionOutput cancel_pairing();
    // Another server's connection has taken the client: client/goodbye another_server, or
    // pair/abort concurrent_attempt while pairing, then close.
    [[nodiscard]] SessionOutput displace();

    enum class Phase : std::uint8_t {
        kHandshake,
        kHello,        // waiting for server/hello
        kProvisional,  // waiting for the first server/activate
        kActive,
        kClosed,
    };

    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] Dialect dialect() const { return dialect_; }
    [[nodiscard]] const crypto::Key32& server_key() const { return server_key_; }
    [[nodiscard]] std::optional<handshake::PskCategory> psk_category() const { return category_; }
    // The server named a PSK this player does not hold, and the handshake completed under the
    // Sentinel: the server holds a pairing the player has lost. Against aiosendspin 9.1.1 the
    // server then fails the handshake, and the player's owner reports it
    // (planning/hearth-sendspin-extension.md, C4).
    [[nodiscard]] bool fell_back() const { return fell_back_; }
    [[nodiscard]] const std::string& server_name() const { return server_name_; }
    [[nodiscard]] const std::vector<messages::Activity>& activities() const { return activities_; }
    [[nodiscard]] const std::vector<std::string>& active_roles() const { return active_roles_; }
    [[nodiscard]] bool clock_converged() const { return clock_.converged(); }
    [[nodiscard]] bool streaming() const { return stream_.has_value(); }
    // An _ac3forge_player@v1 stream is running.
    [[nodiscard]] bool burst_streaming() const { return burst_stream_.has_value(); }
    [[nodiscard]] bool artwork_streaming() const { return artwork_stream_.has_value(); }
    [[nodiscard]] bool visualizer_streaming() const { return visualizer_stream_.has_value(); }
    // source@v1: the server has started the source, and the input stream is open.
    [[nodiscard]] bool source_started() const { return source_started_; }
    [[nodiscard]] bool source_streaming() const { return source_stream_open_; }
    // A pairing activity is declared: from its server/activate until the next, or until the
    // re-handshake after a pairing.
    [[nodiscard]] bool pairing() const;
    [[nodiscard]] bool pairing_attempt_in_progress() const { return attempt_ && attempt_->in_progress(); }

   private:
    class PairingEvents;

    [[nodiscard]] SessionOutput close_silently();
    void seal(std::string_view json, SessionOutput& out);
    [[nodiscard]] SessionOutput on_handshake_text(std::string_view text);
    [[nodiscard]] SessionOutput on_message(std::span<const std::uint8_t> message, std::int64_t arrival);
    [[nodiscard]] SessionOutput on_json(std::string_view text, std::int64_t arrival);
    [[nodiscard]] SessionOutput on_activate(const messages::Activate& activate);
    [[nodiscard]] SessionOutput on_rehandshake(std::string_view text);
    [[nodiscard]] SessionOutput pairing_step(pairing_flow::Step step);
    void end_pairing();
    // Tells the listener when pairing_attempt_in_progress() has changed.
    void report_attempt();
    // The availability client/state reports now.
    [[nodiscard]] bool available_now() const;
    void send_state(SessionOutput& out);
    // Hands the listener one player@v1 chunk, unless it is a replay.
    void deliver_audio(std::int64_t timestamp_us, std::span<const std::uint8_t> frame);
    // Forgets the held chunks and the last timestamp: the stream began again, was cleared or ended.
    void restart_audio();
    void send_clock(SessionOutput& out);
    [[nodiscard]] bool player_active() const;
    [[nodiscard]] bool ac3forge_active() const;
    [[nodiscard]] bool role_active(std::string_view role) const;
    // Whether the player listed `stream`'s data type and sample rate.
    [[nodiscard]] bool lists(const ac3forge::StreamStart& stream) const;
    // An artwork message received: false for one the role says closes the connection.
    [[nodiscard]] bool on_artwork(std::span<const std::uint8_t> message);
    void seal_binary(std::span<const std::uint8_t> message, SessionOutput& out);

    PlayerConfig config_;
    const handshake::ClientKeyring* keyring_;
    pairing_flow::ClientPairingState* pairing_state_;
    PlayerListener* listener_;
    const Clock* clock_source_;
    std::uint64_t connection_;

    Phase phase_ = Phase::kHandshake;
    std::int64_t phase_started_ = 0;
    std::unique_ptr<handshake::Responder> responder_;
    std::unique_ptr<handshake::Responder> rehandshake_;
    std::optional<Channel> channel_;
    Dialect dialect_ = Dialect::kSpecification;
    crypto::Key32 server_key_{};
    std::optional<handshake::PskCategory> category_;
    bool fell_back_ = false;
    std::string server_name_;

    std::vector<messages::Activity> activities_;
    std::vector<std::string> active_roles_;
    std::size_t activations_ = 0;

    // Pairing activations since the last handshake (pairing.md, Pairing index).
    std::uint32_t pairing_index_ = 0;
    std::unique_ptr<PairingEvents> pairing_events_;
    std::unique_ptr<pairing_flow::ClientPairing> attempt_;
    bool reported_attempt_ = false;
    // Set once paired, until the server's re-handshake arrives or the wait times out.
    std::optional<std::int64_t> rehandshake_due_;

    ClockSync clock_;
    bool reported_available_ = false;
    bool sent_state_ = false;
    bool external_source_ = false;
    messages::PlayerState state_;
    std::optional<messages::PlayerStream> stream_;
    // player@v1's chunks from an aiosendspin 9.1.1 server that came before the clock's first
    // exchange, held for it, and the timestamp of the last chunk the listener had (C13).
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> held_audio_;
    std::uint64_t held_audio_bytes_ = 0;
    std::optional<std::int64_t> last_audio_timestamp_;
    ac3forge::State ac3forge_state_;
    std::optional<ac3forge::StreamStart> burst_stream_;

    source::State source_state_;
    artwork::Channels artwork_state_;
    visualizer::State visualizer_state_;
    std::optional<artwork::Channels> artwork_stream_;
    // The artwork transfer in flight: its channel and the bytes still to come.
    std::optional<std::pair<std::size_t, std::uint32_t>> artwork_transfer_;
    std::optional<visualizer::StreamStart> visualizer_stream_;
    bool source_started_ = false;
    bool source_stream_open_ = false;
};

}  // namespace ac3::sendspin
