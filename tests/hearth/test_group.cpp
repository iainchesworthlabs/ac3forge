#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "sink.hpp"

// A ServerHost and two test sinks in process over loopback WebSockets, with mDNS off: the host
// dials both, approves them for unpaired access, and plays one programme to them as a group, PCM
// to one and FLAC to the other. Each sink's WAV holds exactly the programme, and every chunk's
// logged play time puts the programme's first frame at the same local time on both sinks, within
// 1 ms: the group plays in step (planning/hearth-reference-player.md, A4's exit, here with PCM
// before the extension role carries E-AC-3).
//
// It dials, so under ThreadSanitizer it needs what tests/sendspin/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;

class QuietLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        const std::size_t at = text.find("PAIRING CODE ");
        if (at != std::string_view::npos) {
            std::string digits;
            for (const char c : text.substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            code_ = digits;
        }
    }

    // The last dynamic pairing code a sink showed, as digits.
    std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        return code_;
    }

   private:
    std::mutex mutex_;
    std::optional<std::string> code_;
};

class HostEvents final : public ac3::sendspin::ServerHostEvents {
   public:
    void on_client(const ac3::sendspin::ClientView& client) override {
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
                          std::optional<ac3::sendspin::pairing_messages::AbortReason> /*reason*/) override {}
    void on_log(std::string_view /*line*/) override {}

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(clients_); });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, ac3::sendspin::ClientView> clients_;
};

std::unique_ptr<testsink::Sink> start_sink(const fs::path& directory, std::string name, m::Codec codec, QuietLog& log,
                                          bool unpaired_access = true) {
    testsink::SinkOptions options;
    options.name = std::move(name);
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = directory / "state";
    options.output_directory = directory / "out";
    options.advertise = false;
    options.unpaired_access = unpaired_access;
    options.codecs = {codec};
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    return std::move(*sink);
}

// The local time each logged chunk puts the stream's first frame at.
std::vector<double> first_frame_times(const fs::path& log) {
    std::vector<double> times;
    std::ifstream in(log);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string local;
        std::string first;
        if (!std::getline(fields, local, ',') || !std::getline(fields, first, ',') || local == "clear") {
            continue;
        }
        times.push_back(std::stod(local) - (std::stod(first) * 1'000'000.0 / 48000.0));
    }
    return times;
}

}  // namespace

TEST_CASE("group: two test sinks play one programme in step, in PCM and FLAC", "[hearth][group][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "hearth_group";
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> kitchen = start_sink(scratch / "kitchen", "Kitchen", m::Codec::kPcm, log);
    const std::unique_ptr<testsink::Sink> lounge = start_sink(scratch / "lounge", "Lounge", m::Codec::kFlac, log);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");

    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        CHECK_FALSE(client.playing);
        REQUIRE((*host)->approve(client.client_id, true));
    }
    // Approved, both play once their clocks converge.
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Downstairs");
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }
    const m::AudioFormat source{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    REQUIRE(group->start(source, true));

    // Two seconds of a tone, pushed in blocks as fast as the group takes them.
    std::vector<std::int32_t> programme;
    for (int frame = 0; frame < 96000; ++frame) {
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.0575))));
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.131))));
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (offset < programme.size() && std::chrono::steady_clock::now() < deadline) {
        const std::size_t block = std::min<std::size_t>(4800 * 2, programme.size() - offset);
        const std::size_t taken = group->push(std::span<const std::int32_t>(programme).subspan(offset, block));
        if (taken == 0) {
            std::this_thread::sleep_for(10ms);
        }
        offset += taken * 2;
    }
    REQUIRE(offset == programme.size());
    CHECK(group->members_playing() == 2);
    group->stop();

    // Both sinks have played it all.
    const auto played = [](const testsink::Sink& sink) { return sink.totals().frames; };
    const auto until = std::chrono::steady_clock::now() + 10s;
    while ((played(*kitchen) < 96000 || played(*lounge) < 96000) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(played(*kitchen) == 96000);
    REQUIRE(played(*lounge) == 96000);
    // A group must not outlive its host.
    group.reset();
    host->reset();

    // Each WAV is the programme, sample for sample.
    for (const fs::path& directory : {scratch / "kitchen", scratch / "lounge"}) {
        const auto wav = ac3::io::read_wav((directory / "out" / "stream-1-1.wav").string());
        REQUIRE(wav.has_value());
        REQUIRE(wav->frame_count() == 96000);
        std::size_t different = 0;
        for (std::size_t frame = 0; frame < 96000; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float wanted = static_cast<float>(programme[(frame * 2) + channel]) / 32768.0F;
                different += wav->channels[channel][frame] == wanted ? 0U : 1U;
            }
        }
        CHECK(different == 0);
    }

    // Every chunk on both sinks puts the first frame at the same local time, within 1 ms.
    std::vector<double> times = first_frame_times(scratch / "kitchen" / "out" / "stream-1-1.times.csv");
    const std::vector<double> lounge_times = first_frame_times(scratch / "lounge" / "out" / "stream-1-1.times.csv");
    REQUIRE_FALSE(times.empty());
    REQUIRE_FALSE(lounge_times.empty());
    times.insert(times.end(), lounge_times.begin(), lounge_times.end());
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}

TEST_CASE("group: a host pairs one test sink by its token and another by a dynamic code", "[hearth][group][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "hearth_pairing";
    fs::remove_all(scratch);
    QuietLog token_log;
    QuietLog code_log;
    const std::unique_ptr<testsink::Sink> by_token = start_sink(scratch / "token", "By token", m::Codec::kPcm, token_log, false);
    const std::unique_ptr<testsink::Sink> by_code = start_sink(scratch / "code", "By code", m::Codec::kPcm, code_log, false);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());

    // The operator enters the first sink's token before the host has even met it.
    REQUIRE((*host)->enter_pairing_token(by_token->pairing_token()));
    CHECK_FALSE((*host)->enter_pairing_token("SP:0NOTATOKEN"));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(by_token->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(by_code->port()) + "/sendspin");

    const auto playing = [](const std::string& id) {
        return [id](const auto& clients) {
            const auto found = clients.find(id);
            return found != clients.end() && found->second.playing &&
                   found->second.psk == ac3::sendspin::handshake::PskCategory::kLongTerm;
        };
    };
    REQUIRE(events.wait(playing(by_token->client_id()), 20s));

    // The second waits unpaired until the operator pairs it by the code it shows.
    REQUIRE(events.wait([&](const auto& clients) { return clients.contains(by_code->client_id()); }, 15s));
    const std::optional<ac3::sendspin::ClientView> waiting = (*host)->client(by_code->client_id());
    REQUIRE(waiting.has_value());
    CHECK_FALSE(waiting->playing);
    REQUIRE((*host)->pair(by_code->client_id(), m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    const auto shown = std::chrono::steady_clock::now() + 15s;
    while (!code_log.code() && std::chrono::steady_clock::now() < shown) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(code_log.code().has_value());
    const auto wanted = std::chrono::steady_clock::now() + 10s;
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < wanted) {
        entered = (*host)->enter_code(by_code->client_id(), *code_log.code());
        if (!entered) {
            std::this_thread::sleep_for(20ms);
        }
    }
    REQUIRE(entered);
    REQUIRE(events.wait(playing(by_code->client_id()), 20s));

    host->reset();
}