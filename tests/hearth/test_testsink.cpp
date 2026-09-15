#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "sink.hpp"

// ac3hearth-testsink in process, over a loopback WebSocket with mDNS off: a server session pairs
// with it by the pairing token the sink prints, plays a stream in PCM, FLAC or Opus, and finds in
// the sink's WAV file exactly what a local decode of the same units gives, with a play time
// logged for every unit; then a sink restarted on the same state directory is reached under the
// long-term PSK without pairing again.
//
// It dials, so under ThreadSanitizer it needs what tests/sendspin/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace codec = ac3::sendspin::codec;
namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
namespace testsink = ac3::hearth::testsink;
namespace websocket = ac3::sendspin::transport::websocket;
using ac3::sendspin::crypto::Key32;
using namespace std::chrono_literals;

class QuietLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(text);
    }

   private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

class Keys final : public hs::ServerKeyring {
   public:
    hs::PskChoice choice = hs::sentinel_choice();
    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return choice; }
};

// The server's events, which the test waits on.
class Server final : public ac3::sendspin::ServerListener {
   public:
    void on_hello(const m::ClientHello& /*hello*/) override { update([&] { ++hellos; }); }
    void on_state(const m::ClientState& client_state) override { update([&] { available = client_state.available; }); }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}
    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override {}
    void on_pairing_code_wanted() override {}
    bool on_paired(const Key32& /*client_key*/, const Key32& psk) override {
        update([&] { long_term_psk = psk; });
        return true;
    }
    void on_pairing_ended(std::optional<ac3::sendspin::pairing_messages::AbortReason> /*reason*/) override {}

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::forward<Predicate>(predicate));
    }

    int hellos = 0;
    bool available = false;
    std::optional<Key32> long_term_psk;

   private:
    template <class Update>
    void update(Update&& change) {
        {
            const std::lock_guard lock(mutex_);
            change();
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
};

ac3::sendspin::noise::KeyPair generated() {
    std::optional<ac3::sendspin::noise::KeyPair> pair = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

// A second of a stereo tone at 16 bits, different in each channel, interleaved.
std::vector<std::int32_t> tone() {
    std::vector<std::int32_t> samples;
    for (int frame = 0; frame < 48000; ++frame) {
        for (int channel = 0; channel < 2; ++channel) {
            const double phase = static_cast<double>(frame) * (channel == 0 ? 0.0575 : 0.131);
            samples.push_back(static_cast<std::int32_t>(std::lround(12000.0 * std::sin(phase))));
        }
    }
    return samples;
}

}  // namespace

TEST_CASE("test sink: paired by its token over loopback, it writes what it plays", "[hearth][testsink][websocket]") {
    const m::Codec kind = GENERATE(m::Codec::kPcm, m::Codec::kFlac, m::Codec::kOpus);
    const m::AudioFormat format{.codec = kind, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "hearth_testsink";
    fs::remove_all(scratch);
    testsink::SinkOptions options;
    options.name = "Loopback sink";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = scratch / "state";
    options.output_directory = scratch / "out";
    options.advertise = false;
    options.codecs = {kind};

    // What the server sends: the tone encoded as one stream.
    const std::unique_ptr<codec::Encoder> encoder = codec::make_encoder(format);
    REQUIRE(encoder != nullptr);
    std::optional<std::vector<codec::Unit>> units = encoder->encode(tone());
    REQUIRE(units.has_value());
    const std::optional<std::vector<codec::Unit>> rest = encoder->finish();
    REQUIRE(rest.has_value());
    units->insert(units->end(), rest->begin(), rest->end());

    QuietLog log;
    const ac3::sendspin::SteadyClock clock;
    const ac3::sendspin::noise::KeyPair server_identity = generated();
    std::optional<Key32> long_term_psk;
    std::string client_id;

    {
        auto sink = testsink::Sink::start(options, log);
        REQUIRE(sink.has_value());
        client_id = (*sink)->client_id();
        const std::optional<ac3::sendspin::pairing::PairingPskToken> token =
            ac3::sendspin::pairing::decode_pairing_psk_token((*sink)->pairing_token());
        REQUIRE(token.has_value());

        Keys keys;
        keys.choice = {.psk = token->pairing_psk, .category = hs::PskCategory::kPairing};
        Server events;
        ac3::sendspin::ServerSession server(
            {.identity = server_identity, .name = "Test server", .languages = {}, .max_message_bytes = 1 << 22}, keys, events,
            clock);
        auto dialled = websocket::connect("ws://127.0.0.1:" + std::to_string((*sink)->port()) + "/sendspin");
        REQUIRE(dialled.has_value());
        ac3::sendspin::SessionDriver driver(std::move(*dialled),
                                            {.receive = [&](const auto& frame) { return server.receive(frame); },
                                             .tick = [&] { return server.tick(); },
                                             .next_tick_us = [&] { return server.next_tick_us(); },
                                             .ended = {}});
        driver.start();

        REQUIRE(events.wait([&] { return events.hellos == 1; }, 10s));
        // The server has the client's key from the token, and must check it against the connection.
        CHECK(driver.inspect([&] { return ac3::sendspin::base64url::encode(server.client_key()); }) == client_id);
        REQUIRE(driver.call([&] {
                          return server.activate({.activities = {m::Activity::kPairing},
                                                  .active_roles = std::vector<std::string>{},
                                                  .pairing = m::PairingActivation{.method = m::PairMethod::kPairingPsk,
                                                                                  .format = std::nullopt,
                                                                                  .pin_length = 0,
                                                                                  .languages = {}}});
                      }).has_value());
        REQUIRE(events.wait([&] { return events.hellos == 2; }, 10s));
        long_term_psk = events.long_term_psk;
        REQUIRE(long_term_psk.has_value());

        REQUIRE(driver.call([&] {
                          return server.activate({.activities = {m::Activity::kPlayback},
                                                  .active_roles = std::vector<std::string>{"player@v1"},
                                                  .pairing = std::nullopt});
                      }).has_value());
        REQUIRE(events.wait([&] { return events.available; }, 15s));
        REQUIRE(driver.call([&] {
                          return server.start_stream({.format = format, .codec_header = encoder->codec_header()});
                      }).has_value());
        // Each unit at its first frame's time, earlier by the codec's look-ahead.
        const std::int64_t first = clock.now_us() + 400'000;
        for (const codec::Unit& unit : *units) {
            const std::int64_t at = first + ((unit.first_frame - encoder->delay_frames()) * 1'000'000 / 48000);
            REQUIRE(driver.call([&] { return server.send_audio(at, unit.bytes); }).has_value());
        }
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((*sink)->totals().chunks < units->size() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        REQUIRE((*sink)->totals().chunks == units->size());
        REQUIRE(driver.call([&] { return server.end_stream(); }).has_value());
        std::this_thread::sleep_for(200ms);
        driver.close();
        driver.join();
    }

    // The WAV equals a local decode of the same units, sample for sample, and the log has a play
    // time for every unit.
    const std::unique_ptr<codec::Decoder> local = codec::make_decoder({.format = format, .codec_header = encoder->codec_header()});
    REQUIRE(local != nullptr);
    std::vector<std::int32_t> expected;
    for (const codec::Unit& unit : *units) {
        const std::optional<std::vector<std::int32_t>> decoded = local->decode(unit.bytes);
        REQUIRE(decoded.has_value());
        expected.insert(expected.end(), decoded->begin(), decoded->end());
    }
    const auto wav = ac3::io::read_wav((options.output_directory / "stream-1-1.wav").string());
    REQUIRE(wav.has_value());
    CHECK(wav->sample_rate == 48000);
    REQUIRE(wav->channels.size() == 2);
    REQUIRE(wav->frame_count() == expected.size() / 2);
    std::size_t different = 0;
    for (std::size_t frame = 0; frame < wav->frame_count(); ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float wanted = static_cast<float>(static_cast<double>(expected[(frame * 2) + channel]) / 32768.0);
            different += wav->channels[channel][frame] == wanted ? 0U : 1U;
        }
    }
    CHECK(different == 0);
    std::ifstream times(options.output_directory / "stream-1-1.times.csv");
    std::string line;
    std::size_t lines = 0;
    while (std::getline(times, line)) {
        ++lines;
    }
    CHECK(lines == units->size() + 1);

    // Restarted on the same state, the sink is reached under its long-term PSK, without pairing.
    auto again = testsink::Sink::start(options, log);
    REQUIRE(again.has_value());
    CHECK((*again)->client_id() == client_id);
    Keys keys;
    keys.choice = {.psk = *long_term_psk, .category = hs::PskCategory::kLongTerm};
    Server events;
    ac3::sendspin::ServerSession server({.identity = server_identity, .name = "Test server", .languages = {}, .max_message_bytes = 1 << 22},
                                        keys, events, clock);
    auto dialled = websocket::connect("ws://127.0.0.1:" + std::to_string((*again)->port()) + "/sendspin");
    REQUIRE(dialled.has_value());
    ac3::sendspin::SessionDriver driver(std::move(*dialled),
                                        {.receive = [&](const auto& frame) { return server.receive(frame); },
                                         .tick = [&] { return server.tick(); },
                                         .next_tick_us = [&] { return server.next_tick_us(); },
                                         .ended = {}});
    driver.start();
    REQUIRE(events.wait([&] { return events.hellos == 1; }, 10s));
    CHECK(driver.inspect([&] { return server.psk_category(); }) == hs::PskCategory::kLongTerm);
    CHECK_FALSE(driver.inspect([&] { return server.credential_mismatch(); }));
    driver.close();
    driver.join();
}
