#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"

#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "network_group_sink.hpp"
#include "sink.hpp"

// ac3::hearth::NetworkGroupSink (apps/hearth/engine/network_group_sink.cpp)
// against a real ac3::sendspin::Group and two real in-process test sinks
// (apps/hearth/testsink), proving what Player itself does not re-prove: that
// this sink's own translation is correct - planar float to Group::push()'s
// interleaved int32 (checked sample for sample against the sink's WAV, the
// same way tests/hearth/test_group.cpp checks Group itself), and a burst's
// pc/pd/payload/frame forwarded to Group::push_burst() unchanged (checked by
// the sink's own burst count and frame total, since decoding a real AC-3
// frame correctly is what test_group.cpp and the wider codec suite already
// cover - this is about the wrapper, not the codec).
//
// It dials, so under ThreadSanitizer it needs what tests/sendspin/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;

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

// `unpaired_access` matches test_group.cpp's own two flows: a plain
// player@v1 sink is approved for unpaired access (needs no pairing to play
// PCM), while an _ac3forge_player@v1 sink is paired by its token instead
// (below) - SinkOptions::unpaired_access defaults to false, which
// test_group.cpp's own paired sinks rely on rather than setting explicitly,
// so this does too rather than guessing the extension role also works
// unpaired.
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

// One real, valid AC-3 access unit: a quiet stereo tone, 48 kHz.
std::vector<std::byte> ac3_unit() {
    ac3::EncoderConfig config;
    config.sample_rate = ac3::SampleRate::k48000;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::FrameEncoder encoder{config};
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        samples[n] = 0.1F * std::sin(static_cast<float>(n) * 0.1F);
    }
    const std::vector<std::span<const float>> views(2, samples);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

}  // namespace

TEST_CASE("network group sink: PCM and a burst reach real sinks through the wrapper",
         "[hearth][group][websocket]") {
    const fs::path scratch =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_sink_" + scratch_pid_suffix());
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
    // burst_sink pairs by its own token, entered before it even connects
    // (test_group.cpp's own recipe for an _ac3forge_player@v1 sink) - it
    // needs no separate approve() below, unlike pcm_sink's unpaired access.
    REQUIRE((*host)->enter_pairing_token(burst_sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(pcm_sink->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(burst_sink->port()) + "/sendspin");
    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    const std::string pcm_client_id = pcm_sink->client_id();
    REQUIRE((*host)->approve(pcm_client_id, true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    // Diagnostic: which negotiated the extension role. bursts=false is
    // fine for pcm_sink (extension_role=false forces player@v1 there);
    // burst_sink (extension_role=true, default) is expected to have
    // negotiated _ac3forge_player@v1 (bursts=true) - if it did not, no
    // burst this test sends can ever reach it, whatever Group does.
    std::size_t clients_with_bursts = 0;
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        INFO("client " << client.name << " playing=" << client.playing << " bursts=" << client.bursts
                       << " available=" << client.available);
        clients_with_bursts += client.bursts ? 1U : 0U;
    }
    CHECK(clients_with_bursts == 1);

    // The resolver a real HearthController<->NetworkController coupling
    // would supply - here, just the one group this test made.
    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Test group");
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }
    const std::unique_ptr<ac3::hearth::NetworkGroupSink> sink =
        ac3::hearth::make_group_sink([&](const std::string&) { return group; });

    const ac3::render::OutputLayout layout = ac3::render::OutputLayout::named("2.0").value();
    const auto opened = sink->open(
        "Test group",
        ac3::hearth::NetworkGroupSink::Format{
            .sample_rate = 48000, .layout = layout, .stream = ac3::audio::BitstreamFormat::kAc3});
    REQUIRE(opened.has_value());
    CHECK(opened->mode == ac3::hearth::OutputMode::kNetworkGroup);

    // PCM: a known tone, pushed until every frame is taken, planar - the
    // shape Player::drain_group() itself offers.
    constexpr std::size_t kFrames = 4800;  // 0.1 s at 48 kHz
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);
    for (std::size_t n = 0; n < kFrames; ++n) {
        left[n] = 0.5F * std::sin(static_cast<float>(n) * 0.05F);
        right[n] = -0.5F * std::sin(static_cast<float>(n) * 0.05F);
    }
    std::size_t offset = 0;
    const auto pcm_deadline = std::chrono::steady_clock::now() + 30s;
    while (offset < kFrames && std::chrono::steady_clock::now() < pcm_deadline) {
        const std::array<std::span<const float>, 2> remaining{
            std::span<const float>(left).subspan(offset), std::span<const float>(right).subspan(offset)};
        const std::size_t taken = sink->submit_pcm(remaining, kFrames - offset);
        if (taken == 0) {
            std::this_thread::sleep_for(10ms);
        }
        offset += taken;
    }
    REQUIRE(offset == kFrames);
    // Taken is not played: the group reads ahead of its own timeline, which
    // starts a lead after the first push, so the tone is all still queued -
    // Player waits on this before it closes the output, or the programme's
    // last second or so would never be heard.
    {
        const auto position = sink->position();
        REQUIRE(position.has_value());
        CHECK(position->frames_played + position->frames_queued == kFrames);
        CHECK(position->frames_played < kFrames);
    }

    // A burst: one real AC-3 frame, wrapped exactly as Player's own
    // send_unit() would, pc/pd read back from the wrap the same way
    // (player.cpp's own comment on iec61937's stable, documented preamble
    // layout explains why that read-back is safe rather than a guess).
    const std::vector<std::byte> unit = ac3_unit();
    const auto wrapped = ac3::iec61937::wrap_frame(unit);
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->size() >= 8);
    const auto byte_at = [&](std::size_t i) { return std::to_integer<unsigned>((*wrapped)[i]); };
    const auto pc = static_cast<std::uint16_t>(byte_at(4) | (byte_at(5) << 8));
    const auto pd = static_cast<std::uint16_t>(byte_at(6) | (byte_at(7) << 8));
    // Group::push_burst() returns true once the group's own pacing accepts
    // a burst, not once a member has actually received it - a member only
    // takes bursts once its own `started` flag catches up, asynchronously,
    // shortly after add()/start() (server_host.cpp's own Group::push_burst()
    // skips a member silently until then, and still returns true). A single
    // burst can race that, so this resends at the real burst period
    // (kSamplesPerFrame is 1,536, matching push_burst()'s own "one burst,
    // of 1,536 samples") until the sink actually counts one - the same
    // retry-until-taken shape Player's own drain_group() uses, just kept
    // running past the first "taken" to outlast the member's own start-up.
    const auto played_pcm = [&] { return pcm_sink->totals().frames; };
    const auto played_burst = [&] { return burst_sink->totals().bursts; };
    std::int64_t next_frame = 0;
    const auto burst_deadline = std::chrono::steady_clock::now() + 30s;
    while (played_burst() < 1 && std::chrono::steady_clock::now() < burst_deadline) {
        if (sink->submit_burst(pc, pd, unit, next_frame)) {
            next_frame += ac3::kSamplesPerFrame;
        }
        std::this_thread::sleep_for(32ms);
    }

    const auto until = std::chrono::steady_clock::now() + 10s;
    while ((played_pcm() < kFrames || played_burst() < 1) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(played_pcm() == kFrames);
    CHECK(played_burst() >= 1);
    CHECK(burst_sink->totals().burst_frames >= static_cast<std::uint64_t>(ac3::kSamplesPerFrame));
    // The group's timeline runs through it within its lead (a sink may count
    // a chunk as it arrives, before its time).
    {
        const auto timeline_deadline = std::chrono::steady_clock::now() + 10s;
        while (sink->position()->frames_played < kFrames && std::chrono::steady_clock::now() < timeline_deadline) {
            std::this_thread::sleep_for(20ms);
        }
        const auto position = sink->position();
        REQUIRE(position.has_value());
        CHECK(position->frames_played == kFrames);
        CHECK(position->frames_queued == 0);
    }

    sink->close();
    group.reset();
    host->reset();

    // The PCM sink's WAV is the tone, sample for sample - the float to
    // int32 conversion round-tripped through a real 16-bit player@v1
    // stream and back without a scale or a sign error.
    const auto wav = ac3::io::read_wav((scratch / "pcm" / "out" / "stream-1-1.wav").string());
    REQUIRE(wav.has_value());
    REQUIRE(wav->frame_count() == kFrames);
    std::size_t different = 0;
    for (std::size_t n = 0; n < kFrames; ++n) {
        different += std::abs(wav->channels[0][n] - left[n]) > 0.001F ? 1U : 0U;
        different += std::abs(wav->channels[1][n] - right[n]) > 0.001F ? 1U : 0U;
    }
    CHECK(different == 0);
}
