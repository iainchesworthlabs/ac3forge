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

#include "platform/process.hpp"

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "burst_output.hpp"
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

std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

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

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// The first `count` sync frames of `path`, whose first is an I-frame, as an
// elementary stream of their own.
std::vector<std::byte> ac4_frames(const fs::path& path, std::size_t count) {
    const std::vector<std::byte> file = read_bytes(path);
    const ac4::ScanResult scanned = ac4::scan(file);
    REQUIRE(scanned.frames.size() > count);
    return {file.begin(), file.begin() + static_cast<std::ptrdiff_t>(scanned.frames[count].offset)};
}

// The only file in `directory` whose name starts with `prefix` and ends with `extension`.
fs::path only_file(const fs::path& directory, std::string_view prefix, std::string_view extension) {
    std::vector<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(extension)) {
            found.push_back(entry.path());
        }
    }
    REQUIRE(found.size() == 1);
    return found.front();
}

// The local time each logged burst puts the stream's first frame at
// (test_group.cpp's own).
std::vector<double> first_frame_times(const fs::path& log) {
    std::vector<double> times;
    std::ifstream in(log);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string local;
        std::string first;
        if (!std::getline(fields, local, ',') || !std::getline(fields, first, ',') ||
            local == "clear") {
            continue;
        }
        times.push_back(std::stod(local) - (std::stod(first) * 1'000'000.0 / 48000.0));
    }
    return times;
}

// A host with a PCM sink (player@v1, approved unpaired) and a burst sink
// (_ac3forge_player@v1, paired) both playing, and a group of the two.
struct TwoSinkGroup {
    QuietLog log;
    std::unique_ptr<testsink::Sink> pcm_sink;
    std::unique_ptr<testsink::Sink> burst_sink;
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    std::unique_ptr<ac3::sendspin::ServerHost> host;
    std::shared_ptr<ac3::sendspin::Group> group;

    explicit TwoSinkGroup(const fs::path& scratch) {
        pcm_sink = start_sink(scratch / "pcm", "PCM sink", /*extension_role=*/false,
                              /*unpaired_access=*/true, log);
        burst_sink = start_sink(scratch / "burst", "Burst sink", /*extension_role=*/true,
                                /*unpaired_access=*/false, log);
        std::optional<ac3::sendspin::noise::KeyPair> identity =
            ac3::sendspin::noise::KeyPair::generate();
        REQUIRE(identity.has_value());
        auto started = ac3::sendspin::ServerHost::start({.identity = *identity,
                                                         .name = "Test host",
                                                         .languages = {"en"},
                                                         .address = "127.0.0.1",
                                                         .port = std::nullopt,
                                                         .advertise = false,
                                                         .browse = false,
                                                         .mdns_interfaces = {}},
                                                        store, events);
        REQUIRE(started.has_value());
        host = std::move(*started);
        REQUIRE(host->enter_pairing_token(burst_sink->pairing_token()));
        host->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
        host->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
        REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
        REQUIRE(host->approve(pcm_sink->client_id(), true));
        REQUIRE(events.wait(
            [](const auto& clients) {
                return clients.size() == 2 &&
                       std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                           return entry.second.playing && entry.second.available;
                       });
            },
            20s));
        group = host->make_group("Living room");
        for (const ac3::sendspin::ClientView& client : host->clients()) {
            group->add(client.client_id);
        }
    }
    TwoSinkGroup(const TwoSinkGroup&) = delete;
    TwoSinkGroup& operator=(const TwoSinkGroup&) = delete;

    ~TwoSinkGroup() {
        // A group must not outlive its host.
        group.reset();
        host.reset();
    }
};

// Plays `programme` from an Engine whose only output is `group`, with
// `settings`, to its end.
EngineStatus play_to_group(const std::shared_ptr<ac3::sendspin::Group>& group,
                           std::vector<std::byte> programme,
                           const ac3::hearth::DecoderSettings& settings) {
    const ItemLoader loader =
        [programme = std::move(programme)](
            const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such file: " + path);
        }
        return LoadedItem{.bytes = programme};
    };
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    EngineOutputs outputs{
        .group = ac3::hearth::make_group_sink([group](const std::string&) { return group; })};
    Engine engine(std::move(outputs), loader, *layout, settings,
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
    return engine.status();
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

// planning/ac4.md, I2: "AC-4 decodes to PCM for every output, and is sent as a
// bitstream only over the extension role ... to sinks that decode it." One
// AC-4 item from the Engine to a group of a player@v1 sink and an
// _ac3forge_player@v1 sink that lists AC-4 (D11's test sink): the first takes
// the Engine's decode as PCM, and the second the item's own sync frames, a
// burst each, which it decodes itself - to what ac4::Decoder makes of them,
// sample for sample, every burst's frame placed on the group's one timeline.
TEST_CASE(
    "engine: an AC-4 item reaches a group as PCM, and as a bitstream for the sink that decodes it",
    "[hearth][group][websocket][ac4]") {
    constexpr std::size_t kFrames = 24;  // DEE's 2.0 tones at frame_rate_index 13: 1.05 s
    const fs::path scratch =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_engine_group_ac4_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const std::vector<std::byte> programme = ac4_frames(
        fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4", kFrames);
    TwoSinkGroup sinks(scratch);
    const EngineStatus finished =
        play_to_group(sinks.group, programme, ac3::hearth::DecoderSettings{});
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    const std::uint64_t expected = static_cast<std::uint64_t>(kFrames) * 2048;
    CHECK(finished.history.front().frames == expected);

    const auto played_pcm = [&] { return sinks.pcm_sink->totals().frames; };
    const auto played_bursts = [&] { return sinks.burst_sink->totals().bursts; };
    REQUIRE(eventually([&] { return played_pcm() >= expected && played_bursts() >= kFrames; }));
    CHECK(played_pcm() == expected);
    CHECK(played_bursts() == kFrames);
    CHECK(sinks.burst_sink->totals().burst_frames == expected);

    // The burst sink's WAV: the frames as ac4::Decoder decodes them, placed
    // on its layout - the test sink's own, 7.1.4 - by the bed of their
    // speakers, as BurstOutput places them.
    const std::optional<ac3::render::OutputLayout> layout =
        ac3::render::OutputLayout::parse(testsink::SinkOptions{}.layout);
    REQUIRE(layout.has_value());
    ac3::io::WavStreamReader wav;
    REQUIRE(wav.open(only_file(scratch / "burst" / "out", "bursts-", ".wav").string()).has_value());
    REQUIRE(static_cast<std::size_t>(wav.channels()) == layout->slots());
    std::vector<std::vector<float>> heard(layout->slots(),
                                          std::vector<float>(ac3::kSamplesPerBlock));
    std::vector<std::span<float>> heard_spans(heard.begin(), heard.end());
    std::vector<std::array<float, ac3::kSamplesPerBlock>> rendered(layout->slots());
    std::vector<std::span<float>> rendered_spans;
    for (std::array<float, ac3::kSamplesPerBlock>& slot : rendered) {
        rendered_spans.emplace_back(slot);
    }
    ac4::Decoder decoder;
    ac3::render::LayoutRenderer renderer(*layout);
    std::uint64_t compared = 0;
    std::uint64_t different = 0;
    for (const ac4::SyncFrame& frame : ac4::scan(programme).frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        renderer.set_bed(testsink::ac4_bed(pcm.speakers));
        std::vector<std::span<const float>> channels(pcm.channels.size());
        for (std::size_t at = 0; at < pcm.samples; at += ac3::kSamplesPerBlock) {
            const std::size_t m = std::min<std::size_t>(ac3::kSamplesPerBlock, pcm.samples - at);
            for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
                channels[c] = std::span<const float>(pcm.channels[c]).subspan(at, m);
            }
            renderer.render(ac3::PcmBlock{.index = 0,
                                          .blocks = 1,
                                          .channels = channels,
                                          .objects = {},
                                          .object_indices = {},
                                          .object_metadata = nullptr},
                            false, 1.0F, rendered_spans);
            const auto got = wav.read_planar(heard_spans, m);
            REQUIRE(got.has_value());
            REQUIRE(*got == m);
            for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
                for (std::size_t t = 0; t < m; ++t) {
                    different += rendered[slot][t] == heard[slot][t] ? 0U : 1U;
                }
            }
            compared += m;
        }
    }
    CHECK(compared == expected);
    CHECK(different == 0);
    // Every burst puts the stream's first frame at the same local time, within
    // 1 ms: each is placed at its own frame's start.
    const std::vector<double> times =
        first_frame_times(only_file(scratch / "burst" / "out", "bursts-", ".times.csv"));
    REQUIRE(times.size() == kFrames);
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}

// A sink decodes the presentation it would choose with no preferences, so a
// presentation the listener has chosen that it would not reaches the group as
// PCM alone: the _ac3forge_player@v1 sink is sent nothing.
TEST_CASE("engine: an AC-4 presentation a sink would not choose reaches a group as PCM alone",
          "[hearth][group][websocket][ac4]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} /
                             ("hearth_engine_group_ac4_choice_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    // E6's broadcast stream: presentation 2 is music and effects with the
    // German dialogue, not the one a decoder with no preferences selects.
    const std::vector<std::byte> programme =
        read_bytes(fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "encoder-broadcast.ac4");
    TwoSinkGroup sinks(scratch);
    ac3::hearth::DecoderSettings settings;
    settings.ac4.presentation_id = 2;
    const EngineStatus finished = play_to_group(sinks.group, programme, settings);
    INFO("output_reason: " << finished.output_reason << " / note: " << finished.note
                           << " / error: " << finished.error);
    REQUIRE(finished.history.size() == 1);
    const std::uint64_t expected = finished.history.front().expected_frames;
    CHECK(finished.history.front().frames == expected);
    REQUIRE(eventually([&] { return sinks.pcm_sink->totals().frames >= expected; }));
    CHECK(sinks.pcm_sink->totals().frames == expected);
    CHECK(sinks.burst_sink->totals().burst_streams == 0U);
    CHECK(sinks.burst_sink->totals().bursts == 0U);
}
