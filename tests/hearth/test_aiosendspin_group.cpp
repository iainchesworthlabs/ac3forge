#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
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

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "engine_thread.hpp"
#include "network_group_sink.hpp"
#include "sink.hpp"

// Issue #874's own exit (planning/hearth-reference-player.md, A6), read the way A4's own exit and
// Verified-by (the same doc) set the precedent for what "the reference Python player" means: a
// real aiosendspin 9.1.1 process, driven by tools/sendspin/aiosendspin_group_exit.py the way
// aiosendspin_exit.py drives test_aiosendspin.cpp for A4. That test proves single-client interop
// at the library level (Group::push() called directly); test_engine_network_group.cpp proves two
// test sinks played from the app's own Engine. This is the union of both: the scripted player and
// two test sinks in ONE group, played from Engine - "a group of two test sinks and the reference
// Python player plays one programme," literally.
//
// What is NOT checked here, and why: byte-exact PCM against the scripted player. The programme is
// E-AC-3 (needed for the burst-taking test sink to have anything to take), and AC-3/E-AC-3 is a
// lossy codec - there is no reference PCM this could byte-match the way test_aiosendspin.cpp's own
// hand-pushed, uncompressed samples do. What real third-party interop actually needs proving -
// pairing, handshake, negotiation, a complete stream the player's own decoder accepts without
// error, in step with what the test sinks heard - is what this checks instead: frame counts and a
// clean stream end, on both sides, the same rigour test_engine_network_group.cpp already applies
// to the test sinks. Codec correctness itself is exhaustively covered elsewhere in this suite.
//
// Hidden, and skipped without the environment - aiosendspin_group_exit.py's own contract, mirroring
// test_aiosendspin.cpp's.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace ss = ac3::sendspin;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;
using ac3::hearth::Engine;
using ac3::hearth::EngineOutputs;
using ac3::hearth::EngineStatus;
using ac3::hearth::EngineTiming;
using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
using ac3::hearth::OutputMode;
using ac3::hearth::OutputPreferences;
using ac3::hearth::QueueItem;
using ac3::hearth::TransportState;

[[nodiscard]] std::optional<std::string> environment(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string scratch_pid_suffix() {
#ifdef _WIN32
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

class QuietLog final : public testsink::SinkLog {
   public:
    void line(std::string_view /*text*/) override {}
};

class HostEvents final : public ss::ServerHostEvents {
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

std::unique_ptr<testsink::Sink> start_sink(const fs::path& directory, std::string name, bool extension_role,
                                           bool unpaired_access, QuietLog& log) {
    testsink::SinkOptions options;
    options.name = std::move(name);
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = directory / "state";
    options.output_directory = directory / "out";
    options.advertise = false;
    options.unpaired_access = unpaired_access;
    options.codecs = {m::Codec::kPcm};
    options.extension_role = extension_role;
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    return std::move(*sink);
}

// `frames` E-AC-3 access units of a 440 Hz tone - test_engine.cpp's own eac3_stream(): each unit
// is a full six-block (1,536-sample, 32 ms) burst on its own.
std::vector<std::byte> eac3_stream(int frames) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0));
        }
        const std::vector<std::span<const float>> views(2, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

}  // namespace

// [aiosendspin-group], not [aiosendspin]: aiosendspin_exit.py's own "ac3tests [aiosendspin]"
// (once per codec) must not also pick this up - it shares AC3FORGE_AIOSENDSPIN_URL/_TOKEN/_OUT's
// names with test_aiosendspin.cpp, but expects a different scripted player and a different group
// shape (two members here, one there); the two must never run against the same process.
TEST_CASE("aiosendspin: a group of two test sinks and the scripted aiosendspin player plays one programme",
         "[.][aiosendspin-group]") {
    const std::optional<std::string> url = environment("AC3FORGE_AIOSENDSPIN_URL");
    const std::optional<std::string> token = environment("AC3FORGE_AIOSENDSPIN_TOKEN");
    const std::optional<std::string> out = environment("AC3FORGE_AIOSENDSPIN_OUT");
    if (!url || !token || !out) {
        SKIP("run by tools/sendspin/aiosendspin_group_exit.py");
    }
    const fs::path directory{*out};
    fs::create_directories(directory);
    constexpr int kFrameCount = 20;  // 640 ms
    const fs::path scratch =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_aiosendspin_group_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> burst_sink =
        start_sink(scratch / "burst", "Burst sink", /*extension_role=*/true, /*unpaired_access=*/false, log);

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    HostEvents events;
    auto host = ss::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1",
         .port = std::nullopt, .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(burst_sink->pairing_token()));
    REQUIRE((*host)->enter_pairing_token(*token));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
    (*host)->dial(*url);

    const bool ready = events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        45s);
    if (!ready) {
        for (const std::string& line : events.log()) {
            UNSCOPED_INFO(line);
        }
    }
    REQUIRE(ready);
    // One client is the scripted player (player@v1 over aiosendspin's own dialect), the other the
    // burst-taking test sink (_ac3forge_player@v1) - not identified by arrival order, which is not
    // guaranteed.
    std::size_t player_count = 0;
    for (const ss::ClientView& client : (*host)->clients()) {
        if (client.dialect == ss::Dialect::kAiosendspin911) {
            CHECK_FALSE(client.bursts);
            ++player_count;
        } else {
            CHECK(client.bursts);
        }
    }
    CHECK(player_count == 1);

    std::shared_ptr<ss::Group> group = (*host)->make_group("Living room");
    for (const ss::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }

    const std::vector<std::byte> programme = eac3_stream(kFrameCount);
    const ItemLoader loader = [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    EngineOutputs outputs{
        .group = ac3::hearth::make_group_sink([&group](const std::string&) { return group; })};
    Engine engine(std::move(outputs), loader, *layout, ac3::hearth::DecoderSettings{},
                 EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                     .follow_sink = true,
                                                     .group_name = group->id(),
                                                     .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();

    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] { return burst_sink->totals().bursts >= 1; }));
    CHECK(burst_sink->totals().bursts == static_cast<std::uint64_t>(kFrameCount));
    CHECK(burst_sink->totals().burst_frames == expected_frames);
    CHECK(burst_sink->totals().connections == 1);
    // Not group->members_playing() here: by now Player's own close_output()
    // has already called Group::stop() (the item finished, the same reason
    // finished.history is already complete above), so it reads 0 correctly
    // - it is not a "still playing" check, it would be checking the group
    // after its own programme already ended.

    // What aiosendspin_group_exit.py checks the scripted player against: it
    // decodes locally and reports its own frame/chunk/ended counts, so this
    // is the expectation, not a reference WAV (this test's own header
    // comment says why byte-exact is not the right bar here).
    {
        std::ofstream expected(directory / "expected.txt");
        expected << "frames=" << expected_frames << '\n';
        expected << "sample_rate=48000\n";
        REQUIRE(expected.good());
    }

    // The last units and stream/end are on their way to the scripted player;
    // give the connections a moment before tearing down, the same as
    // test_aiosendspin.cpp's own exit.
    std::this_thread::sleep_for(1s);
    group.reset();
    host->reset();
}
