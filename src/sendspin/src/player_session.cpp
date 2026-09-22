#include "ac3/sendspin/player_session.hpp"

#include <algorithm>
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
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac3/sendspin/transport.hpp"

namespace ac3::sendspin {

namespace {

namespace m = messages;
using handshake::PskCategory;

constexpr std::string_view kPlayerRole = "player@v1";
constexpr std::size_t kMaxTokens = 4096;

[[nodiscard]] std::string_view text_of(std::span<const std::uint8_t> bytes) {
    return {static_cast<const char*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

[[nodiscard]] bool contains(const std::vector<m::Activity>& activities, m::Activity activity) {
    return std::find(activities.begin(), activities.end(), activity) != activities.end();
}

// Whether `activities` (as three flags) is a set the matched PSK allows (messaging.md,
// server/activate).
[[nodiscard]] bool allowed_set(bool playback, bool pairing, bool other, PskCategory psk, bool unpaired_access) {
    if (other || (playback && pairing)) {
        return false;
    }
    switch (psk) {
        case PskCategory::kLongTerm:
            return !pairing;
        case PskCategory::kPairing:
            return !playback;
        case PskCategory::kSentinel:
            return !playback || unpaired_access;
    }
    return false;
}

// The messages pairing.md defines that a server sends.
[[nodiscard]] bool is_pairing_message(std::string_view type) {
    return type.starts_with("server/pair-") || type == "pair/abort";
}

}  // namespace

// The pairing flow's events, carried to the listener with the server they concern.
class PlayerSession::PairingEvents final : public pairing_flow::ClientPairingEvents {
   public:
    explicit PairingEvents(PlayerSession& session) : session_(&session) {}

    void on_code(const pairing_flow::Code& code) override { session_->listener_->on_pairing_code(code); }
    void on_held_back() override { session_->listener_->on_pairing_held_back(); }
    void on_paired(const crypto::Key32& long_term_psk) override {
        session_->listener_->on_paired(session_->server_key_, long_term_psk);
    }

   private:
    PlayerSession* session_;
};

PlayerSession::PlayerSession(PlayerConfig config, const handshake::ClientKeyring& keyring,
                             pairing_flow::ClientPairingState& pairing, PlayerListener& listener, const Clock& clock)
    : config_(std::move(config)),
      keyring_(&keyring),
      pairing_state_(&pairing),
      listener_(&listener),
      clock_source_(&clock),
      connection_(pairing.next_connection++),
      pairing_events_(std::make_unique<PairingEvents>(*this)),
      state_(config_.player_state),
      ac3forge_state_(config_.ac3forge_state),
      source_state_(config_.source_state),
      artwork_state_(config_.artwork_state),
      visualizer_state_(config_.visualizer_state) {}

PlayerSession::~PlayerSession() {
    // The session goes with its connection: a pairing window bound to it closes.
    attempt_.reset();
    pairing_state_->dropped(connection_);
}

SessionOutput PlayerSession::close_silently() {
    phase_ = Phase::kClosed;
    return {.frames = {}, .close = true};
}

void PlayerSession::seal(std::string_view json, SessionOutput& out) {
    if (!channel_) {
        out.close = true;
        return;
    }
    std::vector<std::vector<std::uint8_t>> sealed;
    if (!channel_->seal_json(json, sealed)) {
        phase_ = Phase::kClosed;
        out.close = true;
        return;
    }
    for (std::vector<std::uint8_t>& ciphertext : sealed) {
        out.binary(std::move(ciphertext));
    }
}

SessionOutput PlayerSession::open() {
    responder_ = std::make_unique<handshake::Responder>(config_.identity, config_.suite, *keyring_);
    phase_ = Phase::kHandshake;
    phase_started_ = clock_source_->now_us();
    SessionOutput out;
    out.text(responder_->client_init());
    return out;
}

SessionOutput PlayerSession::receive(const transport::Frame& frame) {
    return receive(frame, clock_source_->now_us());
}

SessionOutput PlayerSession::receive(const transport::Frame& frame, std::int64_t arrival) {
    switch (phase_) {
        case Phase::kClosed:
            return {};
        case Phase::kHandshake:
            if (frame.kind != transport::FrameKind::kText || !responder_) {
                return close_silently();
            }
            return on_handshake_text(frame.text());
        case Phase::kHello:
        case Phase::kProvisional:
        case Phase::kActive:
            break;
    }
    // Transport mode carries binary frames only; a cleartext one is a silent failure.
    if (frame.kind != transport::FrameKind::kBinary || !channel_) {
        return close_silently();
    }
    const Channel::Opened opened = channel_->open(frame.bytes);
    if (opened.error != Channel::OpenError::kNone) {
        return close_silently();
    }
    if (opened.message.empty()) {
        return {};
    }
    return on_message(opened.message, arrival);
}

SessionOutput PlayerSession::on_handshake_text(std::string_view text) {
    const handshake::Step step = responder_->receive(text);
    SessionOutput out;
    for (const std::string& reply : step.replies) {
        out.text(reply);
    }
    if (step.outcome == handshake::Outcome::kContinue) {
        phase_started_ = clock_source_->now_us();
        return out;
    }
    if (step.outcome == handshake::Outcome::kFailed) {
        phase_ = Phase::kClosed;
        out.close = true;
        return out;
    }
    std::optional<noise::Handshake::Transport> keys = responder_->take_keys();
    if (!keys) {
        return close_silently();
    }
    dialect_ = responder_->dialect();
    server_key_ = responder_->server_key();
    category_ = responder_->category();
    fell_back_ = responder_->fell_back();
    channel_.emplace(std::move(*keys), dialect_, config_.max_message_bytes);
    responder_.reset();
    phase_ = Phase::kHello;
    phase_started_ = clock_source_->now_us();
    return out;
}

SessionOutput PlayerSession::on_message(std::span<const std::uint8_t> message, std::int64_t arrival) {
    const std::uint8_t id = message.front();
    if (id == message_id::kJson) {
        return on_json(text_of(message.subspan(1)), arrival);
    }
    if (id == message_id::kPlayerAudio) {
        // Audio outside an active stream is the server's error; an unavailable player
        // discards it without closing (roles/player/v1.md, Audio Chunks).
        if (!player_active() || !stream_ || external_source_) {
            return {};
        }
        const auto chunk = parse_player_chunk(message, dialect_);
        if (!chunk) {
            return {};
        }
        if (clock_.updates() == 0) {
            // aiosendspin 9.1.1 starts a stream with the activation, so its first chunks can come
            // before the clock's first exchange; they wait for it, within the buffer's capacity.
            if (dialect_ == Dialect::kAiosendspin911 &&
                held_audio_bytes_ + chunk->data.size() <= config_.player_support.buffer_capacity) {
                held_audio_.emplace_back(chunk->timestamp_us,
                                         std::vector<std::uint8_t>(chunk->data.begin(), chunk->data.end()));
                held_audio_bytes_ += chunk->data.size();
            }
            return {};
        }
        deliver_audio(chunk->timestamp_us, chunk->data);
        return {};
    }
    if (id == message_id::kAc3forgeBurst) {
        // As player@v1's audio (planning/hearth-sendspin-extension.md, Burst chunks).
        if (!ac3forge_active() || !burst_stream_ || external_source_ || clock_.updates() == 0) {
            return {};
        }
        const auto chunk = parse_burst_chunk(message);
        if (!chunk || chunk->data_type() != ac3forge::burst_data_type(burst_stream_->data_type)) {
            listener_->on_invalid_burst();
            return {};
        }
        const std::int64_t delay = static_cast<std::int64_t>(ac3forge_state_.output_delay_ms) * 1000;
        listener_->on_burst(*chunk, clock_.to_local(chunk->chunk.timestamp_us) - delay);
        return {};
    }
    if (id >= message_id::kArtworkFirst && id <= message_id::kArtworkLast) {
        return on_artwork(message) ? SessionOutput{} : close_silently();
    }
    if (id >= message_id::kVisualizerFirst && id <= message_id::kVisualizerLast) {
        // As audio: outside a stream or while unavailable, a frame is discarded.
        if (!role_active(visualizer::kRole) || !visualizer_stream_ || external_source_ || clock_.updates() == 0) {
            return {};
        }
        const std::size_t bins =
            visualizer_stream_->spectrum ? static_cast<std::size_t>(visualizer_stream_->spectrum->n_disp_bins) : 0;
        if (const std::optional<visualizer::Frame> frame = visualizer::parse_frame(message, bins)) {
            listener_->on_visualizer_frame(*frame, clock_.to_local(frame->timestamp));
        }
        return {};
    }
    // No rule covers an unknown binary ID; it is ignored
    // (planning/hearth-sendspin-extension.md, Q2).
    return {};
}

bool PlayerSession::on_artwork(std::span<const std::uint8_t> message) {
    const auto parsed = artwork::parse_message(message);
    if (!parsed) {
        return false;
    }
    if (!artwork_stream_ || !role_active(artwork::kRole)) {
        return true;
    }
    // At most one transfer in flight, whose parts come on its channel and stop at its size
    // (roles/artwork/v1.md, Malformed sequences).
    switch (parsed->kind) {
        case artwork::Kind::kAnnounce:
            if (artwork_transfer_) {
                return false;
            }
            if (parsed->total_size > 0) {
                artwork_transfer_.emplace(parsed->channel, parsed->total_size);
            }
            break;
        case artwork::Kind::kPart:
            if (!artwork_transfer_ || artwork_transfer_->first != parsed->channel ||
                parsed->data.size() > artwork_transfer_->second) {
                return false;
            }
            artwork_transfer_->second -= static_cast<std::uint32_t>(parsed->data.size());
            if (artwork_transfer_->second == 0) {
                artwork_transfer_.reset();
            }
            break;
        case artwork::Kind::kCancel:
            if (artwork_transfer_ && artwork_transfer_->first == parsed->channel) {
                artwork_transfer_.reset();
            }
            break;
    }
    // An unavailable client discards the images but keeps count.
    if (!external_source_) {
        listener_->on_artwork_message(*parsed);
    }
    return true;
}

SessionOutput PlayerSession::on_json(std::string_view text, std::int64_t arrival) {
    std::vector<json::Token> tokens;
    json::Document document;
    if (!document.parse(text, tokens, kMaxTokens)) {
        return {};
    }
    const std::optional<m::Envelope> envelope = m::read_envelope(document);
    if (!envelope) {
        return {};
    }
    const std::string_view type = envelope->type;
    const json::Value payload = envelope->payload;
    SessionOutput out;

    if (type == "noise/handshake") {
        return on_rehandshake(text);
    }
    if (phase_ == Phase::kHello) {
        if (type != "server/hello") {
            return {};
        }
        std::expected<m::ServerHello, m::MessageError> hello = m::read_server_hello(payload);
        if (!hello) {
            return close_silently();
        }
        server_name_ = std::move(hello->name);
        const m::ClientHello client_hello{
            .name = config_.name,
            .device_info = config_.device_info,
            .supported_roles = config_.supported_roles,
            .player_support = config_.player_support,
            .ac3forge_support = config_.ac3forge_support,
            .pair_methods = config_.pair_methods,
            .unpaired_access = config_.unpaired_access,
            .trusts_server = category_ == PskCategory::kLongTerm,
            .source_support = config_.source_support,
            .visualizer_support = config_.visualizer_support,
        };
        seal(m::write_client_hello(client_hello, dialect_), out);
        phase_ = Phase::kProvisional;
        phase_started_ = clock_source_->now_us();
        return out;
    }

    if (type == "server/activate") {
        std::expected<m::Activate, m::MessageError> activate = m::read_activate(payload, dialect_);
        if (!activate) {
            return close_silently();
        }
        return on_activate(*activate);
    }
    if (phase_ != Phase::kActive) {
        return {};
    }

    if (is_pairing_message(type)) {
        if (!attempt_) {
            // Messages from an attempt that has ended are discarded until the next activation
            // (pairing.md, Entering and leaving pairing); with no pairing activation since the
            // handshake, one is out of sequence, a protocol error.
            return pairing_index_ > 0 ? SessionOutput{} : close_silently();
        }
        return pairing_step(attempt_->receive(type, payload, clock_source_->now_us()));
    }
    if (type == "server/time") {
        if (const auto time = m::read_server_time(payload)) {
            clock_.receive(*time, arrival);
            send_clock(out);
            if (clock_.updates() > 0 && !held_audio_.empty()) {
                std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> held;
                held.swap(held_audio_);
                held_audio_bytes_ = 0;
                for (const auto& [timestamp_us, frame] : held) {
                    deliver_audio(timestamp_us, frame);
                }
            }
        }
        return out;
    }
    if (type == "stream/start") {
        const auto start = m::read_stream_start(payload);
        if (!start) {
            return {};
        }
        if (start->player && player_active()) {
            // aiosendspin 9.1.1 changes a running stream's format expecting the player to
            // drop what it holds (C17).
            if (stream_ && dialect_ == Dialect::kAiosendspin911 && !(stream_->format == start->player->format)) {
                restart_audio();
                listener_->on_stream_clear();
            } else if (!stream_) {
                restart_audio();
            }
            stream_ = start->player;
            listener_->on_stream_start(*stream_);
        }
        if (start->ac3forge && ac3forge_active() && lists(*start->ac3forge)) {
            burst_stream_ = start->ac3forge;
            listener_->on_burst_stream_start(*burst_stream_);
        }
        if (start->artwork && role_active(artwork::kRole)) {
            // A channel whose configuration changes discards its pending image, and with it a
            // transfer in flight on it.
            if (artwork_stream_ && artwork_transfer_ &&
                artwork_stream_->at(artwork_transfer_->first) != start->artwork->at(artwork_transfer_->first)) {
                artwork_transfer_.reset();
            }
            artwork_stream_ = start->artwork;
            listener_->on_artwork_stream_start(*artwork_stream_);
        }
        if (start->visualizer && role_active(visualizer::kRole)) {
            visualizer_stream_ = start->visualizer;
            listener_->on_visualizer_stream_start(*visualizer_stream_);
        }
        return {};
    }
    // Whether a stream/clear or stream/end covers the role whose family is `family`.
    const auto names = [](const std::optional<std::vector<std::string>>& roles, std::string_view family) {
        return !roles || std::find(roles->begin(), roles->end(), family) != roles->end();
    };
    if (type == "stream/clear") {
        const auto clear = m::read_stream_clear(payload);
        if (clear && stream_ && names(clear->roles, "player")) {
            restart_audio();
            listener_->on_stream_clear();
        }
        if (clear && burst_stream_ && names(clear->roles, ac3forge::kObjectKey)) {
            listener_->on_burst_stream_clear();
        }
        if (clear && visualizer_stream_ && names(clear->roles, "visualizer")) {
            listener_->on_visualizer_stream_clear();
        }
        return {};
    }
    if (type == "stream/end") {
        const auto end = m::read_stream_end(payload);
        if (end && stream_ && names(end->roles, "player")) {
            stream_.reset();
            restart_audio();
            listener_->on_stream_end();
        }
        if (end && burst_stream_ && names(end->roles, ac3forge::kObjectKey)) {
            burst_stream_.reset();
            listener_->on_burst_stream_end();
        }
        if (end && artwork_stream_ && names(end->roles, "artwork")) {
            artwork_stream_.reset();
            artwork_transfer_.reset();
            listener_->on_artwork_stream_end();
        }
        if (end && visualizer_stream_ && names(end->roles, "visualizer")) {
            visualizer_stream_.reset();
            listener_->on_visualizer_stream_end();
        }
        return {};
    }
    if (type == "server/state") {
        auto state = m::read_server_state(payload);
        if (!state) {
            return {};
        }
        // Only the objects of roles that are active.
        if (!role_active(metadata::kRole)) {
            state->metadata.reset();
        }
        if (!role_active(controller::kRole)) {
            state->controller.reset();
        }
        if (!role_active(color::kRole)) {
            state->color.reset();
        }
        if (state->metadata || state->controller || state->color) {
            listener_->on_server_state(*state);
        }
        return {};
    }
    if (type == "server/command") {
        const auto command = m::read_server_command(payload, dialect_);
        if (!command) {
            return {};
        }
        if (command->player && player_active()) {
            // Only commands the player listed are applied; aiosendspin 9.1.1 lists volume and
            // mute in the hello's support object (C28).
            const m::PlayerCommand which = command->player->command;
            const std::vector<m::PlayerCommand> none;
            const std::vector<m::PlayerCommand>& state_list =
                state_.supported_commands ? *state_.supported_commands : none;
            const bool listed_in_state = std::find(state_list.begin(), state_list.end(), which) != state_list.end();
            const std::vector<m::PlayerCommand>& hello_list = config_.player_support.commands;
            const bool listed_in_hello = std::find(hello_list.begin(), hello_list.end(), which) != hello_list.end();
            const bool listed = dialect_ == Dialect::kSpecification || which == m::PlayerCommand::kSetOutputDelay
                                    ? listed_in_state
                                    : listed_in_hello;
            if (listed) {
                listener_->on_command(*command->player);
            }
        }
        if (ac3forge_active()) {
            // As player@v1: a command the role's latest state does not list is ignored.
            const std::vector<ac3forge::Command>& listed = ac3forge_state_.supported_commands;
            const auto lists_command = [&](ac3forge::Command which) {
                return std::find(listed.begin(), listed.end(), which) != listed.end();
            };
            if (command->ac3forge && lists_command(command->ac3forge->command)) {
                listener_->on_ac3forge_command(*command->ac3forge);
            }
            if (command->ac3forge_refused && lists_command(ac3forge::Command::kSettings)) {
                listener_->on_settings_refused(*command->ac3forge_refused);
            }
        }
        if (command->source && role_active(source::kRole)) {
            // A start while unavailable is ignored, and each command is idempotent
            // (roles/source/v1.md).
            if (*command->source == source::Command::kStart) {
                if (external_source_ || !clock_.converged() || source_started_) {
                    return {};
                }
                source_started_ = true;
                listener_->on_source_command(source::Command::kStart);
                return {};
            }
            if (!source_started_) {
                return {};
            }
            source_started_ = false;
            SessionOutput out_stop = end_source_stream();
            listener_->on_source_command(source::Command::kStop);
            return out_stop;
        }
        return {};
    }
    if (type == "group/update") {
        if (const auto update = m::read_group_update(payload, dialect_)) {
            listener_->on_group(*update);
        }
        return {};
    }
    if (type == "server/unpair") {
        // An unpaired session ignores it.
        if (category_ != PskCategory::kLongTerm) {
            return {};
        }
        listener_->on_unpaired(server_key_);
        return goodbye(m::GoodbyeReason::kUnpaired);
    }
    // Messages of roles this player does not have, pairing's, and types no rule covers.
    return {};
}

SessionOutput PlayerSession::on_activate(const m::Activate& activate) {
    SessionOutput out;
    const bool playback = contains(activate.activities, m::Activity::kPlayback);
    const bool pairing = contains(activate.activities, m::Activity::kPairing);
    const bool other = contains(activate.activities, m::Activity::kOther);
    const PskCategory psk = category_.value_or(PskCategory::kSentinel);
    const bool unpaired = config_.unpaired_access;

    // The roles this activation leaves in place: those it sends, or the persisted ones, or
    // none on a first activation (messaging.md, server/activate).
    const bool capable = allowed_set(true, pairing, other, psk, unpaired);
    std::vector<std::string> roles;
    if (activate.active_roles) {
        roles = *activate.active_roles;
    } else if (activations_ > 0 && capable) {
        roles = active_roles_;
    }
    const auto admissible = [&](bool with_unpaired_access) {
        const bool set_ok = allowed_set(playback, pairing, other, psk, with_unpaired_access);
        const bool roles_ok = roles.empty() || allowed_set(true, pairing, other, psk, with_unpaired_access);
        return set_ok && roles_ok;
    };
    if (!admissible(unpaired)) {
        end_pairing();
        if (psk == PskCategory::kSentinel && !unpaired && admissible(true)) {
            return goodbye(m::GoodbyeReason::kPairingRequired);
        }
        return goodbye(m::GoodbyeReason::kUnauthorized);
    }

    // An admissible activation ends any attempt in progress, and any wait for the re-handshake
    // after a pairing (pairing.md, Entering and leaving pairing).
    end_pairing();
    rehandshake_due_.reset();

    // The owner arbitrates between its servers (connection.md, Multiple servers).
    const bool first = activations_ == 0;
    if (!listener_->on_activation(server_key_, activate, first)) {
        if (pairing) {
            seal(pairing_messages::write_pair_abort(pairing_messages::AbortReason::kConcurrentAttempt, dialect_), out);
            phase_ = Phase::kClosed;
            out.close = true;
            return out;
        }
        return goodbye(m::GoodbyeReason::kConcurrentAttempt);
    }

    const bool had_player = player_active();
    const bool had_ac3forge = ac3forge_active();
    const std::vector<std::string> previous = phase_ == Phase::kActive ? active_roles_ : std::vector<std::string>{};
    activities_ = activate.activities;
    active_roles_ = std::move(roles);
    ++activations_;
    phase_ = Phase::kActive;
    const auto added = [&](std::string_view role) {
        return role_active(role) && std::find(previous.begin(), previous.end(), role) == previous.end();
    };

    if (had_player && !player_active() && stream_) {
        stream_.reset();
        restart_audio();
        listener_->on_stream_end();
    }
    if (had_ac3forge && !ac3forge_active() && burst_stream_) {
        burst_stream_.reset();
        listener_->on_burst_stream_end();
    }
    if (!role_active(artwork::kRole) && artwork_stream_) {
        artwork_stream_.reset();
        artwork_transfer_.reset();
        listener_->on_artwork_stream_end();
    }
    if (!role_active(visualizer::kRole) && visualizer_stream_) {
        visualizer_stream_.reset();
        listener_->on_visualizer_stream_end();
    }
    if (!role_active(source::kRole)) {
        // The start authorisation goes with the role, and an open input stream ends
        // (roles/source/v1.md).
        source_started_ = false;
        out.append(end_source_stream());
    }
    if (pairing) {
        // One attempt of the method named. A method the matched PSK disallows or this player
        // does not offer is answered with pair/abort method_not_supported, and the connection
        // stays open.
        ++pairing_index_;
        attempt_ = std::make_unique<pairing_flow::ClientPairing>(
            pairing_flow::ClientPairingConfig{.suite = config_.suite,
                                              .dialect = dialect_,
                                              .handshake_hash = channel_->handshake_hash(),
                                              .matched = psk,
                                              .offered = config_.pair_methods,
                                              .static_code = config_.static_code,
                                              .connection = connection_},
            *pairing_state_, *pairing_events_);
        out.append(pairing_step(attempt_->start(activate.pairing.value_or(m::PairingActivation{}), pairing_index_,
                                                clock_source_->now_us())));
        return out;
    }
    // A role with a state object that becomes active owes the server that object (messaging.md,
    // client/state).
    if (!active_roles_.empty() && (first || (player_active() && !had_player) || (ac3forge_active() && !had_ac3forge) ||
                                   added(artwork::kRole) || added(visualizer::kRole) || added(source::kRole) ||
                                   !sent_state_)) {
        send_state(out);
    }
    send_clock(out);
    return out;
}

SessionOutput PlayerSession::on_rehandshake(std::string_view text) {
    if (!channel_ || phase_ == Phase::kHandshake) {
        return close_silently();
    }
    handshake::Responder responder(config_.identity, config_.suite, server_key_, channel_->handshake_hash(),
                                   *keyring_);
    const handshake::Step step = responder.receive(text);
    if (step.outcome != handshake::Outcome::kEstablished) {
        return close_silently();
    }
    SessionOutput out;
    // Message 2 goes under the keys in use; the next frame uses the new ones.
    for (const std::string& reply : step.replies) {
        seal(reply, out);
    }
    std::optional<noise::Handshake::Transport> keys = responder.take_keys();
    if (!keys) {
        return close_silently();
    }
    channel_->rekey(std::move(*keys));
    category_ = responder.category();
    // The connection continues as after a first handshake: server/hello, client/hello,
    // server/activate; roles, activities and pairing from before do not carry into the new
    // session.
    end_pairing();
    rehandshake_due_.reset();
    pairing_index_ = 0;
    activations_ = 0;
    activities_.clear();
    active_roles_.clear();
    phase_ = Phase::kHello;
    phase_started_ = clock_source_->now_us();
    return out;
}

SessionOutput PlayerSession::pairing_step(pairing_flow::Step step) {
    SessionOutput out;
    for (const std::string& message : step.messages) {
        seal(message, out);
    }
    switch (step.after) {
        case pairing_flow::After::kContinue:
            break;
        case pairing_flow::After::kEnded:
            listener_->on_pairing_ended(attempt_ ? attempt_->aborted() : std::nullopt);
            break;
        case pairing_flow::After::kClose:
            listener_->on_pairing_ended(attempt_ ? attempt_->aborted() : std::nullopt);
            phase_ = Phase::kClosed;
            out.close = true;
            break;
        case pairing_flow::After::kPaired:
            // The record is persisted. Nothing more goes out until the server's re-handshake
            // arrives, which a Music Assistant server expects as the next frame (C6).
            rehandshake_due_ = clock_source_->now_us() + kHandshakeTimeout;
            break;
    }
    report_attempt();
    return out;
}

void PlayerSession::end_pairing() {
    if (!attempt_) {
        return;
    }
    if (!attempt_->finished()) {
        attempt_->abandon();
        listener_->on_pairing_ended(std::nullopt);
    }
    attempt_.reset();
    report_attempt();
}

void PlayerSession::report_attempt() {
    const bool in_progress = pairing_attempt_in_progress();
    if (in_progress != reported_attempt_) {
        reported_attempt_ = in_progress;
        listener_->on_pairing_attempt(in_progress);
    }
}

SessionOutput PlayerSession::displace() {
    if (phase_ == Phase::kClosed) {
        return {};
    }
    if (!pairing()) {
        end_pairing();
        return goodbye(m::GoodbyeReason::kAnotherServer);
    }
    SessionOutput out;
    if (attempt_ && !attempt_->finished()) {
        out = pairing_step(attempt_->abort(pairing_messages::AbortReason::kConcurrentAttempt));
    } else {
        seal(pairing_messages::write_pair_abort(pairing_messages::AbortReason::kConcurrentAttempt, dialect_), out);
    }
    phase_ = Phase::kClosed;
    out.close = true;
    return out;
}

bool PlayerSession::pairing() const {
    return phase_ == Phase::kActive && contains(activities_, m::Activity::kPairing);
}

bool PlayerSession::player_active() const {
    return phase_ == Phase::kActive &&
           std::find(active_roles_.begin(), active_roles_.end(), kPlayerRole) != active_roles_.end();
}

bool PlayerSession::ac3forge_active() const {
    return phase_ == Phase::kActive && config_.ac3forge_support &&
           std::find(active_roles_.begin(), active_roles_.end(), ac3forge::kRole) != active_roles_.end();
}

bool PlayerSession::role_active(std::string_view role) const {
    return phase_ == Phase::kActive && std::find(active_roles_.begin(), active_roles_.end(), role) != active_roles_.end();
}

bool PlayerSession::lists(const ac3forge::StreamStart& stream) const {
    if (!config_.ac3forge_support) {
        return false;
    }
    const ac3forge::Support& support = *config_.ac3forge_support;
    return std::find(support.data_types.begin(), support.data_types.end(), stream.data_type) !=
               support.data_types.end() &&
           std::find(support.sample_rates.begin(), support.sample_rates.end(), stream.sample_rate) !=
               support.sample_rates.end();
}

void PlayerSession::deliver_audio(std::int64_t timestamp_us, std::span<const std::uint8_t> frame) {
    // A chunk that does not start after the last one is a replay. aiosendspin 9.1.1 holds back
    // what it sends before an activation's first client/state, then sends it and replays the
    // stream from its start as well (C13); no conformant server sends one.
    if (last_audio_timestamp_ && timestamp_us <= *last_audio_timestamp_) {
        return;
    }
    last_audio_timestamp_ = timestamp_us;
    const std::int64_t delay = static_cast<std::int64_t>(state_.output_delay_ms.value_or(0)) * 1000;
    listener_->on_audio(frame, clock_.to_local(timestamp_us) - delay);
}

void PlayerSession::restart_audio() {
    held_audio_.clear();
    held_audio_bytes_ = 0;
    last_audio_timestamp_.reset();
}

bool PlayerSession::available_now() const {
    // aiosendspin 9.1.1 takes available: false for an external source, and its own client reports
    // available: true from activation on (C14).
    return (clock_.converged() || dialect_ == Dialect::kAiosendspin911) && !external_source_;
}

void PlayerSession::send_state(SessionOutput& out) {
    m::ClientState state;
    state.available = available_now();
    if (player_active()) {
        state.player = state_;
    }
    if (ac3forge_active()) {
        state.ac3forge = ac3forge_state_;
    }
    if (role_active(source::kRole)) {
        state.source = source_state_;
    }
    if (role_active(artwork::kRole)) {
        state.artwork = artwork_state_;
    }
    if (role_active(visualizer::kRole)) {
        state.visualizer = visualizer_state_;
    }
    seal(m::write_client_state(state, dialect_), out);
    reported_available_ = state.available;
    sent_state_ = true;
}

void PlayerSession::seal_binary(std::span<const std::uint8_t> message, SessionOutput& out) {
    if (!channel_) {
        out.close = true;
        return;
    }
    std::vector<std::vector<std::uint8_t>> sealed;
    if (!channel_->seal(message, sealed)) {
        phase_ = Phase::kClosed;
        out.close = true;
        return;
    }
    for (std::vector<std::uint8_t>& ciphertext : sealed) {
        out.binary(std::move(ciphertext));
    }
}

void PlayerSession::send_clock(SessionOutput& out) {
    if (const std::optional<m::ClientTime> request = clock_.poll(clock_source_->now_us())) {
        seal(m::write_client_time(*request), out);
    }
}

SessionOutput PlayerSession::tick() {
    const std::int64_t now = clock_source_->now_us();
    switch (phase_) {
        case Phase::kHandshake:
        case Phase::kHello:
            if (now - phase_started_ > kHandshakeTimeout) {
                return close_silently();
            }
            return {};
        case Phase::kProvisional:
            if (now - phase_started_ > kProvisionalTimeout) {
                return close_silently();
            }
            return {};
        case Phase::kActive: {
            if (pairing()) {
                // No clock exchanges or state reports while pairing (C25).
                if (rehandshake_due_ && now > *rehandshake_due_) {
                    return close_silently();
                }
                return attempt_ ? pairing_step(attempt_->tick(now)) : SessionOutput{};
            }
            SessionOutput out;
            send_clock(out);
            const bool available = available_now();
            if (sent_state_ && available != reported_available_) {
                send_state(out);
            }
            return out;
        }
        case Phase::kClosed:
            return {};
    }
    return {};
}

std::int64_t PlayerSession::next_tick_us() const {
    constexpr std::int64_t kIdle = 1'000'000;
    if (phase_ != Phase::kActive || pairing()) {
        return kIdle;
    }
    const std::int64_t now = clock_source_->now_us();
    const std::int64_t due = clock_.next_due();
    if (due <= now) {
        return 1'000;
    }
    return std::clamp<std::int64_t>(due - now, 1'000, kIdle);
}

SessionOutput PlayerSession::set_state(const m::PlayerState& state) {
    state_ = state;
    SessionOutput out;
    if (player_active() && sent_state_) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::set_ac3forge_state(const ac3forge::State& state) {
    ac3forge_state_ = state;
    SessionOutput out;
    if (ac3forge_active() && sent_state_) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::set_artwork_state(const artwork::Channels& channels) {
    artwork_state_ = channels;
    SessionOutput out;
    if (role_active(artwork::kRole) && sent_state_) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::set_visualizer_state(const visualizer::State& state) {
    visualizer_state_ = state;
    SessionOutput out;
    if (role_active(visualizer::kRole) && sent_state_) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::set_source_state(const source::State& state) {
    source_state_ = state;
    SessionOutput out;
    if (role_active(source::kRole) && sent_state_) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::send_command(const controller::CommandMessage& command) {
    SessionOutput out;
    if (role_active(controller::kRole) && !pairing()) {
        m::ClientCommand message;
        message.controller = command;
        seal(m::write_client_command(message), out);
    }
    return out;
}

SessionOutput PlayerSession::start_source_stream(const m::ClientStreamStart& start) {
    SessionOutput out;
    if (!role_active(source::kRole) || !source_started_ || external_source_ || !clock_.converged()) {
        return out;
    }
    seal(m::write_client_stream_start(start), out);
    source_stream_open_ = true;
    return out;
}

SessionOutput PlayerSession::send_source_audio(std::int64_t timestamp_us, std::span<const std::uint8_t> frame) {
    SessionOutput out;
    if (source_stream_open_ && role_active(source::kRole)) {
        seal_binary(source::write_chunk(timestamp_us, frame), out);
    }
    return out;
}

SessionOutput PlayerSession::end_source_stream() {
    SessionOutput out;
    if (source_stream_open_) {
        source_stream_open_ = false;
        seal(m::write_client_stream_end(), out);
    }
    return out;
}

SessionOutput PlayerSession::set_external_source(bool external) {
    external_source_ = external;
    SessionOutput out;
    if (external) {
        // A source ends its input stream before it reports itself unavailable, and needs a new
        // start after (roles/source/v1.md).
        out.append(end_source_stream());
        source_started_ = false;
    }
    // While pairing the change waits: tick() reports it once the activities change.
    if (phase_ == Phase::kActive && sent_state_ && !pairing()) {
        send_state(out);
    }
    return out;
}

SessionOutput PlayerSession::resume_pairing() {
    if (!attempt_ || !pairing()) {
        return {};
    }
    return pairing_step(attempt_->resume(clock_source_->now_us()));
}

SessionOutput PlayerSession::cancel_pairing() {
    if (!attempt_ || !pairing()) {
        return {};
    }
    return pairing_step(attempt_->cancel());
}

SessionOutput PlayerSession::goodbye(m::GoodbyeReason reason) {
    SessionOutput out;
    if (channel_ && phase_ != Phase::kHandshake && phase_ != Phase::kClosed) {
        seal(m::write_client_goodbye(reason), out);
    }
    phase_ = Phase::kClosed;
    out.close = true;
    return out;
}

}  // namespace ac3::sendspin
