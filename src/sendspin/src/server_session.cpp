#include "ac3/sendspin/server_session.hpp"

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

#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
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

[[nodiscard]] std::string_view family_of(std::string_view role) {
    return role.substr(0, role.find('@'));
}

[[nodiscard]] std::unexpected<Refusal> refuse(Refusal refusal) {
    return std::unexpected(refusal);
}

}  // namespace

ServerSession::ServerSession(ServerConfig config, const handshake::ServerKeyring& keyring,
                             ServerListener& listener, const Clock& clock)
    : config_(std::move(config)),
      listener_(&listener),
      clock_(&clock),
      phase_started_(clock.now_us()),
      initiator_(std::make_unique<handshake::Initiator>(config_.identity, keyring)) {}

ServerSession::~ServerSession() = default;

SessionOutput ServerSession::close_silently() {
    phase_ = Phase::kClosed;
    return {.frames = {}, .close = true};
}

bool ServerSession::seal_json(std::string_view json, SessionOutput& out) {
    if (!channel_) {
        return false;
    }
    std::vector<std::vector<std::uint8_t>> sealed;
    if (!channel_->seal_json(json, sealed)) {
        phase_ = Phase::kClosed;
        out.close = true;
        return false;
    }
    for (std::vector<std::uint8_t>& ciphertext : sealed) {
        out.binary(std::move(ciphertext));
    }
    return true;
}

std::expected<SessionOutput, Refusal> ServerSession::sent(SessionOutput out, bool ok) {
    if (!ok) {
        return refuse(Refusal::kCrypto);
    }
    return out;
}

SessionOutput ServerSession::receive(const transport::Frame& frame) {
    const std::int64_t arrival = clock_->now_us();
    if (phase_ == Phase::kClosed) {
        return {};
    }
    if (phase_ == Phase::kHandshake) {
        if (frame.kind != transport::FrameKind::kText || !initiator_) {
            return close_silently();
        }
        return on_handshake_text(frame.text());
    }
    if (frame.kind != transport::FrameKind::kBinary || !channel_) {
        return close_silently();
    }
    const Channel::Opened opened = channel_->open(frame.bytes);
    if (opened.error != Channel::OpenError::kNone) {
        return close_silently();
    }
    if (opened.message.empty() || opened.message.front() != message_id::kJson) {
        // No role this server activates sends binary data to it.
        return {};
    }
    return on_json(text_of(opened.message.subspan(1)), arrival);
}

SessionOutput ServerSession::on_handshake_text(std::string_view text) {
    const handshake::Step step = initiator_->receive(text);
    SessionOutput out;
    for (const std::string& reply : step.replies) {
        out.text(reply);
    }
    if (step.outcome == handshake::Outcome::kContinue) {
        phase_started_ = clock_->now_us();
        return out;
    }
    if (step.outcome == handshake::Outcome::kFailed) {
        phase_ = Phase::kClosed;
        out.close = true;
        return out;
    }
    out.append(established(*initiator_));
    return out;
}

SessionOutput ServerSession::established(handshake::Initiator& initiator) {
    std::optional<noise::Handshake::Transport> keys = initiator.take_keys();
    if (!keys) {
        return close_silently();
    }
    client_key_ = initiator.client_key();
    suite_ = initiator.suite();
    category_ = initiator.category();
    mismatch_ = initiator.credential_mismatch();
    if (channel_) {
        channel_->rekey(std::move(*keys));
    } else {
        channel_.emplace(std::move(*keys), dialect_, config_.max_message_bytes);
    }
    initiator_.reset();

    // A new session: nothing activated before it carries into it.
    hello_.reset();
    state_.reset();
    player_state_received_ = false;
    activities_.clear();
    active_roles_.clear();
    stream_.reset();

    SessionOutput out;
    if (!seal_json(m::write_server_hello({.name = config_.name, .languages = config_.languages}), out)) {
        return close_silently();
    }
    phase_ = Phase::kHello;
    phase_started_ = clock_->now_us();
    return out;
}

SessionOutput ServerSession::on_json(std::string_view text, std::int64_t arrival) {
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

    if (phase_ == Phase::kRehandshake) {
        // Application messages under the old keys, sent before the client saw message 1,
        // are tolerated and discarded (connection.md, Re-handshake).
        if (type != "noise/handshake" || !initiator_) {
            return {};
        }
        const handshake::Step step = initiator_->receive(text);
        if (step.outcome != handshake::Outcome::kEstablished) {
            return close_silently();
        }
        return established(*initiator_);
    }

    if (type == "client/goodbye") {
        if (const auto reason = m::read_client_goodbye(payload)) {
            listener_->on_goodbye(*reason);
        }
        phase_ = Phase::kClosed;
        return {.frames = {}, .close = true};
    }
    if (type == "client/hello") {
        if (phase_ != Phase::kHello) {
            return {};
        }
        dialect_ = m::client_hello_dialect(payload);
        std::expected<m::ClientHello, m::MessageError> hello = m::read_client_hello(payload, dialect_);
        if (!hello) {
            return close_silently();
        }
        channel_->set_dialect(dialect_);
        hello_ = std::move(*hello);
        phase_ = Phase::kReady;
        listener_->on_hello(*hello_);
        return {};
    }
    if (phase_ != Phase::kReady && phase_ != Phase::kActive) {
        return {};
    }
    if (type == "client/time") {
        const auto time = m::read_client_time(payload);
        if (!time) {
            return {};
        }
        SessionOutput out;
        // server_transmitted is taken as late as possible, just before sealing (messaging.md,
        // Transmit timestamps).
        const std::string reply = m::write_server_time({.client_transmitted = time->client_transmitted,
                                                        .server_received = arrival,
                                                        .server_transmitted = clock_->now_us()});
        if (!seal_json(reply, out)) {
            return close_silently();
        }
        return out;
    }
    if (type == "client/state") {
        std::expected<m::ClientState, m::MessageError> state = m::read_client_state(payload, dialect_);
        if (!state) {
            return {};
        }
        // aiosendspin 9.1.1 reports only what changed after its first report (C29), and may
        // leave out `available`.
        if (dialect_ == Dialect::kAiosendspin911 && state_) {
            if (!payload["available"].exists()) {
                state->available = state_->available;
            }
            if (state->player && state_->player) {
                m::PlayerState& next = *state->player;
                const m::PlayerState& previous = *state_->player;
                next.volume = next.volume ? next.volume : previous.volume;
                next.muted = next.muted ? next.muted : previous.muted;
                next.output_delay_ms = next.output_delay_ms ? next.output_delay_ms : previous.output_delay_ms;
                next.required_lead_time_ms =
                    next.required_lead_time_ms ? next.required_lead_time_ms : previous.required_lead_time_ms;
                next.min_buffer_ms = next.min_buffer_ms ? next.min_buffer_ms : previous.min_buffer_ms;
                if (!next.supported_commands) {
                    next.supported_commands = previous.supported_commands;
                }
            } else if (!state->player) {
                state->player = state_->player;
            }
        }
        if (state->player && player_active()) {
            player_state_received_ = true;
        }
        state_ = std::move(*state);
        listener_->on_state(*state_);
        return {};
    }
    if (type == "client/leave") {
        listener_->on_leave();
        return {};
    }
    return {};
}

bool ServerSession::player_active() const {
    return phase_ == Phase::kActive &&
           std::find(active_roles_.begin(), active_roles_.end(), kPlayerRole) != active_roles_.end();
}

std::expected<SessionOutput, Refusal> ServerSession::activate(const m::Activate& activate) {
    if ((phase_ != Phase::kReady && phase_ != Phase::kActive) || !hello_) {
        return refuse(Refusal::kNotReady);
    }
    const bool playback = contains(activate.activities, m::Activity::kPlayback);
    const bool pairing = contains(activate.activities, m::Activity::kPairing);
    if (contains(activate.activities, m::Activity::kOther) || (playback && pairing) || pairing) {
        // Pairing is not implemented in this session yet.
        return refuse(Refusal::kBadActivation);
    }
    const std::vector<std::string> roles = activate.active_roles.value_or(active_roles_);
    // Playback-capable: the sets messaging.md allows per matched PSK, with unpaired access
    // on the Sentinel only when the client offers it. The engine decides operator approval.
    const bool capable = category_ == PskCategory::kLongTerm ||
                         (category_ == PskCategory::kSentinel && hello_->unpaired_access && !mismatch_);
    if ((playback || !roles.empty()) && !capable) {
        return refuse(Refusal::kBadActivation);
    }
    std::vector<std::string_view> families;
    for (const std::string& role : roles) {
        const bool listed = std::find(hello_->supported_roles.begin(), hello_->supported_roles.end(), role) !=
                            hello_->supported_roles.end();
        const std::string_view family = family_of(role);
        if (!listed || std::find(families.begin(), families.end(), family) != families.end() ||
            (role == kPlayerRole && !hello_->player_support)) {
            return refuse(Refusal::kBadActivation);
        }
        families.push_back(family);
    }

    SessionOutput out;
    const bool keeps_player = std::find(roles.begin(), roles.end(), kPlayerRole) != roles.end();
    if (stream_ && !keeps_player) {
        // A role's stream ends before the role does (messaging.md, server/activate).
        if (!seal_json(m::write_stream_end({.roles = std::vector<std::string>{"player"}}), out)) {
            return refuse(Refusal::kCrypto);
        }
        stream_.reset();
    }
    m::Activate written = activate;
    // aiosendspin 9.1.1 takes a missing active_roles as the persisted roles even where the
    // specification would clear them, so they always go out (C11).
    written.active_roles = roles;
    if (!seal_json(m::write_activate(written, dialect_), out)) {
        return refuse(Refusal::kCrypto);
    }
    const bool had_player = player_active();
    activities_ = activate.activities;
    active_roles_ = roles;
    phase_ = Phase::kActive;
    if (!had_player || !keeps_player) {
        // A newly activated player owes a fresh client/state before its stream starts.
        player_state_received_ = false;
    }
    return out;
}

std::expected<SessionOutput, Refusal> ServerSession::start_stream(const m::PlayerStream& stream) {
    if (!player_active() || !player_state_received_ || !hello_ || !hello_->player_support) {
        return refuse(Refusal::kNoPlayerState);
    }
    if (!state_ || !state_->available) {
        return refuse(Refusal::kUnavailable);
    }
    const std::vector<m::AudioFormat>& formats = hello_->player_support->supported_formats;
    if (std::find(formats.begin(), formats.end(), stream.format) == formats.end()) {
        return refuse(Refusal::kFormatNotListed);
    }
    if (stream_ && dialect_ == Dialect::kAiosendspin911 && !(stream_->format == stream.format)) {
        return refuse(Refusal::kFormatChange);
    }
    SessionOutput out;
    const bool ok = seal_json(
        m::write_stream_start({.server_transmitted = clock_->now_us(), .player = stream}), out);
    if (ok) {
        stream_ = stream;
    }
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::send_audio(std::int64_t timestamp_us,
                                                                std::span<const std::uint8_t> frame) {
    if (!stream_ || !player_active()) {
        return refuse(Refusal::kNoStream);
    }
    const std::size_t header = audio_chunk_header_bytes(dialect_);
    std::vector<std::uint8_t> message(header + frame.size());
    std::copy(frame.begin(), frame.end(), message.begin() + static_cast<std::ptrdiff_t>(header));
    const std::uint32_t send_ahead = saturate_send_ahead(timestamp_us - clock_->now_us());
    if (!write_player_chunk_header(message, timestamp_us, send_ahead, dialect_)) {
        return refuse(Refusal::kCrypto);
    }
    std::vector<std::vector<std::uint8_t>> sealed;
    if (!channel_->seal(message, sealed)) {
        phase_ = Phase::kClosed;
        return refuse(Refusal::kCrypto);
    }
    SessionOutput out;
    for (std::vector<std::uint8_t>& ciphertext : sealed) {
        out.binary(std::move(ciphertext));
    }
    return out;
}

std::expected<SessionOutput, Refusal> ServerSession::clear_stream() {
    if (!stream_) {
        return refuse(Refusal::kNoStream);
    }
    SessionOutput out;
    const bool ok = seal_json(m::write_stream_clear({.server_transmitted = clock_->now_us(),
                                                     .roles = std::vector<std::string>{"player"}}),
                              out);
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::end_stream() {
    if (!stream_) {
        return refuse(Refusal::kNoStream);
    }
    SessionOutput out;
    const bool ok = seal_json(m::write_stream_end({.roles = std::vector<std::string>{"player"}}), out);
    stream_.reset();
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::update_group(const m::GroupUpdate& update) {
    if (phase_ != Phase::kActive) {
        return refuse(Refusal::kNotReady);
    }
    SessionOutput out;
    const bool ok = seal_json(m::write_group_update(update), out);
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::command(const m::PlayerCommandMessage& command) {
    if (!player_active() || !player_state_received_ || !state_ || !state_->player || !hello_ ||
        !hello_->player_support) {
        return refuse(Refusal::kNoPlayerState);
    }
    // Settable only when listed: in client/state, or for aiosendspin 9.1.1's volume and mute
    // in the hello's support object (C28, C29).
    const std::vector<m::PlayerCommand> none;
    const std::vector<m::PlayerCommand>& in_state =
        state_->player->supported_commands ? *state_->player->supported_commands : none;
    const std::vector<m::PlayerCommand>& in_hello = hello_->player_support->commands;
    const bool from_hello = dialect_ == Dialect::kAiosendspin911 && command.command != m::PlayerCommand::kSetOutputDelay;
    const std::vector<m::PlayerCommand>& listed = from_hello ? in_hello : in_state;
    if (std::find(listed.begin(), listed.end(), command.command) == listed.end()) {
        return refuse(Refusal::kCommandNotListed);
    }
    SessionOutput out;
    const bool ok = seal_json(m::write_server_command({.player = command}, dialect_), out);
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::unpair() {
    if (phase_ != Phase::kReady && phase_ != Phase::kActive) {
        return refuse(Refusal::kNotReady);
    }
    if (category_ != PskCategory::kLongTerm) {
        return refuse(Refusal::kNotPaired);
    }
    SessionOutput out;
    const bool ok = seal_json(m::write_server_unpair(), out);
    return sent(std::move(out), ok);
}

std::expected<SessionOutput, Refusal> ServerSession::rehandshake(const handshake::PskChoice& choice) {
    if ((phase_ != Phase::kReady && phase_ != Phase::kActive) || !channel_) {
        return refuse(Refusal::kNotReady);
    }
    initiator_ = std::make_unique<handshake::Initiator>(config_.identity, client_key_, suite_,
                                                         channel_->handshake_hash(), choice);
    const handshake::Step step = initiator_->start();
    if (step.outcome == handshake::Outcome::kFailed) {
        return refuse(Refusal::kCrypto);
    }
    SessionOutput out;
    for (const std::string& reply : step.replies) {
        if (!seal_json(reply, out)) {
            return refuse(Refusal::kCrypto);
        }
    }
    phase_ = Phase::kRehandshake;
    phase_started_ = clock_->now_us();
    return out;
}

SessionOutput ServerSession::tick() {
    const std::int64_t now = clock_->now_us();
    switch (phase_) {
        case Phase::kHandshake:
        case Phase::kRehandshake:
        case Phase::kHello:
            if (now - phase_started_ > kHandshakeTimeout) {
                return close_silently();
            }
            return {};
        case Phase::kReady:
        case Phase::kActive:
        case Phase::kClosed:
            return {};
    }
    return {};
}

}  // namespace ac3::sendspin
