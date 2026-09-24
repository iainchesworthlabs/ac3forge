#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
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
#include <string_view>
#include <thread>
#include <vector>

#include "platform/process.hpp"

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"
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
// The programme is E-AC-3, needed for the burst-taking test sink to have anything to take, and
// AC-3/E-AC-3 is lossy - there is no reference PCM this could byte-match the way
// test_aiosendspin.cpp's own hand-pushed, uncompressed samples do, by just keeping a copy of what
// was pushed. What this test keeps instead is a local decode and render of the same access units -
// the way test_group.cpp's own decode_and_render() already proves, in play_joc_programme() against
// a real sink's own recorded WAV, reproduces a real member's audio sample for sample - carried
// through the same two steps NetworkGroupSink::submit_pcm() (network_group_sink.cpp) and
// ac3::sendspin::Group take before a PCM member's encoder ever sees a sample: the group only ever
// pushes its own full 32-bit scale, and Group rescales each member down to whatever bit depth it
// negotiated (server_host.cpp's rescaled(), an exact bit shift, never lossy rounding). Applying that
// same float-to-int32-to-16-bit chain to the local decode's own float PCM, before it goes to
// programme.wav, gives a reference in the same shape as test_aiosendspin.cpp's own programme.wav -
// so aiosendspin_group_exit.py's check() can byte-compare the player's own decoded.wav against it,
// the same rigour aiosendspin_exit.py's own check() already applies to A4's PCM and FLAC. What real
// third-party interop additionally needs proving - pairing, handshake, negotiation, a complete
// stream that decodes without error, in step with what the test sink heard - is unchanged from
// before, the same rigour test_engine_network_group.cpp already applies to the test sinks. Codec
// correctness itself is exhaustively covered elsewhere in this suite.
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

std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

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

// A sample at full 32-bit scale, exactly as network_group_sink.cpp's own (anonymous-namespace,
// unreachable from here) to_sample() computes it for NetworkGroupSink::submit_pcm() - kept in
// lockstep by hand so the reference this builds matches what a real PCM member actually receives,
// bit for bit. If that function ever changes, this one must change with it.
[[nodiscard]] std::int32_t to_sample(float value) {
    const double scaled = std::clamp(static_cast<double>(value), -1.0, 1.0) * 2147483647.0;
    return static_cast<std::int32_t>(std::lround(scaled));
}

// A local decode and render of `stream`'s access units, `passes` times over, as
// test_group.cpp's own decode_and_render() does (that file's own comment explains the technique;
// duplicated here rather than shared, the same as this file's other small test helpers, e.g.
// HostEvents and QuietLog above): each block of `layout`'s slots to `consume`. Unlike that
// function's own caller (play_joc_programme(), only ever used with layouts wider than two
// channels), `layout` here is "2.0" - OutputLayout::fold() gives a stereo layout a real fold
// (serving.hpp's own comment: "a stereo or mono room is the decoder's fold"), so
// serving::serve() legitimately returns a populated serving.fold (Lo/Ro) rather than
// std::nullopt. That fold is exactly what a real member's own decode uses too
// (decoder_setup(), decoder_settings.cpp, for the same "2.0" layout and the same
// DecoderSettings{} default stereo_fold) - the point of this function is to match that, not to
// assert it away.
template <class Consume>
void decode_and_render(const ac3::io::ScannedStream& stream, int passes, const ac3::render::OutputLayout& layout,
                       Consume&& consume) {
    const ac3::render::Serving serving =
        ac3::render::serve(layout, ac3::DownmixTarget::kLoRo, ac3::render::ObjectsPolicy::kAuto);
    ac3::DecoderConfig config;
    config.output.mode = ac3::OperatingMode::kLine;
    ac3::render::configure_decoder(serving, config);
    ac3::Eac3Decoder decoder(config);
    ac3::render::LayoutRenderer renderer(layout);
    const std::size_t slots = layout.slots();
    std::vector<std::array<float, ac3::kSamplesPerBlock>> block(slots);
    std::vector<std::span<float>> spans;
    for (std::array<float, ac3::kSamplesPerBlock>& slot : block) {
        spans.emplace_back(slot);
    }
    // Each unit's bed, taken by its first block whichever call delivers it.
    std::deque<ac3::eac3::chanmap::Layout> beds;
    for (int pass = 0; pass < passes; ++pass) {
        for (const std::span<const std::byte> unit : stream.access_units) {
            const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> scanned = ac3::io::scan(unit);
            REQUIRE(scanned.has_value());
            beds.push_back(ac3::eac3::chanmap::expand(scanned->channel_map));
            const auto decoded = decoder.decode_access_unit_by_block(unit, [&](const ac3::PcmBlock& pcm) {
                if (pcm.index == 0) {
                    renderer.set_bed(beds.front());
                    beds.pop_front();
                    if (serving.reconstruct) {
                        renderer.set_objects(pcm.object_metadata, pcm.objects.size());
                    }
                }
                renderer.render(pcm, serving.reconstruct, 1.0F, spans);
                consume(std::span<const std::array<float, ac3::kSamplesPerBlock>>(block),
                        pcm.channels.empty() ? std::size_t{0} : pcm.channels.front().size());
            });
            REQUIRE(decoded.has_value());
        }
    }
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

    // What aiosendspin_group_exit.py checks the scripted player's own decoded.wav against: a local
    // decode and render of `programme` (this file's own header comment explains why this, rather
    // than a copy of the pushed samples, is the reference here), carried through the same
    // float-to-int32-to-16-bit chain NetworkGroupSink::submit_pcm() and Group::rescaled() apply
    // before a PCM member's encoder ever sees a sample.
    const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> stream = ac3::io::scan(programme);
    REQUIRE(stream.has_value());
    std::vector<std::byte> reference_bytes;
    decode_and_render(*stream, /*passes=*/1, *layout,
                      [&](std::span<const std::array<float, ac3::kSamplesPerBlock>> block, std::size_t n) {
                          for (std::size_t t = 0; t < n; ++t) {
                              for (std::size_t slot = 0; slot < block.size(); ++slot) {
                                  // network_group_sink.cpp's kBitDepth (32) down to
                                  // aiosendspin_player.py's own BIT_DEPTH (16) - server_host.cpp's
                                  // rescaled(sample, 32, 16), an arithmetic shift that always lands
                                  // in int16_t range.
                                  const std::int32_t at_16_bit = to_sample(block[slot][t]) >> 16;
                                  const auto word = static_cast<std::uint16_t>(static_cast<std::int16_t>(at_16_bit));
                                  reference_bytes.push_back(static_cast<std::byte>(word & 0xFFU));
                                  reference_bytes.push_back(static_cast<std::byte>(word >> 8U));
                              }
                          }
                      });
    REQUIRE(ac3::io::write_wav_pcm16_raw((directory / "programme.wav").string(), reference_bytes, 48000,
                                         static_cast<std::uint16_t>(layout->slots()))
               .has_value());

    // The last units and stream/end are on their way to the scripted player;
    // give the connections a moment before tearing down, the same as
    // test_aiosendspin.cpp's own exit.
    std::this_thread::sleep_for(1s);
    group.reset();
    host->reset();
}
