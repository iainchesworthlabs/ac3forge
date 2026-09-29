#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "ac4/ac4.hpp"
#include "ac4_stream.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "decoder_settings.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"

// The Hearth engine's AC-4 path (planning/ac4.md, phase I2): an AC-4 item
// opened by Session, decoded by StreamDecoder through ac4::Decoder's public API
// alone, rendered onto the output layout, and played by Player.
//
// The exit: "the engine plays every committed AC-4 stream through the
// decoder's public API; each control ... changes the decoded output as its
// formula says, measured with tones". The first is held sample for sample
// against ac4::Decoder's own decode() of the same frames, placed by speaker on
// a layout with a slot for each; the second on streams of tones, the encoder's
// here and E6's committed ones, each control's effect measured at its tone's
// frequency against ETSI TS 103 190-1's formula for it, with the stream's own
// values. The formulas are gain_ac4_decode.py's and the E6 tests', which hold
// the decoder to them; what these hold is that the engine's settings reach
// the decoder as the page means them.

namespace {

namespace fs = std::filesystem;
using ac3::hearth::Ac4Settings;
using ac3::hearth::DecoderSettings;
using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;
using ac3::hearth::Player;
using ac3::hearth::QueueItem;
using ac3::hearth::Session;
using ac3::hearth::StreamDecoder;
using ac3::hearth::UnitReport;

// A slot for every speaker ac4::Decoder names, each at its own location, so
// the renderer puts each decoded channel on one slot at a gain of exactly 1.
// The top back and top side pairs share Lts and Rts, as ac4_bed() maps them:
// no stream has both.
constexpr std::string_view kEverySpeaker = "L,R,C,LFE,Ls,Rs,Lrs,Rrs,Lw,Rw,Vhl,Vhr,Lts,Rts,LFE2";

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

fs::path baseline(std::string_view leg) {
    return fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
}

// Every committed AC-4 stream: DEE's, the constructed ones and the
// multiplexed presentations with their sources - the set
// tests/ac4dec/test_ac4dec_api.cpp decodes through the API.
std::vector<fs::path> committed_streams() {
    std::vector<fs::path> paths;
    for (const fs::path& root :
         {fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR}, fs::path{AC4DEC_GOLDEN_DIR}}) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ac4") {
                paths.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(paths);
    return paths;
}

ItemLoader loader_of(std::map<std::string, std::vector<std::byte>> items) {
    return [items = std::move(items)](
               const std::string& path) -> std::expected<LoadedItem, std::string> {
        const auto found = items.find(path);
        if (found == items.end()) {
            return std::unexpected("no such item: " + path);
        }
        return LoadedItem{.bytes = found->second};
    };
}

ac3::render::OutputLayout layout_of(std::string_view text) {
    const auto layout = ac3::render::OutputLayout::parse(text);
    REQUIRE(layout.has_value());
    return *layout;
}

// Settings with no output processing: the coded level, no compression.
DecoderSettings as_coded() {
    DecoderSettings settings;
    settings.ac4.normalise = false;
    settings.ac4.drc = ac4::DrcMode::kOff;
    return settings;
}

// What a session put out through a decoder: each slot end to end, each unit's
// report, and what would not decode.
struct Played {
    std::vector<std::vector<float>> slots;
    std::vector<UnitReport> reports;
    std::vector<std::string> errors;
    std::size_t frames = 0;
};

Played play(Session& session, StreamDecoder& decoder) {
    Played out;
    out.slots.resize(decoder.layout().slots());
    const StreamDecoder::BlockFn deliver = [&out](std::span<const std::span<const float>> slots,
                                                  std::size_t n) {
        for (std::size_t s = 0; s < slots.size(); ++s) {
            REQUIRE(slots[s].size() == n);
            out.slots[s].insert(out.slots[s].end(), slots[s].begin(), slots[s].end());
        }
        out.frames += n;
    };
    const Session::ReportFn reported = [&out](const UnitReport& report, std::size_t) {
        out.reports.push_back(report);
    };
    while (!session.finished()) {
        const auto got = session.render(decoder, deliver, std::size_t{1} << 20U, reported);
        if (!got) {
            out.errors.push_back(got.error());
        }
    }
    return out;
}

// Opens `bytes` as an item and plays it through a StreamDecoder on `layout`.
Played play_item(const std::vector<std::byte>& bytes, const ac3::render::OutputLayout& layout,
                 const DecoderSettings& settings) {
    auto session = Session::open("item", loader_of({{"item", bytes}}), std::nullopt,
                                 ac3::hearth::presentation_choice(settings));
    INFO((session ? std::string{} : session.error()));
    REQUIRE(session.has_value());
    StreamDecoder decoder{layout, session->facts().sample_rate, settings};
    return play(*session, decoder);
}

// ac4::Decoder's own decode() of `bytes` with `config`, each channel on the
// slot of `layout` at its speaker's location, and a frame that waits for an
// I-frame as silence of its length.
std::vector<std::vector<float>> reference(const std::vector<std::byte>& bytes,
                                          const ac3::render::OutputLayout& layout,
                                          const ac4::DecoderConfig& config) {
    const auto units = ac3::hearth::read_ac4_units(bytes);
    REQUIRE(units.has_value());
    std::vector<std::vector<float>> out(layout.slots());
    ac4::Decoder decoder(config);
    for (std::size_t i = 0; i < units->frames.size(); ++i) {
        const auto decoded = decoder.decode(ac3::hearth::raw_frame_of(units->frames[i]));
        INFO("frame " << i << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            for (std::vector<float>& slot : out) {
                slot.insert(slot.end(), units->samples[i], 0.0F);
            }
            continue;
        }
        const ac4::DecodedFrame& frame = **decoded;
        CHECK(frame.samples == units->samples[i]);
        const std::size_t start = out.front().size();
        for (std::vector<float>& slot : out) {
            slot.resize(start + frame.samples, 0.0F);
        }
        for (std::size_t c = 0; c < frame.channels.size(); ++c) {
            const ac3::eac3::chanmap::Layout bed =
                ac3::hearth::ac4_bed(std::span<const ac4::Speaker>(&frame.speakers[c], 1));
            const int slot = layout.index_of(bed[0]);
            REQUIRE(slot >= 0);
            std::copy(
                frame.channels[c].begin(), frame.channels[c].end(),
                out[static_cast<std::size_t>(slot)].begin() + static_cast<std::ptrdiff_t>(start));
        }
    }
    return out;
}

// --- Tones ---------------------------------------------------------------------------

constexpr int kRate = 48000;
constexpr std::size_t kFrame = 2048;
// One tone a channel, in the order ac4::Encoder takes 5.1 (L R C LFE Ls Rs):
// under Table 173's last dialogue enhancement band, the LFE's under 140 Hz.
constexpr std::array<double, 6> kTonesHz = {440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0};
// Where each is heard: a "5.1" layout's slots are in A/52's order, not this.
constexpr std::array<ac3::eac3::chanmap::Location, 6> kToneLocations = {
    ac3::eac3::chanmap::Location::kLeft,         ac3::eac3::chanmap::Location::kRight,
    ac3::eac3::chanmap::Location::kCentre,       ac3::eac3::chanmap::Location::kLfe,
    ac3::eac3::chanmap::Location::kLeftSurround, ac3::eac3::chanmap::Location::kRightSurround};
constexpr double kAmplitude = 0.1;
constexpr std::size_t kToneFrames = 32;
// The analysis window: past the encoder's delay, the decoder's and the first
// frames' overlap, and short of the flushed end.
constexpr std::size_t kFrom = 6 * kFrame;
constexpr std::size_t kTo = 28 * kFrame;
constexpr double kToleranceDb = 0.01;

// The values the tone stream carries for the controls to read (planning/
// ac4.md, "One control for both formats").
constexpr double kDialnorm = -24.0;
constexpr double kLoroCentreDb = -1.5;
constexpr double kLoroSurroundDb = -4.5;
constexpr double kLtrtCentreDb = -3.0;
constexpr double kLtrtSurroundDb = -6.0;
constexpr double kLfeDb = -4.5;
constexpr int kDeCapDb = 9;

double from_db(double db) {
    return std::pow(10.0, db / 20.0);
}

double db(double x) {
    return 20.0 * std::log10(x);
}

// 5.1 tones as ac4::Encoder writes them, in sync frames: dialnorm, the
// downmix's gains and the LFE's, Lt/Rt preferred, and dialogue enhancement's
// channel-independent method on C, whose parameters are then 1 in every band.
std::vector<std::byte> tone_stream(int iframe_interval = 8) {
    ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    config.iframe_interval = iframe_interval;
    config.dialnorm_db = kDialnorm;
    config.downmix = ac4::DownmixConfig{.loro_centre_db = kLoroCentreDb,
                                        .loro_surround_db = kLoroSurroundDb,
                                        .ltrt_centre_db = kLtrtCentreDb,
                                        .ltrt_surround_db = kLtrtSurroundDb,
                                        .lfe_db = kLfeDb,
                                        .preferred = ac4::PreferredDownmix::kLtRt,
                                        .loro_correction_db2 = std::nullopt,
                                        .ltrt_correction_db2 = std::nullopt};
    config.dialogue = ac4::DialogueConfig{};
    config.dialogue->max_gain_db = kDeCapDb;
    auto encoder = ac4::Encoder::create(config);
    INFO(ac4::Encoder::refusal_reason(config));
    REQUIRE(encoder.has_value());
    std::vector<std::vector<float>> input(6, std::vector<float>(kToneFrames * kFrame));
    for (std::size_t c = 0; c < input.size(); ++c) {
        for (std::size_t n = 0; n < input[c].size(); ++n) {
            input[c][n] =
                static_cast<float>(kAmplitude * std::sin(2.0 * std::numbers::pi * kTonesHz[c] *
                                                         static_cast<double>(n) / kRate));
        }
    }
    std::vector<std::byte> out;
    const auto append = [&out](const std::vector<ac4::EncodedFrame>& frames) {
        for (const ac4::EncodedFrame& frame : frames) {
            const std::vector<std::byte> wrapped = ac4::sync_frame(frame.raw_ac4_frame, false);
            out.insert(out.end(), wrapped.begin(), wrapped.end());
        }
    };
    std::vector<std::span<const float>> views(input.begin(), input.end());
    const auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    append(*frames);
    const auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    append(*rest);
    return out;
}

const std::vector<std::byte>& tones() {
    static const std::vector<std::byte> stream = tone_stream();
    return stream;
}

// The complex amplitude of the component at `hz` in `samples`, through a Hann
// window over [from, to).
std::complex<double> component(std::span<const float> samples, double hz, std::size_t from = kFrom,
                               std::size_t to = kTo) {
    REQUIRE(samples.size() >= to);
    const std::span<const float> window = samples.subspan(from, to - from);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    const auto n = static_cast<double>(window.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < window.size(); ++k) {
        const double hann =
            0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0)));
        sum += hann * static_cast<double>(window[k]) *
               std::polar(1.0, -w * static_cast<double>(from + k));
        weights += hann;
    }
    return 2.0 * sum / weights;
}

// A slot's tone, by the slot's name on `layout`.
std::complex<double> tone_in(const Played& played, const ac3::render::OutputLayout& layout,
                             ac3::eac3::chanmap::Location location, double hz) {
    const int slot = layout.index_of(location);
    REQUIRE(slot >= 0);
    return component(played.slots[static_cast<std::size_t>(slot)], hz);
}

// A capture device: plays what it is given at once, keeping every sample.
class CaptureSink final : public PcmSink {
   public:
    struct Log {
        std::vector<std::vector<float>> slots;
        std::uint32_t opens = 0;
        std::uint64_t played = 0;
        bool open = false;
    };

    explicit CaptureSink(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->open = true;
        log_->played = 0;
        log_->slots.resize(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = static_cast<std::uint16_t>(format.layout.slots()),
                                .mode = OutputMode::kLocalPcm};
    }
    void close() override { log_->open = false; }
    [[nodiscard]] bool is_open() const override { return log_->open; }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        for (std::size_t s = 0; s < slots.size() && s < log_->slots.size(); ++s) {
            log_->slots[s].insert(log_->slots[s].end(), slots[s].begin(),
                                  slots[s].begin() + static_cast<std::ptrdiff_t>(frames));
        }
        log_->played += frames;
        return true;
    }
    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return ac3::audio::MonitorPosition{
            .frames_played = log_->played, .frames_queued = 0, .latency_frames = 0};
    }
    void flush() override {
        for (std::vector<float>& slot : log_->slots) {
            slot.clear();
        }
        log_->played = 0;
    }
    bool pause() override { return true; }
    bool resume() override { return true; }

   private:
    std::shared_ptr<Log> log_;
};

// Pumps `player` until it has nothing left to do.
void pump_out(Player& player) {
    for (int guard = 0; guard < 100000 && player.active(); ++guard) {
        (void)player.pump();
    }
    REQUIRE_FALSE(player.active());
}

}  // namespace

// --- Session -------------------------------------------------------------------------

TEST_CASE("hearth ac4: an item's units last what the decoder puts out for each", "[hearth][ac4]") {
    // 29.97 fps alternates a frame between 1 601 and 1 602 samples; 25 fps
    // and index 13 do not alternate.
    for (const std::string_view leg :
         {"ac4-ims-music-64-2997", "ac4-ims-music-128-25", "ac4-20-tones-192"}) {
        INFO(leg);
        const std::vector<std::byte> bytes = read_file(baseline(leg));
        const auto units = ac3::hearth::read_ac4_units(bytes);
        REQUIRE(units.has_value());
        CHECK(units->unread == 0);
        ac4::Decoder decoder;
        std::uint64_t decoded = 0;
        for (std::size_t i = 0; i < units->frames.size(); ++i) {
            const auto frame = decoder.decode(ac3::hearth::raw_frame_of(units->frames[i]));
            REQUIRE(frame.has_value());
            if (frame->has_value()) {
                CHECK((*frame)->samples == units->samples[i]);
                ++decoded;
            }
        }
        CHECK(decoded == units->frames.size());

        auto session = Session::open("item", loader_of({{"item", bytes}}));
        REQUIRE(session.has_value());
        CHECK(session->ac4());
        CHECK(session->unit_count() == units->frames.size());
        std::uint64_t total = 0;
        for (const std::uint32_t samples : units->samples) {
            total += samples;
        }
        CHECK(session->total_samples() == total);
        const ac3::hearth::ItemFacts& facts = session->facts();
        REQUIRE(facts.stream.has_value());
        CHECK(ac3::audio::is_ac4(*facts.stream));
        CHECK(facts.sample_rate == 48000U);
        CHECK(facts.channels == 2U);
        REQUIRE(facts.duration.has_value());
        CHECK(facts.duration->count() == static_cast<std::int64_t>(total * 1000 / 48000));
        REQUIRE(facts.bitrate_kbps.has_value());
        CHECK(*facts.bitrate_kbps > 32.0);
    }
    // The alternation itself, which Table 47 gives phase by phase.
    const auto ims = ac3::hearth::read_ac4_units(read_file(baseline("ac4-ims-music-64-2997")));
    REQUIRE(ims.has_value());
    CHECK(std::ranges::count(ims->samples, 1601U) + std::ranges::count(ims->samples, 1602U) ==
          static_cast<std::ptrdiff_t>(ims->samples.size()));
    CHECK(std::ranges::count(ims->samples, 1602U) > std::ranges::count(ims->samples, 1601U));
}

TEST_CASE("hearth ac4: a stream this build cannot decode is refused when it opens",
          "[hearth][ac4]") {
    // No sync frame at all.
    const std::vector<std::byte> junk{std::byte{0xAC}, std::byte{0x40}, std::byte{0x00}};
    const auto opened = Session::open("junk", loader_of({{"junk", junk}}));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().find("\"junk\" cannot be played") == 0);
}

// --- Every committed stream ------------------------------------------------------------

TEST_CASE(
    "hearth ac4: the engine plays every committed AC-4 stream through the decoder's public API",
    "[hearth][ac4]") {
    const std::vector<fs::path> streams = committed_streams();
    REQUIRE(streams.size() >= 42);
    const ac3::render::OutputLayout layout = layout_of(kEverySpeaker);
    // What a listener gets: dialogue to -31 dBFS, the DRC mode for it.
    const DecoderSettings settings;
    const ac4::DecoderConfig config = ac3::hearth::decoder_setup(settings, layout).ac4;
    int played_whole = 0;
    std::map<std::string, int> refused;
    for (const fs::path& path : streams) {
        INFO("stream " << path.string());
        const std::vector<std::byte> bytes = read_file(path);
        // A stream none of whose presentations the decoder decodes - the
        // immersive legs, until the decoder has their channel elements - is
        // refused when it opens, with the decoder's own reason.
        const auto opened = Session::open("item", loader_of({{"item", bytes}}), std::nullopt,
                                          ac3::hearth::presentation_choice(settings));
        if (!opened) {
            const std::string& why = opened.error();
            const std::string_view refusal = "has no presentation this build decodes: ";
            const std::size_t at = why.find(refusal);
            REQUIRE(at != std::string::npos);
            ++refused[why.substr(at + refusal.size())];
            continue;
        }
        ++played_whole;
        const Played played = play_item(bytes, layout, settings);
        CHECK(played.errors.empty());
        const std::vector<std::vector<float>> expected = reference(bytes, layout, config);
        auto session = Session::open("item", loader_of({{"item", bytes}}));
        REQUIRE(session.has_value());
        CHECK(played.frames == session->total_samples());
        REQUIRE(played.slots.size() == expected.size());
        for (std::size_t slot = 0; slot < expected.size(); ++slot) {
            INFO("slot " << slot);
            REQUIRE(played.slots[slot].size() == expected[slot].size());
            CHECK(played.slots[slot] == expected[slot]);
        }
        // Each frame decoded was reported, as AC-4.
        REQUIRE_FALSE(played.reports.empty());
        CHECK(std::ranges::all_of(played.reports,
                                  [](const UnitReport& r) { return r.ac4.has_value(); }));
    }
    std::ostringstream summary;
    summary << played_whole << " of " << streams.size() << " committed streams played;";
    for (const auto& [reason, count] : refused) {
        summary << " " << count << " refused: " << reason << ";";
    }
    WARN(summary.str());
    // The 42 D8 decoded through the API, and any stream since that the
    // decoder decodes.
    CHECK(played_whole >= 42);
}

TEST_CASE("hearth ac4: the engine plays the streams of AC4DEC_API_STREAM_DIR", "[hearth][ac4]") {
    const char* dir = std::getenv("AC4DEC_API_STREAM_DIR");
    if (dir == nullptr) {
        SKIP("AC4DEC_API_STREAM_DIR is not set");
    }
    std::vector<fs::path> streams;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path{dir})) {
        if (entry.is_regular_file() && entry.path().extension() == ".ac4") {
            streams.push_back(entry.path());
        }
    }
    std::ranges::sort(streams);
    REQUIRE_FALSE(streams.empty());
    const ac3::render::OutputLayout layout = layout_of(kEverySpeaker);
    const DecoderSettings settings;
    std::map<std::string, int> refused;
    int played_whole = 0;
    for (const fs::path& path : streams) {
        const std::vector<std::byte> bytes = read_file(path);
        auto session = Session::open("item", loader_of({{"item", bytes}}), std::nullopt,
                                     ac3::hearth::presentation_choice(settings));
        if (!session) {
            ++refused[session.error().substr(session.error().find(' ') + 1)];
            continue;
        }
        StreamDecoder decoder{layout, session->facts().sample_rate, settings};
        const std::uint64_t total = session->total_samples();
        const Played played = play(*session, decoder);
        INFO("stream " << path.string());
        CHECK(played.frames == total);
        if (played.errors.empty()) {
            ++played_whole;
        } else {
            ++refused[played.errors.front()];
        }
    }
    std::ostringstream summary;
    summary << played_whole << " of " << streams.size() << " streams played whole;";
    for (const auto& [reason, count] : refused) {
        summary << " " << count << ": " << reason << ";";
    }
    WARN(summary.str());
    CHECK(played_whole > 0);
}

// --- Each control against its formula ------------------------------------------------------

TEST_CASE("hearth ac4: the output level takes dialogue to it as Part 1 clause 5.7.9.3.3 says",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    const Played coded = play_item(tones(), layout, as_coded());
    for (const double level : {-31.0, -24.0, -17.0, -6.0}) {
        DecoderSettings settings = as_coded();
        settings.ac4.normalise = true;
        settings.ac4.output_level_dbfs = level;
        const Played played = play_item(tones(), layout, settings);
        // 2^((Lout - dialnorm) / 6), in every channel alike.
        const double expected = db(std::pow(2.0, (level - kDialnorm) / 6.0));
        for (std::size_t c = 0; c < kTonesHz.size(); ++c) {
            CAPTURE(level, c);
            const std::complex<double> before =
                tone_in(coded, layout, kToneLocations[c], kTonesHz[c]);
            const std::complex<double> after =
                tone_in(played, layout, kToneLocations[c], kTonesHz[c]);
            REQUIRE(std::abs(before) > kAmplitude / 2.0);
            CHECK(std::abs(db(std::abs(after) / std::abs(before)) - expected) < kToleranceDb);
        }
    }
}

TEST_CASE("hearth ac4: dialogue enhancement raises the dialogue by its gain up to the stream's cap",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    const Played off = play_item(tones(), layout, as_coded());
    for (const double gain : {3.0, 6.0, 12.0}) {
        DecoderSettings settings = as_coded();
        settings.ac4.dialogue_enhancement_db = gain;
        const Played on = play_item(tones(), layout, settings);
        // Part 1 clause 5.7.8: C, whose parameters are 1 in every band, by
        // 1 + g with g = 10^(G / 20) - 1, G no more than the stream's cap;
        // every other channel as it was.
        for (std::size_t c = 0; c < kTonesHz.size(); ++c) {
            CAPTURE(gain, c);
            const std::complex<double> before =
                tone_in(off, layout, kToneLocations[c], kTonesHz[c]);
            REQUIRE(std::abs(before) > kAmplitude / 2.0);
            const double change = db(std::abs(tone_in(on, layout, kToneLocations[c], kTonesHz[c])) /
                                     std::abs(before));
            const bool centre = kToneLocations[c] == ac3::eac3::chanmap::Location::kCentre;
            CHECK(std::abs(change - (centre ? std::min(gain, static_cast<double>(kDeCapDb))
                                            : 0.0)) < kToleranceDb);
        }
    }
}

TEST_CASE("hearth ac4: a stereo or mono layout takes the downmix Part 1 clause 6.2.17 gives",
          "[hearth][ac4]") {
    using ac3::eac3::chanmap::Location;
    const ac3::render::OutputLayout wide = layout_of("5.1");
    const ac3::render::OutputLayout stereo = layout_of("2.0");
    const ac3::render::OutputLayout mono = layout_of("1.0");
    const Played coded = play_item(tones(), wide, as_coded());
    // Each coded tone, as it came out of its own channel.
    std::array<std::complex<double>, 6> source{};
    for (std::size_t c = 0; c < kTonesHz.size(); ++c) {
        source[c] = tone_in(coded, wide, kToneLocations[c], kTonesHz[c]);
        REQUIRE(std::abs(source[c]) > kAmplitude / 2.0);
    }
    // 5.1 in the encoder's order, L R C LFE Ls Rs, as each tone reaches an
    // output: Tables 217 and 218 with the stream's gains.
    const auto holds = [&](const Played& played, const ac3::render::OutputLayout& layout,
                           Location out, const std::array<double, 6>& weights) {
        for (std::size_t c = 0; c < kTonesHz.size(); ++c) {
            CAPTURE(static_cast<int>(out), c, weights[c]);
            const std::complex<double> got = tone_in(played, layout, out, kTonesHz[c]);
            if (weights[c] == 0.0) {
                CHECK(std::abs(got) < std::abs(source[c]) * 1e-4);
            } else {
                // The gain and its sign: Lt/Rt's surrounds go in out of phase.
                const std::complex<double> ratio = got / source[c];
                CHECK(std::abs(db(std::abs(ratio)) - db(std::abs(weights[c]))) < kToleranceDb);
                CHECK((ratio.real() < 0.0) == (weights[c] < 0.0));
            }
        }
    };
    const double loro_c = from_db(kLoroCentreDb);
    const double loro_s = from_db(kLoroSurroundDb);
    const double ltrt_c = from_db(kLtrtCentreDb);
    const double ltrt_s = from_db(kLtrtSurroundDb);
    const double lfe = from_db(kLfeDb);

    SECTION("Lo/Ro, the LFE in by default") {
        const Played played = play_item(tones(), stereo, as_coded());
        holds(played, stereo, Location::kLeft, {1.0, 0.0, loro_c, lfe, loro_s, 0.0});
        holds(played, stereo, Location::kRight, {0.0, 1.0, loro_c, lfe, 0.0, loro_s});
    }
    SECTION("Lo/Ro with the LFE left out") {
        DecoderSettings settings = as_coded();
        settings.mix_lfe = false;
        const Played played = play_item(tones(), stereo, settings);
        holds(played, stereo, Location::kLeft, {1.0, 0.0, loro_c, 0.0, loro_s, 0.0});
        holds(played, stereo, Location::kRight, {0.0, 1.0, loro_c, 0.0, 0.0, loro_s});
    }
    SECTION("Lt/Rt") {
        DecoderSettings settings = as_coded();
        settings.stereo_fold = ac3::DownmixTarget::kLtRt;
        const Played played = play_item(tones(), stereo, settings);
        holds(played, stereo, Location::kLeft, {1.0, 0.0, ltrt_c, lfe, -ltrt_s, -ltrt_s});
        holds(played, stereo, Location::kRight, {0.0, 1.0, ltrt_c, lfe, ltrt_s, ltrt_s});
    }
    SECTION("the stream's preferred method, which it names Lt/Rt") {
        DecoderSettings settings = as_coded();
        settings.ac4.preferred_downmix = true;
        const Played played = play_item(tones(), stereo, settings);
        holds(played, stereo, Location::kLeft, {1.0, 0.0, ltrt_c, lfe, -ltrt_s, -ltrt_s});
        holds(played, stereo, Location::kRight, {0.0, 1.0, ltrt_c, lfe, ltrt_s, ltrt_s});
    }
    SECTION("mono, L + R of the stream's preferred downmix") {
        const Played played = play_item(tones(), mono, as_coded());
        holds(played, mono, Location::kCentre, {1.0, 1.0, 2.0 * ltrt_c, 2.0 * lfe, 0.0, 0.0});
    }
}

namespace {

// E6's committed broadcast stream (tests/ac4enc/test_ac4enc_presentations.cpp,
// broadcast()): a tone in each substream. Music and effects 5.1 (L 331, R 457,
// C 613, LFE 47, Ls 787, Rs 953 Hz), English dialogue at 1 117 Hz, whose
// dialogue may be raised 6 dB, German at 1 373, and audio description at 1
// 531. Presentation 1 is music and effects with the English, 2 with the
// German, 3 with the English and the audio description.
constexpr double kEnglishHz = 1117.0;
constexpr double kGermanHz = 1373.0;
constexpr double kDescriptionHz = 1531.0;
constexpr double kEnglishCapDb = 6.0;
// Its 20 frames of tones, past the delays and short of the fade they end in.
constexpr std::size_t kBroadcastFrom = 5 * kFrame;
constexpr std::size_t kBroadcastTo = 21 * kFrame;

const std::vector<std::byte>& broadcast() {
    static const std::vector<std::byte> stream =
        read_file(fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "encoder-broadcast.ac4");
    return stream;
}

// The loudest `hz` over every slot.
double loudest(const Played& played, double hz) {
    double most = 0.0;
    for (const std::vector<float>& slot : played.slots) {
        most = std::max(most, std::abs(component(slot, hz, kBroadcastFrom, kBroadcastTo)));
    }
    return most;
}

// The broadcast stream's presentation `id` through the engine.
Played broadcast_presentation(int id, const Ac4Settings& ac4 = {}) {
    DecoderSettings settings = as_coded();
    settings.ac4.presentation_id = id;
    settings.ac4.dialogue_db = ac4.dialogue_db;
    settings.ac4.audio_description = ac4.audio_description;
    settings.ac4.associated_db = ac4.associated_db;
    return play_item(broadcast(), layout_of(kEverySpeaker), settings);
}

}  // namespace

TEST_CASE("hearth ac4: the presentation chosen is the one heard", "[hearth][ac4]") {
    const Played english = broadcast_presentation(1);
    const Played german = broadcast_presentation(2);
    CHECK(loudest(english, kEnglishHz) > 1e-2);
    CHECK(loudest(english, kGermanHz) < loudest(english, kEnglishHz) * 1e-4);
    CHECK(loudest(german, kGermanHz) > 1e-2);
    CHECK(loudest(german, kEnglishHz) < loudest(german, kGermanHz) * 1e-4);
    REQUIRE_FALSE(german.reports.empty());
    REQUIRE(german.reports.back().ac4.has_value());
    CHECK(german.reports.back().ac4->presentation_id == 2);
}

TEST_CASE(
    "hearth ac4: the dialogue level scales the dialogue by g_dialog up to the stream's maximum",
    "[hearth][ac4]") {
    const Played level = broadcast_presentation(1);
    for (const double gain : {-6.0, 3.0, 9.0}) {
        const Played played = broadcast_presentation(1, Ac4Settings{.dialogue_db = gain});
        CAPTURE(gain);
        // Part 1 clause 6.2.16.1: the dialogue's gain, no more than
        // g_dialog_max; the music and effects as they were.
        CHECK(std::abs(db(loudest(played, kEnglishHz) / loudest(level, kEnglishHz)) -
                       std::min(gain, kEnglishCapDb)) < kToleranceDb);
        CHECK(std::abs(db(loudest(played, 331.0) / loudest(level, 331.0))) < kToleranceDb);
    }
}

TEST_CASE("hearth ac4: audio description is mixed in at its level, or not at all",
          "[hearth][ac4]") {
    const Played full =
        broadcast_presentation(3, Ac4Settings{.audio_description = true, .associated_db = 0.0});
    const Played lower =
        broadcast_presentation(3, Ac4Settings{.audio_description = true, .associated_db = -10.0});
    const Played none = broadcast_presentation(3, Ac4Settings{.audio_description = false});
    REQUIRE(loudest(full, kDescriptionHz) > 1e-3);
    // Part 1 clause 6.2.16.2: g_assoc on the associated audio alone.
    CHECK(std::abs(db(loudest(lower, kDescriptionHz) / loudest(full, kDescriptionHz)) + 10.0) <
          kToleranceDb);
    CHECK(std::abs(db(loudest(lower, kEnglishHz) / loudest(full, kEnglishHz))) < kToleranceDb);
    CHECK(loudest(none, kDescriptionHz) < loudest(full, kDescriptionHz) * 1e-4);
}

TEST_CASE("hearth ac4: each DRC decoder mode compresses as the decoder's own does",
          "[hearth][ac4]") {
    // DEE's stream with a compression curve for home theatre and portable
    // headphones, and the default profile for the other two.
    const std::vector<std::byte> bytes = read_file(baseline("ac4-51-drc-ltrt-192"));
    const ac3::render::OutputLayout layout = layout_of(kEverySpeaker);
    std::vector<std::vector<float>> off;
    std::vector<std::vector<float>> home;
    for (const ac4::DrcMode mode :
         {ac4::DrcMode::kOff, ac4::DrcMode::kDefault, ac4::DrcMode::kHomeTheatre,
          ac4::DrcMode::kFlatPanelTv, ac4::DrcMode::kPortableSpeakers,
          ac4::DrcMode::kPortableHeadphones}) {
        CAPTURE(ac4::describe(mode));
        DecoderSettings settings;
        settings.ac4.drc = mode;
        const Played played = play_item(bytes, layout, settings);
        const ac4::DecoderConfig config = ac3::hearth::decoder_setup(settings, layout).ac4;
        CHECK(config.output.output_level_dbfs == -31.0);
        CHECK(config.output.drc == mode);
        CHECK(config.output.headphones == (mode == ac4::DrcMode::kPortableHeadphones));
        CHECK(played.slots == reference(bytes, layout, config));
        REQUIRE_FALSE(played.reports.empty());
        REQUIRE(played.reports.back().ac4.has_value());
        const std::optional<int> applied = played.reports.back().ac4->drc_mode;
        CHECK(applied.has_value() == (mode != ac4::DrcMode::kOff));
        if (mode == ac4::DrcMode::kOff) {
            off = played.slots;
        } else if (mode == ac4::DrcMode::kHomeTheatre) {
            home = played.slots;
            CHECK(applied == 0);
        }
    }
    // The curve acts: the two decodes differ.
    CHECK(off != home);
}

// --- In the player -----------------------------------------------------------------------

TEST_CASE("hearth ac4: the player plays an item as the session and decoder put it out",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    const DecoderSettings settings;
    const Played expected = play_item(tones(), layout, settings);
    auto log = std::make_shared<CaptureSink::Log>();
    Player player(std::make_unique<CaptureSink>(log), loader_of({{"tones", tones()}}), layout,
                  settings);
    player.add(QueueItem{.path = "tones", .title = "Tones", .facts = {}});
    player.play();
    pump_out(player);
    REQUIRE(player.history().size() == 1);
    CHECK(player.history().front().frames == player.history().front().expected_frames);
    CHECK(player.history().front().frames == expected.frames);
    CHECK(log->opens == 1U);
    CHECK(log->slots == expected.slots);
}

TEST_CASE("hearth ac4: a change of settings reaches the playing item at its next frame, in place",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    DecoderSettings before = as_coded();
    const Played unchanged = play_item(tones(), layout, before);
    auto log = std::make_shared<CaptureSink::Log>();
    Player player(std::make_unique<CaptureSink>(log), loader_of({{"tones", tones()}}), layout,
                  before);
    player.add(QueueItem{.path = "tones", .title = "Tones", .facts = {}});
    player.play();
    // A few pumps in, the output level goes to -17 dBFS: 7 dB up from the
    // coded -24.
    for (int i = 0; i < 3; ++i) {
        (void)player.pump(2048);
    }
    const std::size_t at = log->slots.front().size();
    REQUIRE(at > 0);
    REQUIRE(at < kFrom);
    DecoderSettings after = before;
    after.ac4.normalise = true;
    after.ac4.output_level_dbfs = -17.0;
    player.set_decoder_settings(after);
    pump_out(player);
    REQUIRE(player.history().size() == 1);
    // Nothing lost, nothing repeated, and what had been heard was not decoded
    // again.
    CHECK(player.history().front().frames == unchanged.frames);
    REQUIRE(log->slots.front().size() == unchanged.frames);
    for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
        CHECK(std::equal(unchanged.slots[slot].begin(),
                         unchanged.slots[slot].begin() + static_cast<std::ptrdiff_t>(at),
                         log->slots[slot].begin()));
    }
    // Past the frame it applied from, the level the formula gives.
    Played heard;
    heard.slots = log->slots;
    const double expected = db(std::pow(2.0, (-17.0 - kDialnorm) / 6.0));
    const std::complex<double> was =
        tone_in(unchanged, layout, ac3::eac3::chanmap::Location::kLeft, kTonesHz[0]);
    const std::complex<double> is =
        tone_in(heard, layout, ac3::eac3::chanmap::Location::kLeft, kTonesHz[0]);
    CHECK(std::abs(db(std::abs(is) / std::abs(was)) - expected) < kToleranceDb);
}

TEST_CASE("hearth ac4: an AC-4 decoder takes settings in place, and an E-AC-3 one does not",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    StreamDecoder decoder{layout, 48000};
    DecoderSettings changed;
    changed.ac4.dialogue_enhancement_db = 6.0;
    // Nothing decoded yet: in place.
    CHECK(decoder.apply(changed));
    CHECK(decoder.settings() == changed);
    const auto units = ac3::hearth::read_ac4_units(tones());
    REQUIRE(units.has_value());
    const StreamDecoder::BlockFn ignore = [](std::span<const std::span<const float>>, std::size_t) {
    };
    REQUIRE(decoder.decode(units->frames.front(), ignore, {}, units->samples.front()).has_value());
    changed.ac4.output_level_dbfs = -20.0;
    CHECK(decoder.apply(changed));
    // A concealment policy is fixed for an AC-4 decoder.
    changed.concealment = ac3::ConcealmentPolicy::kMute;
    CHECK_FALSE(decoder.apply(changed));

    // E-AC-3 in progress: a new decoder is the way.
    StreamDecoder eac3{layout, 48000};
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::eac3::FrameEncoder encoder{config};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0F);
    const std::vector<std::span<const float>> views(6, silence);
    const auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    REQUIRE(eac3.decode(*frame, ignore).has_value());
    CHECK_FALSE(eac3.apply(changed));
}

TEST_CASE("hearth ac4: a seek starts at an I-frame and plays on as an unbroken decode",
          "[hearth][ac4]") {
    const ac3::render::OutputLayout layout = layout_of("5.1");
    const DecoderSettings settings = as_coded();
    const Played unbroken = play_item(tones(), layout, settings);
    auto session = Session::open("tones", loader_of({{"tones", tones()}}));
    REQUIRE(session.has_value());
    StreamDecoder decoder{layout, session->facts().sample_rate, settings};
    // 0.5 s in, which frame_rate_index 13 puts in the twelfth frame, between
    // I-frames eight apart.
    session->seek(std::chrono::milliseconds{500}, decoder);
    const std::uint64_t from = session->position_samples();
    CHECK(from == 11 * kFrame);
    const Played after = play(*session, decoder);
    CHECK(after.errors.empty());
    REQUIRE(after.frames == unbroken.frames - from);
    for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
        CAPTURE(slot);
        double worst = 0.0;
        for (std::size_t n = 0; n < after.slots[slot].size(); ++n) {
            worst = std::max(worst, static_cast<double>(std::abs(after.slots[slot][n] -
                                                                 unbroken.slots[slot][from + n])));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("hearth ac4: an AC-4 item joins an E-AC-3 one in the same output", "[hearth][ac4]") {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> eac3;
    for (int f = 0; f < 12; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(
                                   n + (static_cast<std::size_t>(f) * ac3::kSamplesPerFrame)) /
                               kRate));
        }
        const std::vector<std::span<const float>> views(6, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        eac3.insert(eac3.end(), frame->begin(), frame->end());
    }
    const ac3::render::OutputLayout layout = layout_of("5.1");
    auto log = std::make_shared<CaptureSink::Log>();
    Player player(std::make_unique<CaptureSink>(log), loader_of({{"eac3", eac3}, {"ac4", tones()}}),
                  layout);
    player.add(QueueItem{.path = "eac3", .title = "E-AC-3", .facts = {}});
    player.add(QueueItem{.path = "ac4", .title = "AC-4", .facts = {}});
    player.play();
    pump_out(player);
    REQUIRE(player.history().size() == 2);
    CHECK(log->opens == 1U);
    for (const ac3::hearth::PlayedItem& item : player.history()) {
        CAPTURE(item.title);
        CHECK(item.frames == item.expected_frames);
        CHECK(item.output_opens == 1U);
    }
    CHECK(player.history()[1].first_frame == player.history()[0].frames);
}
