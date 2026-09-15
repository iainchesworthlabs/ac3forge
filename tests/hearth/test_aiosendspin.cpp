#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"

// A4's exit with aiosendspin 9.1.1 (planning/hearth-reference-player.md, A4's exit;
// planning/hearth-sendspin-extension.md, Decisions): the server's half, which
// tools/sendspin/aiosendspin_exit.py runs once for each codec against the scripted player in
// tools/sendspin/aiosendspin_player.py, passing the player's URL, its SP:0 token and a directory in
// the environment. The host pairs with the player by the token, which makes the pairing PSK flow
// and the re-handshake run in aiosendspin 9.1.1's dialect, and plays it three seconds of two tones
// over player@v1 in the codec it offers. The programme and the time its first frame plays go to the
// directory, and the script checks what the player decoded against them. Hidden, and skipped
// without the environment.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace ss = ac3::sendspin;
using namespace std::chrono_literals;

constexpr std::int32_t kSampleRate = 48000;
constexpr std::size_t kFrames = 3 * 48000;

[[nodiscard]] std::optional<std::string> environment(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

class Events final : public ss::ServerHostEvents {
   public:
    void on_client(const ss::ClientView& client) override {
        {
            const std::lock_guard lock(mutex_);
            clients_[client.client_id] = client;
        }
        changed_.notify_all();
    }
    void on_client_gone(const std::string& /*client_id*/) override {}
    void on_pairing_code_wanted(const std::string& /*client_id*/) override {}
    void on_paired(const std::string& /*client_id*/) override {}
    void on_pairing_ended(const std::string& /*client_id*/,
                          std::optional<ss::pairing_messages::AbortReason> /*reason*/) override {}
    void on_log(std::string_view line) override {
        const std::lock_guard lock(mutex_);
        log_.emplace_back(line);
    }

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(clients_); });
    }

    std::vector<std::string> log() {
        const std::lock_guard lock(mutex_);
        return log_;
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, ss::ClientView> clients_;
    std::vector<std::string> log_;
};

}  // namespace

TEST_CASE("aiosendspin: a host pairs with the scripted aiosendspin 9.1.1 player and plays it a programme",
          "[.][aiosendspin]") {
    const std::optional<std::string> url = environment("AC3FORGE_AIOSENDSPIN_URL");
    const std::optional<std::string> token = environment("AC3FORGE_AIOSENDSPIN_TOKEN");
    const std::optional<std::string> out = environment("AC3FORGE_AIOSENDSPIN_OUT");
    if (!url || !token || !out) {
        SKIP("run by tools/sendspin/aiosendspin_exit.py");
    }
    const fs::path directory{*out};
    fs::create_directories(directory);

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    Events events;
    auto host = ss::ServerHost::start(
        {.identity = *identity, .name = "Hearth exit host", .languages = {"en"}, .address = "127.0.0.1",
         .port = std::nullopt, .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(*token));
    (*host)->dial(*url);

    // Paired and on its long-term PSK, the player plays once its clock has settled.
    const bool playing = events.wait(
        [](const auto& clients) {
            return clients.size() == 1 && clients.begin()->second.playing && clients.begin()->second.available &&
                   clients.begin()->second.psk == ss::handshake::PskCategory::kLongTerm;
        },
        45s);
    if (!playing) {
        for (const std::string& line : events.log()) {
            UNSCOPED_INFO(line);
        }
    }
    REQUIRE(playing);
    const std::vector<ss::ClientView> clients = (*host)->clients();
    REQUIRE(clients.size() == 1);
    const ss::ClientView& player = clients.front();
    CHECK(player.dialect == ss::Dialect::kAiosendspin911);
    CHECK_FALSE(player.bursts);
    REQUIRE(player.player_support.has_value());
    REQUIRE(player.player_support->supported_formats.size() == 1);

    std::shared_ptr<ss::Group> group = (*host)->make_group("Exit");
    group->add(player.client_id);
    const m::AudioFormat source{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = kSampleRate, .bit_depth = 16};
    REQUIRE(group->start({.pcm = source, .bursts = std::nullopt, .buffered = true}));

    // 440 Hz on the left and 1 kHz on the right, near -11 dBFS.
    std::vector<std::int32_t> programme;
    programme.reserve(kFrames * 2);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double t = static_cast<double>(frame) / kSampleRate;
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * t))));
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * t))));
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (offset < programme.size() && std::chrono::steady_clock::now() < deadline) {
        const std::size_t block = std::min<std::size_t>(4800 * 2, programme.size() - offset);
        const std::size_t taken = group->push(std::span<const std::int32_t>(programme).subspan(offset, block));
        if (taken == 0) {
            std::this_thread::sleep_for(10ms);
        }
        offset += taken * 2;
    }
    REQUIRE(offset == programme.size());
    CHECK(group->members_playing() == 1);
    const std::optional<std::int64_t> start_time = group->start_time();
    REQUIRE(start_time.has_value());
    group->stop();

    // What the script checks the player against.
    std::vector<std::byte> bytes;
    bytes.reserve(programme.size() * 2);
    for (const std::int32_t sample : programme) {
        const auto word = static_cast<std::uint16_t>(static_cast<std::int16_t>(sample));
        bytes.push_back(static_cast<std::byte>(word & 0xFFU));
        bytes.push_back(static_cast<std::byte>(word >> 8U));
    }
    REQUIRE(ac3::io::write_wav_pcm16_raw((directory / "programme.wav").string(), bytes, kSampleRate, 2).has_value());
    {
        std::ofstream start(directory / "start_time_us.txt");
        start << *start_time << '\n';
        REQUIRE(start.good());
    }

    // The last units and stream/end are on their way; the connection closes with the host.
    std::this_thread::sleep_for(1s);
    group.reset();
    host->reset();
}
