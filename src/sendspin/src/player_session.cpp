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
      ac3forge_state_(config_.ac3forge_state) {}

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
    const std::int64_t arrival = clock_source_->now_us();
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
        if (!player_active() || !stream_ || external_source_ || clock_.updates() == 0) {
            return {};
        }
        const auto chunk = parse_player_chunk(message, dialect_);
        if (!chunk) {
            return {};
        }
        const std::int64_t delay = static_cast<std::int64_t>(state_.output_delay_ms.value_or(0)) * 1000;
        listener_->on_audio(chunk->data, clock_.to_local(chunk->timestamp_us) - delay);
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
    // No rule covers an unknown binary ID; it is ignored
    // (planning/hearth-sendspin-extension.md, Q2).
    return {};
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
                listener_->on_stream_clear();
            }
            stream_ = start->player;
            listener_->on_stream_start(*stream_);
        }
        if (start->ac3forge && ac3forge_active() && lists(*start->ac3forge)) {
            burst_stream_ = start->ac3forge;
            listener_->on_burst_stream_start(*burst_stream_);
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
            listener_->on_stream_clear();
        }
        if (clear && burst_stream_ && names(clear->roles, ac3forge::kObjectKey)) {
            listener_->on_burst_stream_clear();
        }
        return {};
    }
    if (type == "stream/end") {
        const auto end = m::read_stream_end(payload);
        if (end && stream_ && names(end->roles, "player")) {
            stream_.reset();
            listener_->on_stream_end();
        }
        if (end && burst_stream_ && names(end->roles, ac3forge::kObjectKey)) {
            burst_stream_.reset();
            listener_->on_burst_stream_end();
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
    activities_ = activate.activities;
    active_roles_ = std::move(roles);
    ++activations_;
    phase_ = Phase::kActive;

    if (had_player && !player_active() && stream_) {
        stream_.reset();
        listener_->on_stream_end();
    }
    if (had_ac3forge && !ac3forge_active() && burst_stream_) {
        burst_stream_.reset();
        listener_->on_burst_stream_end();
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
    if (!active_roles_.empty() &&
        (first || (player_active() && !had_player) || (ac3forge_active() && !had_ac3forge) || !sent_state_)) {
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

void PlayerSession::send_state(SessionOutput& out) {
    m::ClientState state;
    state.available = clock_.converged() && !external_source_;
    if (player_active()) {
        state.player = state_;
    }
    if (ac3forge_active()) {
        state.ac3forge = ac3forge_state_;
    }
    seal(m::write_client_state(state, dialect_), out);
    reported_available_ = state.available;
    sent_state_ = true;
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
            const bool available = clock_.converged() && !external_source_;
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

SessionOutput PlayerSession::set_external_source(bool external) {
    external_source_ = external;
    SessionOutput out;
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
