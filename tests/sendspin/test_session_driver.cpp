#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/transport.hpp"
#include "ac3/sendspin/websocket.hpp"

// SessionDriver in real time. A server and a player session, each under its own driver, over
// a WebSocket on loopback: the player listens as a Sendspin client does, the server dials it,
// pairs it with its pairing PSK, activates it once the re-handshake has made it paired, and
// streams PCM chunks the player receives at their times on its own clock. Then the driver's
// own guarantees over the in-memory pair and a connection that stops sending: order, ticks,
// a session's close, and a peer that stops reading.
//
// The WebSocket case dials, so under ThreadSanitizer it needs what test_websocket.cpp says.

namespace {

namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
namespace flow = ac3::sendspin::pairing_flow;
namespace websocket = ac3::sendspin::transport::websocket;
using ac3::sendspin::DrivenSession;
using ac3::sendspin::PlayerConfig;
using ac3::sendspin::PlayerListener;
using ac3::sendspin::PlayerSession;
using ac3::sendspin::ServerConfig;
using ac3::sendspin::ServerListener;
using ac3::sendspin::ServerSession;
using ac3::sendspin::SessionDriver;
using ac3::sendspin::SessionOutput;
using ac3::sendspin::SteadyClock;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::pairing_messages::AbortReason;
using ac3::sendspin::transport::Connection;
using ac3::sendspin::transport::Frame;
using ac3::sendspin::transport::FrameKind;
using namespace std::chrono_literals;

// What the sessions' listeners record, and the test waits on. Listeners run on the drivers'
// threads with a session's lock held, so nothing here calls into a driver.
class Events {
   public:
    template <class Update>
    void update(Update&& update) {
        {
            const std::lock_guard lock(mutex_);
            std::forward<Update>(update)();
        }
        changed_.notify_all();
    }

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::forward<Predicate>(predicate));
    }

    template <class Read>
    auto read(Read&& read) {
        const std::lock_guard lock(mutex_);
        return std::forward<Read>(read)();
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
};

class ClientKeys final : public hs::ClientKeyring {
   public:
    // Read and written only on the player driver's reader thread once it runs.
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

struct PlayerSide final : PlayerListener {
    PlayerSide(Events& e, ClientKeys& k) : events(&e), keys(&k) {}

    Events* events;
    ClientKeys* keys;
    int starts = 0;
    int ends = 0;
    std::vector<std::int64_t> local_times;
    int paired = 0;
    bool ended = false;

    void on_stream_start(const m::PlayerStream& /*stream*/) override {
        events->update([&] { ++starts; });
    }
    void on_stream_clear() override {}
    void on_stream_end() override {
        events->update([&] { ++ends; });
    }
    void on_audio(std::span<const std::uint8_t> /*frame*/, std::int64_t local_time) override {
        events->update([&] { local_times.push_back(local_time); });
    }
    void on_command(const m::PlayerCommandMessage& /*command*/) override {}
    void on_group(const m::GroupUpdate& /*update*/) override {}
    void on_unpaired(const Key32& /*server_key*/) override {}
    void on_pairing_code(const flow::Code& /*code*/) override {}
    void on_pairing_held_back() override {}
    void on_paired(const Key32& server_key, const Key32& long_term_psk) override {
        keys->held.push_back({.psk = long_term_psk, .category = hs::PskCategory::kLongTerm, .server_key = server_key});
        events->update([&] { ++paired; });
    }
    void on_pairing_ended(std::optional<AbortReason> /*reason*/) override {}
};

struct ServerSide final : ServerListener {
    explicit ServerSide(Events& e) : events(&e) {}

    Events* events;
    int hellos = 0;
    std::optional<m::ClientState> state;
    int paired = 0;

    void on_hello(const m::ClientHello& /*hello*/) override {
        events->update([&] { ++hellos; });
    }
    void on_state(const m::ClientState& client_state) override {
        events->update([&] { state = client_state; });
    }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}
    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override {}
    void on_pairing_code_wanted() override {}
    bool on_paired(const Key32& /*client_key*/, const Key32& /*long_term_psk*/) override {
        events->update([&] { ++paired; });
        return true;
    }
    void on_pairing_ended(std::optional<AbortReason> /*reason*/) override {}
};

Key32 random_key() {
    Key32 key{};
    REQUIRE(ac3::sendspin::crypto::random_bytes(key));
    return key;
}

ac3::sendspin::noise::KeyPair generated() {
    std::optional<ac3::sendspin::noise::KeyPair> pair = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

const m::AudioFormat kPcm{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};

DrivenSession driven(PlayerSession& player) {
    return {.receive = [&player](const Frame& frame) { return player.receive(frame); },
            .tick = [&player] { return player.tick(); },
            .next_tick_us = [&player] { return player.next_tick_us(); },
            .ended = {}};
}

DrivenSession driven(ServerSession& server) {
    return {.receive = [&server](const Frame& frame) { return server.receive(frame); },
            .tick = [&server] { return server.tick(); },
            .next_tick_us = [&server] { return server.next_tick_us(); },
            .ended = {}};
}

}  // namespace

TEST_CASE("session driver: a player paired, activated and streamed to over a loopback WebSocket",
          "[sendspin][driver][websocket]") {
    const Key32 pairing_psk = random_key();
    Events events;
    SteadyClock clock;
    ClientKeys client_keys;
    client_keys.held.push_back({.psk = pairing_psk, .category = hs::PskCategory::kPairing, .server_key = {}});
    ServerKeys server_keys;
    server_keys.choice = {.psk = pairing_psk, .category = hs::PskCategory::kPairing};
    flow::ClientPairingState pairing_state;
    PlayerSide player_side(events, client_keys);
    ServerSide server_side(events);

    PlayerConfig config;
    config.identity = generated();
    config.name = "Test sink";
    config.player_support = {.supported_formats = {kPcm}, .buffer_capacity = 1 << 20, .commands = {}};
    config.pair_methods = {{.method = m::PairMethod::kPairingPsk, .locations = {}, .out_channels = {}, .formats = {},
                            .min_pin_length = 0}};
    config.player_state = {.volume = 50,
                           .muted = false,
                           .output_delay_ms = 0,
                           .required_lead_time_ms = 200,
                           .min_buffer_ms = 100,
                           .supported_commands = std::vector<m::PlayerCommand>{},
                           .format = std::nullopt};
    PlayerSession player(std::move(config), client_keys, pairing_state, player_side, clock);
    std::unique_ptr<SessionDriver> player_driver;

    websocket::ListenerOptions options;
    options.address = "127.0.0.1";
    const std::unique_ptr<websocket::Listener> listener =
        websocket::Listener::start(options, [&](std::unique_ptr<Connection> connection) {
            DrivenSession session = driven(player);
            session.ended = [&events, &player_side] { events.update([&] { player_side.ended = true; }); };
            auto driver = std::make_unique<SessionDriver>(std::move(connection), std::move(session));
            driver->start(player.open());
            events.update([&] { player_driver = std::move(driver); });
        });
    REQUIRE(listener != nullptr);

    ServerSession server(ServerConfig{.identity = generated(), .name = "Hearth", .languages = {}, .max_message_bytes = 1 << 22},
                         server_keys, server_side, clock);
    auto dialled = websocket::connect("ws://127.0.0.1:" + std::to_string(listener->port()) + std::string(websocket::kPath));
    REQUIRE(dialled.has_value());
    SessionDriver server_driver(std::move(*dialled), driven(server));
    server_driver.start();

    REQUIRE(events.wait([&] { return server_side.hellos == 1; }, 10s));
    CHECK(server_driver.inspect([&] { return server.psk_category(); }) == hs::PskCategory::kPairing);
    const m::Activate pairing{.activities = {m::Activity::kPairing},
                              .active_roles = std::vector<std::string>{},
                              .pairing = m::PairingActivation{
                                  .method = m::PairMethod::kPairingPsk, .format = std::nullopt, .pin_length = 0, .languages = {}}};
    REQUIRE(server_driver.call([&] { return server.activate(pairing); }).has_value());

    // Paired: the re-handshake brings a second hello, under the long-term PSK.
    REQUIRE(events.wait([&] { return server_side.hellos == 2; }, 10s));
    CHECK(events.read([&] { return server_side.paired == 1 && player_side.paired == 1; }));
    CHECK(server_driver.inspect([&] { return server.psk_category(); }) == hs::PskCategory::kLongTerm);

    const m::Activate playback{.activities = {m::Activity::kPlayback},
                               .active_roles = std::vector<std::string>{"player@v1"},
                               .pairing = std::nullopt};
    REQUIRE(server_driver.call([&] { return server.activate(playback); }).has_value());
    REQUIRE(events.wait([&] { return server_side.state && server_side.state->available; }, 15s));

    REQUIRE(server_driver.call([&] { return server.start_stream({.format = kPcm, .codec_header = {}}); }).has_value());
    REQUIRE(events.wait([&] { return player_side.starts == 1; }, 5s));
    const std::int64_t first = clock.now_us() + 500'000;
    const std::vector<std::uint8_t> chunk(960 * 4, 0);
    for (int k = 0; k < 20; ++k) {
        REQUIRE(server_driver.call([&] { return server.send_audio(first + (k * 20'000), chunk); }).has_value());
    }
    REQUIRE(events.wait([&] { return player_side.local_times.size() == 20; }, 5s));
    // Both clocks are this process's, so each chunk's local time is its timestamp, give or take
    // the filter's error.
    const std::vector<std::int64_t> times = events.read([&] { return player_side.local_times; });
    for (std::size_t k = 0; k < times.size(); ++k) {
        CHECK(std::llabs(times[k] - (first + (static_cast<std::int64_t>(k) * 20'000))) < 2'000);
    }

    REQUIRE(server_driver.call([&] { return server.end_stream(); }).has_value());
    REQUIRE(events.wait([&] { return player_side.ends == 1; }, 5s));
    server_driver.close();
    server_driver.join();
    CHECK(events.wait([&] { return player_side.ended; }, 5s));
}

namespace {

// A connection whose sends wait until the test releases them, as a peer that has stopped
// reading makes them wait, and whose receive() waits until it is closed.
class StalledConnection final : public Connection {
   public:
    std::optional<Frame> receive() override {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return closed_; });
        return std::nullopt;
    }
    bool send_text(std::string_view /*text*/) override { return wait_to_send(); }
    bool send_binary(std::span<const std::uint8_t> /*bytes*/) override { return wait_to_send(); }
    void close() override {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
    }
    [[nodiscard]] std::string peer() const override { return "stalled"; }

   private:
    bool wait_to_send() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return closed_; });
        return false;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_ = false;
};

SessionOutput binary_frames(std::size_t count, std::size_t bytes) {
    SessionOutput out;
    for (std::size_t i = 0; i < count; ++i) {
        out.binary(std::vector<std::uint8_t>(bytes, static_cast<std::uint8_t>(i)));
    }
    return out;
}

}  // namespace

TEST_CASE("session driver: frames in order, ticks when due, and a session's close", "[sendspin][driver]") {
    auto [near, far] = ac3::sendspin::transport::memory_pair();
    Events events;
    int ticks = 0;
    // An echo: a one-byte frame back for each one-byte frame, holding the byte plus one, then a
    // close after a 9.
    DrivenSession session{
        .receive =
            [](const Frame& frame) {
                SessionOutput out;
                const std::uint8_t value = frame.bytes.size() == 1 ? frame.bytes.front() : std::uint8_t{0};
                out.close = value == 9;
                out.binary(std::vector<std::uint8_t>(1, static_cast<std::uint8_t>(value + 1)));
                return out;
            },
        .tick =
            [&] {
                events.update([&] { ++ticks; });
                return SessionOutput{};
            },
        .next_tick_us = [] { return std::int64_t{20'000}; },
        .ended = {}};
    SessionDriver driver(std::move(near), std::move(session));
    SessionOutput first;
    first.text("hello");
    driver.start(std::move(first));

    const std::optional<Frame> greeting = far->receive();
    REQUIRE(greeting.has_value());
    CHECK(greeting->kind == FrameKind::kText);
    CHECK(greeting->text() == "hello");

    // Frames the owner queues through call() go out after the ones before them.
    driver.call([] { return binary_frames(3, 4); });
    for (std::uint8_t i = 0; i < 3; ++i) {
        const std::optional<Frame> frame = far->receive();
        REQUIRE(frame.has_value());
        CHECK(frame->bytes == std::vector<std::uint8_t>(4, i));
    }
    for (std::uint8_t value = 0; value < 5; ++value) {
        REQUIRE(far->send_binary(std::vector<std::uint8_t>{value}));
        const std::optional<Frame> echo = far->receive();
        REQUIRE(echo.has_value());
        CHECK(echo->bytes == std::vector<std::uint8_t>{static_cast<std::uint8_t>(value + 1)});
    }

    CHECK(events.wait([&] { return ticks >= 5; }, 5s));

    // The session asks to close: its last frame still arrives, then the connection ends.
    REQUIRE(far->send_binary(std::vector<std::uint8_t>{9}));
    const std::optional<Frame> last = far->receive();
    REQUIRE(last.has_value());
    CHECK(last->bytes == std::vector<std::uint8_t>{10});
    CHECK_FALSE(far->receive().has_value());
    driver.join();
    CHECK(driver.ended());
    // Later calls queue nothing.
    driver.call([] { return binary_frames(1, 4); });
    CHECK(driver.queued_bytes() == 0);
}

TEST_CASE("session driver: a peer that stops reading is disconnected past the queue bound", "[sendspin][driver]") {
    auto stalled = std::make_unique<StalledConnection>();
    DrivenSession session{.receive = [](const Frame& /*frame*/) { return SessionOutput{}; },
                          .tick = [] { return SessionOutput{}; },
                          .next_tick_us = [] { return std::int64_t{1'000'000}; },
                          .ended = {}};
    SessionDriver driver(std::move(stalled), std::move(session), 3000);
    driver.start();

    // The writer takes the first frame and waits in its send; two more fit under the bound.
    driver.call([] { return binary_frames(3, 1000); });
    CHECK_FALSE(driver.ended());
    // One more does not.
    driver.call([] { return binary_frames(2, 1000); });
    CHECK(driver.ended());
    CHECK(driver.queued_bytes() == 0);
    driver.join();
}
