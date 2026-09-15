#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/clock_sync.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/session.hpp"
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
// Not here yet, and answered as the specification allows meanwhile: pairing (an activation
// that asks for it gets pair/abort reason method_not_supported), arbitration between servers
// (the owner of several sessions decides), and the roles other than player@v1.

namespace ac3::sendspin {

struct PlayerConfig {
    noise::KeyPair identity;
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
    std::string name;
    messages::DeviceInfo device_info;
    // Written to client/hello as they are, in order; player@v1 must be among them.
    std::vector<std::string> supported_roles{"player@v1"};
    messages::PlayerSupport player_support;
    std::vector<messages::PairMethodDescriptor> pair_methods;
    bool unpaired_access = false;
    // The player state reported while nothing has changed it: volume, mute, delay, timing
    // and commands.
    messages::PlayerState player_state;
    // Bounds one reassembled message, ID included.
    std::size_t max_message_bytes = 4 * 1024 * 1024;
};

class PlayerListener {
   public:
    PlayerListener() = default;
    virtual ~PlayerListener() = default;
    PlayerListener(const PlayerListener&) = delete;
    PlayerListener& operator=(const PlayerListener&) = delete;
    PlayerListener(PlayerListener&&) = delete;
    PlayerListener& operator=(PlayerListener&&) = delete;

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
};

class PlayerSession {
   public:
    PlayerSession(PlayerConfig config, const handshake::ClientKeyring& keyring, PlayerListener& listener,
                  const Clock& clock);
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
    // The player's output was taken by something outside Sendspin, or given back.
    [[nodiscard]] SessionOutput set_external_source(bool external);
    [[nodiscard]] SessionOutput goodbye(messages::GoodbyeReason reason);

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

   private:
    [[nodiscard]] SessionOutput close_silently();
    void seal(std::string_view json, SessionOutput& out);
    [[nodiscard]] SessionOutput on_handshake_text(std::string_view text);
    [[nodiscard]] SessionOutput on_message(std::span<const std::uint8_t> message, std::int64_t arrival);
    [[nodiscard]] SessionOutput on_json(std::string_view text, std::int64_t arrival);
    [[nodiscard]] SessionOutput on_activate(const messages::Activate& activate);
    [[nodiscard]] SessionOutput on_rehandshake(std::string_view text);
    void send_state(SessionOutput& out);
    void send_clock(SessionOutput& out);
    [[nodiscard]] bool player_active() const;

    PlayerConfig config_;
    const handshake::ClientKeyring* keyring_;
    PlayerListener* listener_;
    const Clock* clock_source_;

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

    ClockSync clock_;
    bool reported_available_ = false;
    bool sent_state_ = false;
    bool external_source_ = false;
    messages::PlayerState state_;
    std::optional<messages::PlayerStream> stream_;
};

}  // namespace ac3::sendspin
