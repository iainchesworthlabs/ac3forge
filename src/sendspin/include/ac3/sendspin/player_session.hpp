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
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
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
// A pairing activation runs one attempt of the method it names (pairing_flow::ClientPairing).
// From that activation until the next, or until the re-handshake after a pairing, the session
// sends only pairing messages: no clock exchanges and no client/state, which aiosendspin 9.1.1
// requires (C6, C25).
//
// Not here yet: arbitration between servers (the owner of several sessions decides) and the
// roles other than player@v1.

namespace ac3::sendspin {

struct PlayerConfig {
    noise::KeyPair identity;
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
    std::string name;
    messages::DeviceInfo device_info;
    // Written to client/hello as they are, in order; player@v1 must be among them.
    std::vector<std::string> supported_roles{"player@v1"};
    messages::PlayerSupport player_support;
    // The pairing methods offered: pairing_psk, and at most one code method.
    std::vector<messages::PairMethodDescriptor> pair_methods;
    // The static pairing code, eight ASCII digits, when pair_methods offers static_code.
    std::string static_code;
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
    // The player's output was taken by something outside Sendspin, or given back.
    [[nodiscard]] SessionOutput set_external_source(bool external);
    [[nodiscard]] SessionOutput goodbye(messages::GoodbyeReason reason);

    // The owner changed the shared pairing state for the operator (a gesture, or a reset of
    // the round limit): an attempt this session holds back starts if it now may.
    [[nodiscard]] SessionOutput resume_pairing();
    // The operator cancelled the attempt on the device.
    [[nodiscard]] SessionOutput cancel_pairing();

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
    // A pairing activity is declared: from its server/activate until the next, or until the
    // re-handshake after a pairing.
    [[nodiscard]] bool pairing() const;

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
    void send_state(SessionOutput& out);
    void send_clock(SessionOutput& out);
    [[nodiscard]] bool player_active() const;

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
    // Set once paired, until the server's re-handshake arrives or the wait times out.
    std::optional<std::int64_t> rehandshake_due_;

    ClockSync clock_;
    bool reported_available_ = false;
    bool sent_state_ = false;
    bool external_source_ = false;
    messages::PlayerState state_;
    std::optional<messages::PlayerStream> stream_;
};

}  // namespace ac3::sendspin
