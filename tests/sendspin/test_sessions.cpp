#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/player_session.hpp"
#include "ac3/sendspin/server_session.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/transport.hpp"

// A server session and a player session joined by a simulated link: frames take a fixed time
// to cross, both sides are ticked as time advances, and the player's clock can run at an
// offset from the server's. From the handshake to audio chunks played at the right local
// time, and the paths that change a session midway: commands, unpairing, re-handshakes and
// pairing by each method, with its cancels and timeouts. _ac3forge_player@v1's bursts, commands
// and settings run over the same link.

namespace {

namespace ac = ac3::sendspin::ac3forge;
namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
namespace flow = ac3::sendspin::pairing_flow;
using ac3::sendspin::Clock;
using ac3::sendspin::PlayerConfig;
using ac3::sendspin::PlayerListener;
using ac3::sendspin::PlayerSession;
using ac3::sendspin::Refusal;
using ac3::sendspin::ServerConfig;
using ac3::sendspin::ServerListener;
using ac3::sendspin::ServerSession;
using ac3::sendspin::SessionOutput;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::pairing_messages::AbortReason;
using ac3::sendspin::transport::Frame;

class TestClock final : public Clock {
   public:
    TestClock(const std::int64_t& base, std::int64_t offset) : base_(&base), offset_(offset) {}
    [[nodiscard]] std::int64_t now_us() const override { return *base_ + offset_; }

   private:
    const std::int64_t* base_;
    std::int64_t offset_;
};

class ClientKeys final : public hs::ClientKeyring {
   public:
    std::vector<hs::PskCandidate> held;
    [[nodiscard]] std::optional<hs::PskCandidate> find(const Digest32& id,
                                                       std::optional<hs::PskCategory> category) const override {
        for (const hs::PskCandidate& candidate : held) {
            if ((!category || candidate.category == *category) && hs::psk_id(candidate.psk) == id) {
                return candidate;
            }
        }
        return std::nullopt;
    }
};

class ServerKeys final : public hs::ServerKeyring {
   public:
    hs::PskChoice choice = hs::sentinel_choice();
    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return choice; }
};

struct Paired {
    Key32 peer;
    Key32 psk;
};

struct PlayerEvents final : PlayerListener {
    struct Audio {
        std::vector<std::uint8_t> frame;
        std::int64_t local_time;
    };
    std::vector<m::PlayerStream> starts;
    int clears = 0;
    int ends = 0;
    std::vector<Audio> audio;
    std::vector<m::PlayerCommandMessage> commands;
    std::vector<m::GroupUpdate> groups;
    std::vector<Key32> unpaired;
    std::vector<flow::Code> codes;
    int held_back = 0;
    std::vector<Paired> paired;
    std::vector<std::optional<AbortReason>> ended;
    // Where a pairing's record goes, for the re-handshake that follows it.
    ClientKeys* keys = nullptr;
    // What on_activation answers, and what it was asked.
    bool admit = true;
    std::vector<bool> firsts;
    std::vector<bool> attempts;
    // _ac3forge_player@v1.
    struct Burst {
        std::uint16_t pc;
        std::uint16_t pd;
        std::vector<std::uint8_t> payload;
        std::int64_t local_time;
    };
    std::vector<ac::StreamStart> burst_starts;
    int burst_clears = 0;
    int burst_ends = 0;
    std::vector<Burst> bursts;
    int invalid_bursts = 0;
    std::vector<ac::CommandMessage> ac3forge_commands;
    std::vector<ac::SettingsError> refused_settings;

    bool on_activation(const Key32& /*server_key*/, const m::Activate& /*activate*/, bool first) override {
        firsts.push_back(first);
        return admit;
    }
    void on_pairing_attempt(bool in_progress) override { attempts.push_back(in_progress); }
    void on_stream_start(const m::PlayerStream& stream) override { starts.push_back(stream); }
    void on_stream_clear() override { ++clears; }
    void on_stream_end() override { ++ends; }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        audio.push_back({.frame = {frame.begin(), frame.end()}, .local_time = local_time});
    }
    void on_command(const m::PlayerCommandMessage& command) override { commands.push_back(command); }
    void on_group(const m::GroupUpdate& update) override { groups.push_back(update); }
    void on_unpaired(const Key32& server_key) override { unpaired.push_back(server_key); }
    void on_pairing_code(const flow::Code& code) override { codes.push_back(code); }
    void on_pairing_held_back() override { ++held_back; }
    void on_paired(const Key32& server_key, const Key32& long_term_psk) override {
        paired.push_back({.peer = server_key, .psk = long_term_psk});
        keys->held.push_back({.psk = long_term_psk, .category = hs::PskCategory::kLongTerm, .server_key = server_key});
    }
    void on_pairing_ended(std::optional<AbortReason> reason) override { ended.push_back(reason); }

    void on_burst_stream_start(const ac::StreamStart& stream) override { burst_starts.push_back(stream); }
    void on_burst_stream_clear() override { ++burst_clears; }
    void on_burst_stream_end() override { ++burst_ends; }
    void on_burst(const ac3::sendspin::BurstChunk& chunk, std::int64_t local_time) override {
        bursts.push_back({.pc = chunk.pc,
                          .pd = chunk.pd,
                          .payload = {chunk.chunk.data.begin(), chunk.chunk.data.end()},
                          .local_time = local_time});
    }
    void on_invalid_burst() override { ++invalid_bursts; }
    void on_ac3forge_command(const ac::CommandMessage& command) override { ac3forge_commands.push_back(command); }
    void on_settings_refused(const ac::SettingsError& error) override { refused_settings.push_back(error); }
};

struct ServerEvents final : ServerListener {
    std::vector<m::ClientHello> hellos;
    std::vector<m::ClientState> states;
    std::vector<m::GoodbyeReason> goodbyes;
    int leaves = 0;
    std::vector<std::optional<std::string>> held_back;
    int codes_wanted = 0;
    std::vector<Paired> paired;
    std::vector<std::optional<AbortReason>> ended;
    // Whether a pairing record can be stored.
    bool stores = true;

    void on_hello(const m::ClientHello& hello) override { hellos.push_back(hello); }
    void on_state(const m::ClientState& state) override { states.push_back(state); }
    void on_goodbye(m::GoodbyeReason reason) override { goodbyes.push_back(reason); }
    void on_leave() override { ++leaves; }
    void on_pairing_held_back(const std::optional<std::string>& message) override { held_back.push_back(message); }
    void on_pairing_code_wanted() override { ++codes_wanted; }
    bool on_paired(const Key32& client_key, const Key32& long_term_psk) override {
        if (stores) {
            paired.push_back({.peer = client_key, .psk = long_term_psk});
        }
        return stores;
    }
    void on_pairing_ended(std::optional<AbortReason> reason) override { ended.push_back(reason); }
};

ac3::sendspin::noise::KeyPair generated() {
    std::optional<ac3::sendspin::noise::KeyPair> pair = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

// The refusal an expected carries, or nothing when it holds a value.
std::optional<ac3::sendspin::Refusal> refusal(const std::expected<SessionOutput, ac3::sendspin::Refusal>& result) {
    if (result) {
        return std::nullopt;
    }
    return result.error();
}

const m::AudioFormat kPcm{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};

PlayerConfig player_config(bool unpaired_access) {
    PlayerConfig config;
    config.identity = generated();
    config.name = "Test sink";
    config.player_support = {.supported_formats = {kPcm}, .buffer_capacity = 1 << 20, .commands = {}};
    config.pair_methods = {{.method = m::PairMethod::kPairingPsk, .locations = {}, .out_channels = {},
                            .formats = {}, .min_pin_length = 0}};
    config.unpaired_access = unpaired_access;
    config.player_state = {.volume = 50,
                           .muted = false,
                           .output_delay_ms = 0,
                           .required_lead_time_ms = 200,
                           .min_buffer_ms = 100,
                           .supported_commands = std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume,
                                                                               m::PlayerCommand::kMute},
                           .format = std::nullopt};
    return config;
}

// A Hearth sink's offer: _ac3forge_player@v1 before player@v1, with unpaired access.
PlayerConfig extension_config() {
    PlayerConfig config = player_config(true);
    config.supported_roles = {"_ac3forge_player@v1", "player@v1"};
    ac::Support support;
    support.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    support.sample_rates = {48000};
    support.outputs.count = 2;
    support.outputs.bit_depth = 32;
    support.outputs.bit_depths = {16, 32};
    support.management.routing = true;
    support.management.trim_db = {-12.0, 12.0};
    support.management.max_delay_ms = 50.0;
    support.management.crossover_hz = {40.0, 250.0};
    support.management.identify = true;
    support.decoder_settings = {"mode", "drc_cut", "drc_boost"};
    support.buffer_capacity = 1 << 20;
    config.ac3forge_support = support;
    config.ac3forge_state.volume = 100;
    config.ac3forge_state.muted = false;
    config.ac3forge_state.required_lead_time_ms = 300;
    config.ac3forge_state.min_buffer_ms = 150;
    config.ac3forge_state.supported_commands = {ac::Command::kVolume, ac::Command::kMute,
                                                ac::Command::kSetOutputDelay, ac::Command::kSettings};
    return config;
}

// An E-AC-3 burst payload of `bytes` bytes that starts with a syncframe's sync word.
std::vector<std::uint8_t> eac3_payload(std::size_t bytes, std::uint8_t fill) {
    std::vector<std::uint8_t> payload(bytes, fill);
    payload[0] = 0x0B;
    payload[1] = 0x77;
    return payload;
}

m::Activate extension_playback() {
    return {.activities = {m::Activity::kPlayback},
            .active_roles = std::vector<std::string>{"_ac3forge_player@v1"},
            .pairing = std::nullopt};
}

// Both sessions over a simulated link.
struct Rig {
    std::int64_t now = 1'000'000;
    TestClock server_clock{now, 0};
    TestClock player_clock;
    ServerKeys server_keys;
    ClientKeys client_keys;
    ServerEvents server_events;
    PlayerEvents player_events;
    flow::ClientPairingState pairing_state;
    ac3::sendspin::noise::KeyPair server_identity = generated();
    ServerSession server;
    PlayerSession player;
    std::int64_t delay_us = 1'500;

    struct InFlight {
        Frame frame;
        std::int64_t at;
    };
    std::deque<InFlight> to_server;
    std::deque<InFlight> to_player;
    bool server_closed = false;
    bool player_closed = false;

    Rig(PlayerConfig config, std::int64_t player_offset, const hs::PskChoice& choice,
        std::vector<hs::PskCandidate> held)
        : player_clock(now, player_offset),
          server(ServerConfig{.identity = server_identity, .name = "Hearth", .languages = {"en"}, .max_message_bytes = 1 << 22},
                 server_keys, server_events, server_clock),
          player(std::move(config), client_keys, pairing_state, player_events, player_clock) {
        server_keys.choice = choice;
        client_keys.held = std::move(held);
        player_events.keys = &client_keys;
        from_player(player.open());
    }

    void from_server(SessionOutput out) {
        for (Frame& frame : out.frames) {
            to_player.push_back({.frame = std::move(frame), .at = now + delay_us});
        }
        server_closed = server_closed || out.close;
    }

    void from_player(SessionOutput out) {
        for (Frame& frame : out.frames) {
            to_server.push_back({.frame = std::move(frame), .at = now + delay_us});
        }
        player_closed = player_closed || out.close;
    }

    void send(std::expected<SessionOutput, ac3::sendspin::Refusal> out) {
        REQUIRE(out.has_value());
        from_server(std::move(*out));
    }

    // Advances time in `step` µs steps until `done` or `limit` more microseconds have passed.
    // Steps longer than the link's delay suit waits of minutes, for timeouts.
    bool run_until(const std::function<bool()>& done, std::int64_t limit, std::int64_t step = 250) {
        const std::int64_t end = now + limit;
        while (!done()) {
            if (now >= end) {
                return false;
            }
            while (!to_server.empty() && to_server.front().at <= now) {
                InFlight next = std::move(to_server.front());
                to_server.pop_front();
                if (!server_closed) {
                    from_server(server.receive(next.frame));
                }
            }
            while (!to_player.empty() && to_player.front().at <= now) {
                InFlight next = std::move(to_player.front());
                to_player.pop_front();
                if (!player_closed) {
                    from_player(player.receive(next.frame));
                }
            }
            // A side that closed ends the connection for the other once its frames are through.
            if ((server_closed && to_player.empty()) || (player_closed && to_server.empty())) {
                server_closed = player_closed = true;
            }
            if (!server_closed) {
                from_server(server.tick());
            }
            if (!player_closed) {
                from_player(player.tick());
            }
            now += step;
        }
        return true;
    }

    void run_for(std::int64_t duration) {
        (void)run_until([] { return false; }, duration);
    }

    [[nodiscard]] bool available() const {
        return !server_events.states.empty() && server_events.states.back().available;
    }
};

}  // namespace

TEST_CASE("sessions: an unpaired player on the Sentinel converges and plays PCM", "[sendspin][sessions]") {
    const std::int64_t offset = GENERATE(as<std::int64_t>{}, 0, 3'200'000'000, -45'000'000);
    Rig rig(player_config(true), offset, hs::sentinel_choice(), {});

    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    CHECK(rig.server.phase() == ServerSession::Phase::kReady);
    CHECK(rig.player.phase() == PlayerSession::Phase::kProvisional);
    CHECK(rig.server.psk_category() == hs::PskCategory::kSentinel);
    CHECK(rig.player.server_name() == "Hearth");
    CHECK(rig.server_events.hellos[0].name == "Test sink");
    CHECK(rig.server_events.hellos[0].unpaired_access);

    rig.send(rig.server.activate(
        {.activities = {m::Activity::kPlayback}, .active_roles = std::vector<std::string>{"player@v1"}, .pairing = std::nullopt}));
    // The first client/state goes out at once, unavailable until the clock converges.
    REQUIRE(rig.run_until([&] { return !rig.server_events.states.empty(); }, 1'000'000));
    CHECK_FALSE(rig.server_events.states.front().available);
    CHECK(refusal(rig.server.start_stream({.format = kPcm, .codec_header = {}})) == ac3::sendspin::Refusal::kUnavailable);

    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));
    CHECK(rig.player.clock_converged());

    rig.send(rig.server.start_stream({.format = kPcm, .codec_header = {}}));
    REQUIRE(rig.run_until([&] { return !rig.player_events.starts.empty(); }, 100'000));
    CHECK(rig.player_events.starts[0].format == kPcm);

    const std::int64_t first = rig.now + 500'000;
    for (int k = 0; k < 10; ++k) {
        const std::vector<std::uint8_t> frame(480 * 4, static_cast<std::uint8_t>(k));
        rig.send(rig.server.send_audio(first + (k * 10'000), frame));
    }
    REQUIRE(rig.run_until([&] { return rig.player_events.audio.size() == 10; }, 100'000));
    for (int k = 0; k < 10; ++k) {
        const PlayerEvents::Audio& audio = rig.player_events.audio[static_cast<std::size_t>(k)];
        CHECK(audio.frame.size() == 480 * 4);
        CHECK(audio.frame[0] == static_cast<std::uint8_t>(k));
        // The chunk's server timestamp, on the player's clock.
        const std::int64_t expected = first + (k * 10'000) + offset;
        CHECK(std::llabs(audio.local_time - expected) < 1'000);
    }

    rig.send(rig.server.end_stream());
    REQUIRE(rig.run_until([&] { return rig.player_events.ends == 1; }, 100'000));
    CHECK_FALSE(rig.player.streaming());
    CHECK(refusal(rig.server.send_audio(rig.now, std::vector<std::uint8_t>(4))) == ac3::sendspin::Refusal::kNoStream);
}

TEST_CASE("sessions: commands only when listed, and the state that answers them", "[sendspin][sessions]") {
    Rig rig(player_config(true), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    rig.send(rig.server.activate(
        {.activities = {m::Activity::kPlayback}, .active_roles = std::vector<std::string>{"player@v1"}, .pairing = std::nullopt}));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));

    rig.send(rig.server.command({.command = m::PlayerCommand::kVolume, .volume = 30, .mute = false, .output_delay_ms = 0}));
    CHECK(refusal(rig.server.command(
              {.command = m::PlayerCommand::kSetOutputDelay, .volume = 0, .mute = false, .output_delay_ms = 20})) ==
          ac3::sendspin::Refusal::kCommandNotListed);
    REQUIRE(rig.run_until([&] { return !rig.player_events.commands.empty(); }, 100'000));
    CHECK(rig.player_events.commands[0].volume == 30);

    m::PlayerState changed = player_config(true).player_state;
    changed.volume = 30;
    rig.from_player(rig.player.set_state(changed));
    REQUIRE(rig.run_until([&] { return rig.server_events.states.back().player && rig.server_events.states.back().player->volume == 30; },
                          100'000));

    rig.send(rig.server.update_group({.playback_state = m::PlaybackState::kStopped, .group_id = "g", .group_name = "Kitchen"}));
    REQUIRE(rig.run_until([&] { return !rig.player_events.groups.empty(); }, 100'000));
    CHECK(rig.player_events.groups[0].group_name == "Kitchen");

    // Output taken by something else: available false, and the stream is refused.
    rig.from_player(rig.player.set_external_source(true));
    REQUIRE(rig.run_until([&] { return !rig.available(); }, 100'000));
    CHECK(refusal(rig.server.start_stream({.format = kPcm, .codec_header = {}})) == ac3::sendspin::Refusal::kUnavailable);
}

TEST_CASE("sessions: a player without unpaired access gets no playback on the Sentinel", "[sendspin][sessions]") {
    Rig rig(player_config(false), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    CHECK(refusal(rig.server.activate({.activities = {m::Activity::kPlayback},
                                       .active_roles = std::vector<std::string>{"player@v1"},
                                       .pairing = std::nullopt})) == ac3::sendspin::Refusal::kBadActivation);
    // An empty activation is allowed, and holds the connection.
    rig.send(rig.server.activate({.activities = {}, .active_roles = std::vector<std::string>{}, .pairing = std::nullopt}));
    REQUIRE(rig.run_until([&] { return rig.player.phase() == PlayerSession::Phase::kActive; }, 100'000));
    rig.run_for(100'000);
    CHECK(rig.server_events.states.empty());
    CHECK_FALSE(rig.player_closed);
}

namespace {

Key32 random_key() {
    Key32 key{};
    REQUIRE(ac3::sendspin::crypto::random_bytes(key));
    return key;
}

}  // namespace

TEST_CASE("sessions: a paired player plays without unpaired access, and the server unpairs it",
          "[sendspin][sessions]") {
    const Key32 psk = random_key();
    Rig rig(player_config(false), 0, {.psk = psk, .category = hs::PskCategory::kLongTerm}, {});
    // The player's key ring is read when message 1 arrives, so the record can be bound to the
    // server's key now that the rig has made it.
    rig.client_keys.held.push_back(
        {.psk = psk, .category = hs::PskCategory::kLongTerm, .server_key = rig.server_identity.public_key()});

    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    CHECK(rig.player.psk_category() == hs::PskCategory::kLongTerm);
    CHECK_FALSE(rig.player.fell_back());
    rig.send(rig.server.activate(
        {.activities = {m::Activity::kPlayback}, .active_roles = std::vector<std::string>{"player@v1"}, .pairing = std::nullopt}));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));

    rig.send(rig.server.unpair());
    REQUIRE(rig.run_until([&] { return !rig.server_events.goodbyes.empty(); }, 100'000));
    CHECK(rig.server_events.goodbyes[0] == m::GoodbyeReason::kUnpaired);
    REQUIRE(rig.player_events.unpaired.size() == 1);
    CHECK(rig.player_events.unpaired[0] == rig.server_identity.public_key());
    CHECK(rig.player.phase() == PlayerSession::Phase::kClosed);
    CHECK(rig.server.phase() == ServerSession::Phase::kClosed);
}

TEST_CASE("sessions: a record the player lost gives the server the mismatch signal", "[sendspin][sessions]") {
    Rig rig(player_config(true), 0, {.psk = random_key(), .category = hs::PskCategory::kLongTerm}, {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    CHECK(rig.player.fell_back());
    CHECK(rig.server.credential_mismatch());
    CHECK(rig.server.psk_category() == hs::PskCategory::kSentinel);
    // While the server holds its record, no roles and no playback, unpaired access or not.
    CHECK(refusal(rig.server.activate({.activities = {m::Activity::kPlayback},
                                       .active_roles = std::vector<std::string>{"player@v1"},
                                       .pairing = std::nullopt})) == ac3::sendspin::Refusal::kBadActivation);
}

TEST_CASE("sessions: a long-term record bound to another server fails the handshake", "[sendspin][sessions]") {
    const Key32 psk = random_key();
    Rig rig(player_config(true), 0, {.psk = psk, .category = hs::PskCategory::kLongTerm},
            {{.psk = psk, .category = hs::PskCategory::kLongTerm, .server_key = random_key()}});
    REQUIRE(rig.run_until([&] { return rig.player_closed; }, 1'000'000));
    CHECK(rig.server_events.hellos.empty());
    CHECK(rig.player.phase() == PlayerSession::Phase::kClosed);
}

TEST_CASE("sessions: a re-handshake from the Sentinel to a long-term PSK", "[sendspin][sessions]") {
    Rig rig(player_config(true), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    rig.send(rig.server.activate(
        {.activities = {m::Activity::kPlayback}, .active_roles = std::vector<std::string>{"player@v1"}, .pairing = std::nullopt}));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));

    // As after a pairing: both now hold the same long-term PSK.
    const Key32 psk = random_key();
    rig.client_keys.held.push_back(
        {.psk = psk, .category = hs::PskCategory::kLongTerm, .server_key = rig.server_identity.public_key()});
    rig.send(rig.server.rehandshake({.psk = psk, .category = hs::PskCategory::kLongTerm}));
    CHECK(refusal(rig.server.activate({.activities = {}, .active_roles = std::nullopt, .pairing = std::nullopt})) ==
          ac3::sendspin::Refusal::kNotReady);
    REQUIRE(rig.run_until([&] { return rig.server_events.hellos.size() == 2; }, 1'000'000));
    CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    CHECK(rig.player.psk_category() == hs::PskCategory::kLongTerm);
    CHECK_FALSE(rig.server.credential_mismatch());

    const std::size_t states = rig.server_events.states.size();
    rig.send(rig.server.activate(
        {.activities = {m::Activity::kPlayback}, .active_roles = std::vector<std::string>{"player@v1"}, .pairing = std::nullopt}));
    REQUIRE(rig.run_until([&] { return rig.server_events.states.size() > states && rig.available(); }, 1'000'000));
    rig.send(rig.server.start_stream({.format = kPcm, .codec_header = {}}));
    rig.send(rig.server.send_audio(rig.now + 300'000, std::vector<std::uint8_t>(8, 1)));
    REQUIRE(rig.run_until([&] { return rig.player_events.audio.size() == 1; }, 100'000));
}

namespace {

const m::PairMethodDescriptor kDynamicDigits{.method = m::PairMethod::kDynamicCode,
                                             .locations = {},
                                             .out_channels = {m::OutChannel::kDisplay},
                                             .formats = {m::CodeFormat::kDigits},
                                             .min_pin_length = 6};
const m::PairMethodDescriptor kStaticOnDevice{.method = m::PairMethod::kStaticCode,
                                              .locations = {m::SecretLocation::kDevice},
                                              .out_channels = {},
                                              .formats = {},
                                              .min_pin_length = 0};

m::Activate pairing_activation(m::PairMethod method, std::optional<m::CodeFormat> format = std::nullopt) {
    return {.activities = {m::Activity::kPairing},
            .active_roles = std::vector<std::string>{},
            .pairing = m::PairingActivation{.method = method, .format = format, .pin_length = 0, .languages = {}}};
}

m::Activate playback_activation() {
    return {.activities = {m::Activity::kPlayback},
            .active_roles = std::vector<std::string>{"player@v1"},
            .pairing = std::nullopt};
}

m::Activate empty_activation() {
    return {.activities = {}, .active_roles = std::vector<std::string>{}, .pairing = std::nullopt};
}

std::string digits_of(const flow::Code& code) {
    REQUIRE(std::holds_alternative<std::string>(code));
    return std::get<std::string>(code);
}

std::string mistyped(std::string code) {
    code[0] = code[0] == '9' ? '0' : static_cast<char>(code[0] + 1);
    return code;
}

}  // namespace

TEST_CASE("sessions: pairing with the pairing PSK, then playback on the long-term PSK", "[sendspin][sessions]") {
    const Key32 pairing_psk = random_key();
    Rig rig(player_config(false), 0, {.psk = pairing_psk, .category = hs::PskCategory::kPairing},
            {{.psk = pairing_psk, .category = hs::PskCategory::kPairing, .server_key = {}}});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    CHECK(rig.server.psk_category() == hs::PskCategory::kPairing);
    // The pairing PSK is for pairing_psk alone: no playback, and no code method.
    CHECK(refusal(rig.server.activate(playback_activation())) == Refusal::kBadActivation);
    CHECK(refusal(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits))) ==
          Refusal::kBadActivation);

    rig.send(rig.server.activate(pairing_activation(m::PairMethod::kPairingPsk)));
    // The client delivers a PSK; the server stores it, acknowledges, and re-handshakes to it.
    REQUIRE(rig.run_until([&] { return rig.server_events.hellos.size() == 2; }, 1'000'000));
    REQUIRE(rig.server_events.paired.size() == 1);
    REQUIRE(rig.player_events.paired.size() == 1);
    CHECK(rig.server_events.paired[0].peer == rig.server.client_key());
    CHECK(rig.player_events.paired[0].peer == rig.server_identity.public_key());
    CHECK(rig.server_events.paired[0].psk == rig.player_events.paired[0].psk);
    CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    CHECK(rig.player.psk_category() == hs::PskCategory::kLongTerm);
    CHECK_FALSE(rig.player.pairing());
    CHECK(rig.player_events.ended.empty());
    CHECK(rig.server_events.ended.empty());

    // Paired: no pairing on the long-term PSK, and playback without unpaired access.
    CHECK(refusal(rig.server.activate(pairing_activation(m::PairMethod::kPairingPsk))) == Refusal::kBadActivation);
    rig.send(rig.server.activate(playback_activation()));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));
}

TEST_CASE("sessions: a dynamic code typed wrong, then right, pairs on the Sentinel", "[sendspin][sessions]") {
    PlayerConfig config = player_config(false);
    config.pair_methods.push_back(kDynamicDigits);
    Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    // On the Sentinel a code method, not pairing_psk, and only what the client offers.
    CHECK(refusal(rig.server.activate(pairing_activation(m::PairMethod::kPairingPsk))) == Refusal::kBadActivation);
    CHECK(refusal(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kQrCode))) ==
          Refusal::kBadActivation);
    CHECK(refusal(rig.server.activate(pairing_activation(m::PairMethod::kStaticCode))) == Refusal::kBadActivation);
    CHECK(refusal(rig.server.enter_code(std::string("123456"))) == Refusal::kNoAttempt);

    rig.send(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)));
    REQUIRE(rig.run_until([&] { return rig.server.pairing_wants_code() && rig.player_events.codes.size() == 1; },
                          1'000'000));
    CHECK(rig.server_events.codes_wanted == 1);
    const std::string code = digits_of(rig.player_events.codes[0]);
    CHECK(refusal(rig.server.enter_code(std::string("12345"))) == Refusal::kBadCode);

    rig.send(rig.server.enter_code(mistyped(code)));
    REQUIRE(rig.run_until([&] { return rig.server.pairing_wants_code() && rig.player_events.codes.size() == 2; },
                          1'000'000));
    CHECK(rig.server_events.codes_wanted == 2);
    CHECK(digits_of(rig.player_events.codes[1]) == code);
    // While pairing the player reports no state.
    CHECK(rig.server_events.states.empty());

    rig.send(rig.server.enter_code(code));
    REQUIRE(rig.run_until([&] { return rig.server_events.hellos.size() == 2; }, 1'000'000));
    CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    CHECK(rig.player.psk_category() == hs::PskCategory::kLongTerm);
    CHECK(rig.pairing_state.rounds_since_verified == 0);
    CHECK(rig.server_events.ended.empty());
}

TEST_CASE("sessions: a static code waits for the gesture on the device", "[sendspin][sessions]") {
    PlayerConfig config = player_config(false);
    config.pair_methods.push_back(kStaticOnDevice);
    config.static_code = "20260915";
    Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));

    rig.send(rig.server.activate(pairing_activation(m::PairMethod::kStaticCode)));
    REQUIRE(rig.run_until([&] { return rig.server_events.held_back.size() == 1; }, 1'000'000));
    CHECK(rig.player_events.held_back == 1);
    CHECK_FALSE(rig.server_events.held_back[0].has_value());
    CHECK(refusal(rig.server.enter_code(std::string("20260915"))) == Refusal::kNoAttempt);
    // Without the gesture, resuming changes nothing.
    CHECK(rig.player.resume_pairing().frames.empty());

    rig.pairing_state.open_window(rig.now);
    rig.from_player(rig.player.resume_pairing());
    REQUIRE(rig.run_until([&] { return rig.server.pairing_wants_code(); }, 1'000'000));
    rig.send(rig.server.enter_code(std::string("20260915")));
    REQUIRE(rig.run_until([&] { return rig.server_events.hellos.size() == 2; }, 1'000'000));
    CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    // A completed pairing closes the window.
    CHECK_FALSE(rig.pairing_state.window_opened_at.has_value());
}

TEST_CASE("sessions: pairing cancelled on either side, superseded, or timed out by the player",
          "[sendspin][sessions]") {
    PlayerConfig config = player_config(false);
    config.pair_methods.push_back(kDynamicDigits);
    Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    rig.send(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)));
    REQUIRE(rig.run_until([&] { return rig.server.pairing_wants_code() && !rig.player_events.codes.empty(); },
                          1'000'000));
    CHECK(rig.player.pairing());

    SECTION("the server's operator cancels") {
        rig.send(rig.server.cancel_pairing());
        CHECK_FALSE(rig.server.pairing());
        REQUIRE(rig.run_until([&] { return !rig.player.pairing(); }, 100'000));
        REQUIRE(rig.player_events.ended.size() == 1);
        CHECK(rig.player_events.ended[0] == std::optional<AbortReason>(AbortReason::kUserCancelled));
        CHECK(rig.player.phase() == PlayerSession::Phase::kActive);
        CHECK(refusal(rig.server.cancel_pairing()) == Refusal::kNoAttempt);
    }
    SECTION("the device's operator cancels") {
        rig.from_player(rig.player.cancel_pairing());
        REQUIRE(rig.run_until([&] { return !rig.server_events.ended.empty(); }, 100'000));
        CHECK(rig.server_events.ended[0] == std::optional<AbortReason>(AbortReason::kUserCancelled));
        CHECK_FALSE(rig.server.pairing_wants_code());
        // The round had begun, so it counts toward the limit.
        CHECK(rig.pairing_state.rounds_since_verified == 1);
        rig.send(rig.server.activate(empty_activation()));
        REQUIRE(rig.run_until([&] { return !rig.player.pairing(); }, 100'000));
        CHECK(rig.player_events.ended.size() == 1);
    }
    SECTION("a new pairing activation supersedes the attempt") {
        rig.send(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)));
        REQUIRE(rig.run_until([&] { return rig.player_events.codes.size() == 2 && rig.server.pairing_wants_code(); },
                              1'000'000));
        REQUIRE(rig.player_events.ended.size() == 1);
        CHECK_FALSE(rig.player_events.ended[0].has_value());
        rig.send(rig.server.enter_code(rig.player_events.codes[1]));
        REQUIRE(rig.run_until([&] { return rig.server_events.hellos.size() == 2; }, 1'000'000));
        CHECK(rig.server.psk_category() == hs::PskCategory::kLongTerm);
    }
    SECTION("the player's attempt timeout") {
        REQUIRE(rig.run_until([&] { return !rig.server_events.ended.empty(); }, 130'000'000, 100'000));
        CHECK(rig.server_events.ended[0] == std::optional<AbortReason>(AbortReason::kAttemptTimeout));
        REQUIRE(rig.player_events.ended.size() == 1);
        CHECK(rig.player_events.ended[0] == std::optional<AbortReason>(AbortReason::kAttemptTimeout));
        CHECK_FALSE(rig.player_closed);
    }
}

TEST_CASE("sessions: the server's own pairing timeout, and a record it cannot store", "[sendspin][sessions]") {
    SECTION("no gesture before the start timeout") {
        PlayerConfig config = player_config(false);
        config.pair_methods.push_back(kStaticOnDevice);
        config.static_code = "20260915";
        Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(pairing_activation(m::PairMethod::kStaticCode)));
        REQUIRE(rig.run_until([&] { return !rig.server_events.ended.empty(); },
                              ServerSession::kPairingStartTimeout + 1'000'000, 100'000));
        CHECK(rig.server_events.ended[0] == std::optional<AbortReason>(AbortReason::kAttemptTimeout));
        REQUIRE(rig.run_until([&] { return !rig.player.pairing(); }, 1'000'000));
        REQUIRE(rig.player_events.ended.size() == 1);
        CHECK_FALSE(rig.player_events.ended[0].has_value());
    }
    SECTION("the record cannot be stored") {
        const Key32 pairing_psk = random_key();
        Rig rig(player_config(false), 0, {.psk = pairing_psk, .category = hs::PskCategory::kPairing},
                {{.psk = pairing_psk, .category = hs::PskCategory::kPairing, .server_key = {}}});
        rig.server_events.stores = false;
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(pairing_activation(m::PairMethod::kPairingPsk)));
        REQUIRE(rig.run_until([&] { return rig.player_closed; }, 1'000'000));
        CHECK(rig.player_events.paired.empty());
        CHECK(rig.server.phase() == ServerSession::Phase::kClosed);
    }
}

TEST_CASE("sessions: the owner rejects an activation, or another server displaces the connection",
          "[sendspin][sessions]") {
    PlayerConfig config = player_config(true);
    config.pair_methods.push_back(kDynamicDigits);

    SECTION("a rejected playback activation") {
        Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
        rig.player_events.admit = false;
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(playback_activation()));
        REQUIRE(rig.run_until([&] { return !rig.server_events.goodbyes.empty(); }, 100'000));
        CHECK(rig.server_events.goodbyes[0] == m::GoodbyeReason::kConcurrentAttempt);
        CHECK(rig.player_events.firsts == std::vector<bool>{true});
        CHECK(rig.player.phase() == PlayerSession::Phase::kClosed);
    }
    SECTION("a rejected pairing activation") {
        Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
        rig.player_events.admit = false;
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)));
        REQUIRE(rig.run_until([&] { return !rig.server_events.ended.empty(); }, 100'000));
        CHECK(rig.server_events.ended[0] == std::optional<AbortReason>(AbortReason::kConcurrentAttempt));
        CHECK(rig.player_events.codes.empty());
        CHECK(rig.server.phase() == ServerSession::Phase::kClosed);
    }
    SECTION("displaced while playing") {
        Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(playback_activation()));
        REQUIRE(rig.run_until([&] { return !rig.server_events.states.empty(); }, 1'000'000));
        rig.send(rig.server.activate(playback_activation()));
        REQUIRE(rig.run_until([&] { return rig.player_events.firsts.size() == 2; }, 100'000));
        CHECK(rig.player_events.firsts == std::vector<bool>{true, false});
        rig.from_player(rig.player.displace());
        REQUIRE(rig.run_until([&] { return !rig.server_events.goodbyes.empty(); }, 100'000));
        CHECK(rig.server_events.goodbyes[0] == m::GoodbyeReason::kAnotherServer);
    }
    SECTION("displaced during a pairing attempt") {
        Rig rig(std::move(config), 0, hs::sentinel_choice(), {});
        REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
        rig.send(rig.server.activate(pairing_activation(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)));
        REQUIRE(rig.run_until([&] { return rig.server.pairing_wants_code(); }, 1'000'000));
        CHECK(rig.player_events.attempts == std::vector<bool>{true});
        CHECK(rig.player.pairing_attempt_in_progress());
        rig.from_player(rig.player.displace());
        REQUIRE(rig.run_until([&] { return !rig.server_events.ended.empty(); }, 100'000));
        CHECK(rig.server_events.ended[0] == std::optional<AbortReason>(AbortReason::kConcurrentAttempt));
        CHECK(rig.player_events.attempts == std::vector<bool>{true, false});
        CHECK(rig.player_events.ended == std::vector<std::optional<AbortReason>>{AbortReason::kConcurrentAttempt});
    }
}

TEST_CASE("sessions: _ac3forge_player@v1 streams bursts on the player's clock", "[sendspin][sessions][ac3forge]") {
    const std::int64_t offset = GENERATE(as<std::int64_t>{}, 0, -45'000'000);
    Rig rig(extension_config(), offset, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    REQUIRE(rig.server_events.hellos[0].ac3forge_support.has_value());
    CHECK(rig.server_events.hellos[0].supported_roles == std::vector<std::string>{"_ac3forge_player@v1", "player@v1"});

    rig.send(rig.server.activate(extension_playback()));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));
    // The state carries the role's object, and not player@v1's.
    CHECK(rig.server_events.states.back().ac3forge.has_value());
    CHECK_FALSE(rig.server_events.states.back().player.has_value());
    CHECK(refusal(rig.server.start_stream({.format = kPcm, .codec_header = {}})) == Refusal::kNoPlayerState);
    CHECK(refusal(rig.server.start_burst_stream({.data_type = ac::DataType::kEac3, .sample_rate = 44100})) ==
          Refusal::kFormatNotListed);
    CHECK(refusal(rig.server.send_burst(rig.now, 21, 8, eac3_payload(8, 0))) == Refusal::kNoStream);

    rig.send(rig.server.start_burst_stream({.data_type = ac::DataType::kEac3, .sample_rate = 48000}));
    REQUIRE(rig.run_until([&] { return !rig.player_events.burst_starts.empty(); }, 100'000));
    CHECK(rig.player_events.burst_starts[0].data_type == ac::DataType::kEac3);
    CHECK(rig.player.burst_streaming());
    CHECK(rig.player_events.starts.empty());

    // What the player would reject is refused before it goes: an AC-3 burst in an E-AC-3 stream, a
    // Pd that disagrees with the payload, and a payload with no sync word.
    CHECK(refusal(rig.server.send_burst(rig.now, 1, 8 * 8, eac3_payload(8, 0))) == Refusal::kBadBurst);
    CHECK(refusal(rig.server.send_burst(rig.now, 21, 9, eac3_payload(8, 0))) == Refusal::kBadBurst);
    CHECK(refusal(rig.server.send_burst(rig.now, 21, 8, std::vector<std::uint8_t>(8, 0))) == Refusal::kBadBurst);

    const std::int64_t first = rig.now + 500'000;
    for (int k = 0; k < 8; ++k) {
        rig.send(rig.server.send_burst(first + (k * 32'000), 21, 1792, eac3_payload(1792, static_cast<std::uint8_t>(k))));
    }
    REQUIRE(rig.run_until([&] { return rig.player_events.bursts.size() == 8; }, 100'000));
    for (int k = 0; k < 8; ++k) {
        const PlayerEvents::Burst& burst = rig.player_events.bursts[static_cast<std::size_t>(k)];
        CHECK(burst.pc == 21);
        CHECK(burst.pd == 1792);
        CHECK(burst.payload == eac3_payload(1792, static_cast<std::uint8_t>(k)));
        // The burst's server timestamp, on the player's clock.
        CHECK(std::llabs(burst.local_time - (first + (k * 32'000) + offset)) < 1'000);
    }
    CHECK(rig.player_events.invalid_bursts == 0);

    // The role's output delay plays every burst earlier by as much.
    ac::State delayed = extension_config().ac3forge_state;
    delayed.output_delay_ms = 20;
    rig.from_player(rig.player.set_ac3forge_state(delayed));
    REQUIRE(rig.run_until(
        [&] {
            const m::ClientState& last = rig.server_events.states.back();
            return last.ac3forge && last.ac3forge->output_delay_ms == 20;
        },
        100'000));
    rig.send(rig.server.send_burst(first + (8 * 32'000), 21, 1792, eac3_payload(1792, 8)));
    REQUIRE(rig.run_until([&] { return rig.player_events.bursts.size() == 9; }, 100'000));
    CHECK(std::llabs(rig.player_events.bursts[8].local_time - (first + (8 * 32'000) + offset - 20'000)) < 1'000);

    rig.send(rig.server.clear_burst_stream());
    REQUIRE(rig.run_until([&] { return rig.player_events.burst_clears == 1; }, 100'000));
    CHECK(rig.player_events.clears == 0);

    // An activation without the role ends its stream first, and the state then carries
    // player@v1's object.
    rig.send(rig.server.activate(playback_activation()));
    REQUIRE(rig.run_until([&] { return rig.player_events.burst_ends == 1; }, 100'000));
    CHECK_FALSE(rig.player.burst_streaming());
    CHECK_FALSE(rig.server.burst_streaming());
    CHECK(refusal(rig.server.send_burst(rig.now, 21, 1792, eac3_payload(1792, 0))) == Refusal::kNoStream);
    REQUIRE(rig.run_until([&] { return rig.server_events.states.back().player.has_value(); }, 100'000));
    CHECK_FALSE(rig.server_events.states.back().ac3forge.has_value());
    CHECK(rig.player_events.burst_ends == 1);
}

TEST_CASE("sessions: _ac3forge_player@v1's commands and settings", "[sendspin][sessions][ac3forge]") {
    Rig rig(extension_config(), 0, hs::sentinel_choice(), {});
    REQUIRE(rig.run_until([&] { return !rig.server_events.hellos.empty(); }, 1'000'000));
    ac::CommandMessage volume;
    volume.command = ac::Command::kVolume;
    volume.volume = 30;
    // Nothing before the role is active and its state has arrived.
    CHECK(refusal(rig.server.ac3forge_command(volume)) == Refusal::kNoPlayerState);
    rig.send(rig.server.activate(extension_playback()));
    REQUIRE(rig.run_until([&] { return rig.available(); }, 5'000'000));

    rig.send(rig.server.ac3forge_command(volume));
    ac::CommandMessage identify;
    identify.command = ac::Command::kIdentify;
    identify.identify = ac::Identify{.output = 0, .level_db = -30.0};
    CHECK(refusal(rig.server.ac3forge_command(identify)) == Refusal::kCommandNotListed);

    ac::CommandMessage settings;
    settings.command = ac::Command::kSettings;
    settings.settings.revision = 1;
    settings.settings.trim_db = std::vector<double>{0.0, -3.0};
    settings.settings.decoder.drc_cut = 0.5;
    // Settings the player would refuse do not go: one trim short, a decoder key it did not list,
    // and a value outside the reader's range.
    ac::CommandMessage short_trim = settings;
    short_trim.settings.trim_db = std::vector<double>{0.0};
    CHECK(refusal(rig.server.ac3forge_command(short_trim)) == Refusal::kBadSettings);
    ac::CommandMessage unlisted = settings;
    unlisted.settings.decoder.objects = ac::ObjectsPolicy::kNever;
    CHECK(refusal(rig.server.ac3forge_command(unlisted)) == Refusal::kBadSettings);
    ac::CommandMessage strong = settings;
    strong.settings.decoder.drc_cut = 1.5;
    CHECK(refusal(rig.server.ac3forge_command(strong)) == Refusal::kBadSettings);
    rig.send(rig.server.ac3forge_command(settings));

    REQUIRE(rig.run_until([&] { return rig.player_events.ac3forge_commands.size() == 2; }, 100'000));
    CHECK(rig.player_events.ac3forge_commands[0].volume == 30);
    CHECK(rig.player_events.ac3forge_commands[1].settings.revision == 1);
    CHECK(rig.player_events.ac3forge_commands[1].settings.trim_db == settings.settings.trim_db);
    CHECK(rig.player_events.ac3forge_commands[1].settings.decoder.drc_cut == 0.5);
    CHECK(rig.player_events.commands.empty());
    CHECK(rig.player_events.refused_settings.empty());

    // The player applies them and reports the revision.
    ac::State applied = extension_config().ac3forge_state;
    applied.volume = 30;
    applied.settings_revision = 1;
    rig.from_player(rig.player.set_ac3forge_state(applied));
    REQUIRE(rig.run_until(
        [&] {
            const m::ClientState& last = rig.server_events.states.back();
            return last.ac3forge && last.ac3forge->settings_revision == 1;
        },
        100'000));
    CHECK(rig.server_events.states.back().ac3forge->volume == 30);

    // A command the player's latest state no longer lists is ignored, even from a server that has
    // not heard yet.
    applied.supported_commands = {ac::Command::kVolume};
    rig.from_player(rig.player.set_ac3forge_state(applied));
    settings.settings.revision = 2;
    rig.send(rig.server.ac3forge_command(settings));
    rig.run_for(50'000);
    CHECK(rig.player_events.ac3forge_commands.size() == 2);
    CHECK(refusal(rig.server.ac3forge_command(settings)) == Refusal::kCommandNotListed);
}
