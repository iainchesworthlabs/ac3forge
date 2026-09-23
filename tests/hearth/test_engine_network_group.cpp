#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "engine_thread.hpp"
#include "network_group_sink.hpp"
#include "sink.hpp"

// Issue #874's own exit (planning/hearth-reference-player.md, A6): "from the
// app, a group of two test sinks... plays one programme." Unlike
// test_network_group_sink.cpp, which drives NetworkGroupSink directly to
// prove the wrapper's own translation, this drives it from
// ac3::hearth::Engine - the same class HearthController wraps - exactly the
// way HearthController::start()/selectOutputGroup() do: an EngineOutputs
// with a real group resolver, OutputPreferences pinning kNetworkGroup, a
// queue item added and played. What test_group.cpp and PR #932's mixed-
// delivery case already prove at the Group level, and test_network_group_
// sink.cpp proves at the NetworkGroupSink level, this proves end to end
// through the whole application stack bar Qt itself.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
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

// `frames` E-AC-3 access units of a 440 Hz tone, concatenated as one
// elementary stream - test_engine.cpp's own eac3_stream(), which this
// mirrors: each unit is a full six-block (1,536-sample, 32 ms) burst on its
// own, so `frames` * 32 ms is the programme's real-time length.
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

// Polls until `done` holds, or gives up after a generous while - test_engine.cpp's own.
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

TEST_CASE("engine: from the app, a group of two test sinks plays one programme",
         "[hearth][group][websocket]") {
    constexpr int kFrameCount = 20;  // 640 ms
    const fs::path scratch =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> pcm_sink =
        start_sink(scratch / "pcm", "PCM sink", /*extension_role=*/false, /*unpaired_access=*/true, log);
    const std::unique_ptr<testsink::Sink> burst_sink =
        start_sink(scratch / "burst", "Burst sink", /*extension_role=*/true, /*unpaired_access=*/false, log);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1",
         .port = std::nullopt, .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(burst_sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    REQUIRE((*host)->approve(pcm_sink->client_id(), true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Living room");
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }

    // The queue item, in memory - test_engine.cpp's own Library pattern:
    // ItemLoader takes an arbitrary key, not a real path, so this needs no
    // scratch file of its own.
    const std::vector<std::byte> programme = eac3_stream(kFrameCount);
    const ItemLoader loader = [&programme](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };

    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    // No local pcm/bitstream/endpoints: this group is the only output there
    // is, so a fallback silently playing somewhere else would show up as a
    // refusal (EngineStatus::note), not a false pass.
    EngineOutputs outputs{.group = ac3::hearth::make_group_sink(
                              [&group](const std::string&) { return group; })};
    Engine engine(std::move(outputs), loader, *layout, ac3::hearth::DecoderSettings{},
                 EngineTiming{.period = 5ms, .budget = 4800});
    engine.set_output_preferences(OutputPreferences{.pinned = OutputMode::kNetworkGroup,
                                                     .follow_sink = true,
                                                     .group_name = group->id(),
                                                     .group_ready = true});
    engine.add({QueueItem{.path = "programme", .title = "Test programme"}});
    engine.play();

    // The queue is one item, so the transport stops once it has played out
    // - by then the output has closed behind it (test_engine.cpp's own
    // completion checks all read history.size() alongside kStopped for the
    // same reason: nothing here guarantees output.mode is still set by that
    // point). history itself is the record of what was actually played.
    REQUIRE(eventually([&] {
        const EngineStatus status = engine.status();
        return status.state == TransportState::kStopped && !status.history.empty();
    }));
    const EngineStatus finished = engine.status();
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    CHECK(finished.history.front().frames == finished.history.front().expected_frames);

    const auto played_pcm = [&] { return pcm_sink->totals().frames; };
    const auto played_bursts = [&] { return burst_sink->totals().bursts; };
    const std::uint64_t expected_frames = static_cast<std::uint64_t>(kFrameCount) * 1536;
    REQUIRE(eventually([&] { return played_pcm() >= expected_frames && played_bursts() >= 1; }));

    CHECK(played_pcm() == expected_frames);
    CHECK(played_bursts() == static_cast<std::uint64_t>(kFrameCount));
    CHECK(burst_sink->totals().burst_frames == expected_frames);
    // Both sinks heard the same programme, not each a different fraction of
    // it: the group plays in step, the same proof test_group.cpp's own
    // exit makes for the library alone.
    CHECK(pcm_sink->totals().connections == 1);
    CHECK(burst_sink->totals().connections == 1);
}
