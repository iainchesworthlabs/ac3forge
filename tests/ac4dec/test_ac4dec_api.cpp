// The AC-4 decoder's API in its final form (planning/ac4.md, phase D8),
// through its public headers alone, so that a shared build checks it against
// the exported symbols: the configuration's syntax trace kept alive by the
// decoder, set_output() and set_presentation() on a decoder that is playing,
// decode_by_block()'s blocks, presentations() with their names, metadata()
// against what the frames sent, latency_samples() against the encoder's
// figure for the same frame rate, and an engine in the shape of Hearth's
// (apps/hearth/engine/stream_decoder.hpp) standing in for it, which decodes
// every committed AC-4 stream by block.
//
// AC4DEC_API_STREAM_DIR, when set, names a directory whose .ac4 files the
// engine decodes as well, the local gold set or the third-party streams; a
// stream in syntax this version refuses by name is counted, not failed.
//
// Under the sanitizers (tests/sanitized.hpp) the engine plays one committed
// stream of each kind, and of a long stream its first 5 frames
// (tests/ac4_stream_kinds.hpp, which tests/hearth/test_ac4_engine.cpp shares).
// Each stream is decoded twice, and DEE's 120-frame legs took most of the ASan
// leg's time for this test. A kind is a frame rate, the channels and substream
// roles of the presentation decode() plays, how the stream codes its objects,
// and the codec modes of its first frame: every layout, codec mode, object
// coding and frame rate the streams show is still played. A normal build plays
// every stream to its end.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4/syntax.hpp"
#include "ac4_stream_kinds.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "ac4dec_bits.hpp"
#include "iclforge/ac4enc/encoder.hpp"
#include "sanitized.hpp"

namespace {

namespace fs = std::filesystem;
using iclforge::test::kSanitized;
using iclforge::test::kSanitizedFrames;
using iclforge::test::streams_to_play;
using ac4dec_test::BitWriter;

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// Each sync frame's raw_ac4_frame, copied, so that the frames outlive the
// file's bytes.
std::vector<std::vector<std::byte>> frames_of(const fs::path& path) {
    const std::vector<std::byte> bytes = read_file(path);
    std::vector<std::vector<std::byte>> frames;
    for (const iclforge::ac4::SyncFrame& frame : iclforge::ac4::scan(bytes).frames) {
        frames.emplace_back(frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    return frames;
}

fs::path baseline(std::string_view leg) {
    return fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
}

// Every committed AC-4 stream: DEE's, the constructed ones and the
// multiplexed presentations with their sources.
std::vector<fs::path> committed_streams() {
    std::vector<fs::path> paths;
    for (const fs::path& root :
         {fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR}, fs::path{AC4DEC_GOLDEN_DIR}}) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ac4") {
                paths.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(paths);
    return paths;
}

// decode() over every frame: the channels that came out, appended.
struct Decoded {
    std::vector<iclforge::ac4::Speaker> speakers;
    std::vector<std::vector<float>> channels;
    std::size_t frames = 0;
    std::size_t waited = 0;  // frames that came out with nothing
};

Decoded decode_all(iclforge::ac4::Decoder& decoder,
                   const std::vector<std::vector<std::byte>>& frames) {
    Decoded out;
    for (const std::vector<std::byte>& frame : frames) {
        const auto decoded = decoder.decode(frame);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            ++out.waited;
            continue;
        }
        const iclforge::ac4::DecodedFrame& f = **decoded;
        if (out.channels.empty()) {
            out.speakers = f.speakers;
            out.channels.resize(f.channels.size());
        }
        REQUIRE(f.speakers == out.speakers);
        REQUIRE(f.samples == f.channels.front().size());
        for (std::size_t c = 0; c < f.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), f.channels[c].begin(),
                                   f.channels[c].end());
        }
        ++out.frames;
    }
    return out;
}

// A syntax callable that counts its live copies.
struct CountingSink {
    int* alive;
    std::vector<iclforge::ac4::SyntaxRecord>* records;
    CountingSink(int* a, std::vector<iclforge::ac4::SyntaxRecord>* r) : alive(a), records(r) {
        ++*alive;
    }
    CountingSink(const CountingSink& other) : alive(other.alive), records(other.records) {
        ++*alive;
    }
    CountingSink& operator=(const CountingSink&) = delete;
    ~CountingSink() { --*alive; }
    void operator()(const iclforge::ac4::SyntaxRecord& record) const { records->push_back(record); }
};

struct RecordFunctor {
    void operator()(const iclforge::ac4::SyntaxRecord& /*record*/) const {}
};

// The non-owning reference the readers hold refuses a temporary at compile
// time; a named callable binds.
static_assert(!std::is_constructible_v<iclforge::ac4::SyntaxSink, RecordFunctor>);
static_assert(!std::is_constructible_v<iclforge::ac4::SyntaxSink, RecordFunctor&&>);
static_assert(std::is_constructible_v<iclforge::ac4::SyntaxSink, RecordFunctor&>);
static_assert(std::is_constructible_v<iclforge::ac4::SyntaxTrace, RecordFunctor>);

}  // namespace

// --- The syntax trace's lifetime ---------------------------------------------

TEST_CASE("a decoder keeps its own copy of the syntax callable it is configured with",
          "[ac4dec][api]") {
    // The review of #700 and D7: DecoderConfig::syntax held only its
    // callable's address, so one written in place was gone by the next
    // statement, which one platform's heap hid and MSVC's Release build did
    // not. The configuration and the decoder now each keep a copy.
    int alive = 0;
    std::vector<iclforge::ac4::SyntaxRecord> records;
    std::optional<iclforge::ac4::Decoder> decoder;
    {
        iclforge::ac4::DecoderConfig config;
        config.syntax = CountingSink{&alive, &records};
        REQUIRE(alive >= 1);
        decoder.emplace(config);
    }
    REQUIRE(alive >= 1);
    const auto frames = frames_of(baseline("ac4-stereo-64"));
    REQUIRE(decoder->parse(frames.front()).has_value());
    CHECK_FALSE(records.empty());
    decoder.reset();
    CHECK(alive == 0);

    // A lambda written in place, D7's form, now traces.
    std::vector<iclforge::ac4::SyntaxRecord> traced;
    iclforge::ac4::DecoderConfig config;
    config.syntax = [&traced](const iclforge::ac4::SyntaxRecord& record) {
        traced.push_back(record);
    };
    iclforge::ac4::Decoder in_place(config);
    config = {};
    REQUIRE(in_place.parse(frames.front()).has_value());
    CHECK(traced.size() == records.size());
}

TEST_CASE("an encoder keeps its own copy of the syntax callable it is configured with",
          "[ac4enc][api]") {
    int alive = 0;
    std::vector<iclforge::ac4::SyntaxRecord> records;
    std::optional<iclforge::ac4::Encoder> encoder;
    {
        iclforge::ac4::EncoderConfig config;
        config.trace = CountingSink{&alive, &records};
        REQUIRE(alive >= 1);
        auto created = iclforge::ac4::Encoder::create(config);
        REQUIRE(created.has_value());
        encoder.emplace(std::move(*created));
    }
    REQUIRE(alive >= 1);
    const std::vector<float> silence(8192, 0.0F);
    const std::array<std::span<const float>, 2> channels = {silence, silence};
    const auto frames = encoder->encode(channels);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    CHECK_FALSE(records.empty());
    encoder.reset();
    CHECK(alive == 0);
}

// --- Changing the output and the presentation while a stream plays ------------

TEST_CASE("set_output changes the output level from the next frame without waiting for an I-frame",
          "[ac4dec][api]") {
    const auto frames = frames_of(baseline("ac4-20-music-192"));
    REQUIRE(frames.size() > 40);
    const iclforge::ac4::OutputConfig before{};
    const iclforge::ac4::OutputConfig after{.output_level_dbfs = -20.0,
                                            .drc = iclforge::ac4::DrcMode::kOff};

    iclforge::ac4::Decoder first(iclforge::ac4::DecoderConfig{.syntax = {}, .output = before});
    iclforge::ac4::Decoder second(iclforge::ac4::DecoderConfig{.syntax = {}, .output = after});
    const Decoded as_before = decode_all(first, frames);
    const Decoded as_after = decode_all(second, frames);

    // Changed at frame 20, which is not an I-frame: no frame is lost, the
    // output up to it is the first decode's, and from two frames after it the
    // second's, the control data having reached the QMF domain.
    iclforge::ac4::Decoder playing(iclforge::ac4::DecoderConfig{.syntax = {}, .output = before});
    constexpr std::size_t kChange = 20;
    std::vector<std::vector<float>> out(2);
    std::size_t lengths_before = 0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f == kChange) {
            playing.set_output(after);
            lengths_before = out[0].size();
        }
        const auto decoded = playing.decode(frames[f]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        for (std::size_t c = 0; c < 2; ++c) {
            out[c].insert(out[c].end(), (**decoded).channels[c].begin(),
                          (**decoded).channels[c].end());
        }
    }
    CHECK(playing.output().output_level_dbfs == after.output_level_dbfs);
    REQUIRE(out[0].size() == as_before.channels[0].size());
    const std::size_t frame_length = 2048;
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t n = 0; n < lengths_before; ++n) {
            REQUIRE(out[c][n] == as_before.channels[c][n]);
        }
        for (std::size_t n = lengths_before + 2 * frame_length; n < out[c].size(); ++n) {
            REQUIRE(out[c][n] == as_after.channels[c][n]);
        }
    }
    // A decoder built afresh at the change, as a player that rebuilds its
    // decoder would, puts out nothing until the stream's next I-frame.
    iclforge::ac4::Decoder rebuilt(iclforge::ac4::DecoderConfig{.syntax = {}, .output = after});
    std::size_t silent = 0;
    for (std::size_t f = kChange; f < frames.size(); ++f) {
        const auto decoded = rebuilt.decode(frames[f]);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            break;
        }
        ++silent;
    }
    INFO("frames a rebuilt decoder waits for an I-frame");
    CHECK(silent > 0);
}

namespace {

// 5.1 at 384 kbps, a tone a channel at -20 dBFS (L 440, R 620, C 800, LFE 90,
// Ls 1030, Rs 1270 Hz) with gains of its own for every downmix, which the
// encoder sends in I-frames alone, every 24 frames: Hearth's tones stream
// (apps/hearth/ui/tests/test_room.cpp).
std::vector<std::vector<std::byte>> tones_with_their_own_gains(std::size_t frames) {
    iclforge::ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    config.downmix = iclforge::ac4::DownmixConfig{.loro_centre_db = -1.5,
                                                  .loro_surround_db = -4.5,
                                                  .ltrt_centre_db = -3.0,
                                                  .ltrt_surround_db = -6.0,
                                                  .lfe_db = -4.5,
                                                  .preferred = iclforge::ac4::PreferredDownmix::kLtRt,
                                                  .loro_correction_db2 = std::nullopt,
                                                  .ltrt_correction_db2 = std::nullopt};
    REQUIRE(config.iframe_interval == 24);
    std::vector<std::vector<float>> input;
    for (const double hz : {440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0}) {
        std::vector<float>& channel = input.emplace_back(frames * 2048);
        for (std::size_t n = 0; n < channel.size(); ++n) {
            channel[n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
        }
    }
    auto encoder = iclforge::ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    auto encoded = encoder->encode(views);
    REQUIRE(encoded.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    std::vector<std::vector<std::byte>> out;
    for (const auto* part : {&*encoded, &*rest}) {
        for (const iclforge::ac4::EncodedFrame& frame : *part) {
            out.push_back(frame.raw_ac4_frame);
        }
    }
    return out;
}

// The first sample in [from, to) at which a and b differ.
std::optional<std::size_t> first_difference(const std::vector<float>& a, const std::vector<float>& b,
                                            std::size_t from, std::size_t to) {
    for (std::size_t n = from; n < to && n < a.size() && n < b.size(); ++n) {
        if (a[n] != b[n]) {
            return n;
        }
    }
    return std::nullopt;
}

// Plays `frames` with `before` and changes to `after` at frame `change`: what
// comes out is a decode with `before` throughout up to the change, and from
// two frames after it, the control data having reached the QMF domain, a
// decode with `after` throughout.
void require_change_lands_whole(const std::vector<std::vector<std::byte>>& frames, std::size_t change,
                                const iclforge::ac4::OutputConfig& before,
                                const iclforge::ac4::OutputConfig& after) {
    iclforge::ac4::Decoder first(iclforge::ac4::DecoderConfig{.syntax = {}, .output = before});
    iclforge::ac4::Decoder second(iclforge::ac4::DecoderConfig{.syntax = {}, .output = after});
    const Decoded as_before = decode_all(first, frames);
    const Decoded as_after = decode_all(second, frames);
    REQUIRE(as_before.waited == 0);
    REQUIRE(as_after.speakers == as_before.speakers);

    iclforge::ac4::Decoder playing(iclforge::ac4::DecoderConfig{.syntax = {}, .output = before});
    std::vector<std::vector<float>> out(as_before.channels.size());
    std::size_t at_change = 0;
    std::size_t frame_length = 0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f == change) {
            playing.set_output(after);
            at_change = out[0].size();
        }
        const auto decoded = playing.decode(frames[f]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        REQUIRE(pcm.channels.size() == out.size());
        frame_length = std::max(frame_length, pcm.samples);
        for (std::size_t c = 0; c < out.size(); ++c) {
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    REQUIRE(at_change > 0);
    REQUIRE(out[0].size() == as_after.channels[0].size());
    const auto shown = [](const std::optional<std::size_t>& at) {
        return at ? std::to_string(*at) : std::string{"none"};
    };
    for (std::size_t c = 0; c < out.size(); ++c) {
        const std::optional<std::size_t> early =
            first_difference(out[c], as_before.channels[c], 0, at_change);
        const std::optional<std::size_t> late = first_difference(
            out[c], as_after.channels[c], at_change + (2 * frame_length), out[c].size());
        INFO("channel " << c << ", " << frame_length << " samples a frame, changed at sample "
                        << at_change << "; first sample unlike the old output before it "
                        << shown(early) << ", unlike the new output after it " << shown(late));
        CHECK_FALSE(early.has_value());
        CHECK_FALSE(late.has_value());
    }
}

}  // namespace

// The gains persist from the frame that sends them (6.2.17.0), and a stream
// may send them only in its I-frames: dropped at a new fold or LFE choice,
// the downmix would take a stream without gains' -3 dB, and no LFE, for up to
// a second (Hearth's DecoderAc4::test_downmixIsHeardWithTheStreamsGains).
TEST_CASE("set_output changes the downmix from the next frame, keeping the stream's own gains",
          "[ac4dec][api]") {
    const auto frames = tones_with_their_own_gains(60);
    REQUIRE(frames.size() >= 60);
    // Frame 30 is between the I-frames 24 and 48, the only frames that carry
    // the gains.
    constexpr std::size_t kChange = 30;
    SECTION("Lo/Ro to Lt/Rt") {
        require_change_lands_whole(frames, kChange,
                                   {.downmix = iclforge::ac4::DownmixTarget::kLoRo},
                                   {.downmix = iclforge::ac4::DownmixTarget::kLtRt});
    }
    SECTION("the LFE back into Lt/Rt") {
        require_change_lands_whole(
            frames, kChange, {.downmix = iclforge::ac4::DownmixTarget::kLtRt, .mix_lfe = false},
            {.downmix = iclforge::ac4::DownmixTarget::kLtRt, .mix_lfe = true});
    }
}

TEST_CASE("set_output changes the downmix without starting the DRC again", "[ac4dec][api]") {
    auto frames = frames_of(baseline("ac4-51-drc-ltrt-192"));
    REQUIRE(frames.size() > 60);
    frames.resize(60);
    const iclforge::ac4::OutputConfig loro{.output_level_dbfs = -20.0,
                                           .downmix = iclforge::ac4::DownmixTarget::kLoRo};
    iclforge::ac4::OutputConfig ltrt = loro;
    ltrt.downmix = iclforge::ac4::DownmixTarget::kLtRt;
    // The DRC compresses this stream at -20 dBFS, or the change has nothing
    // of it to keep.
    iclforge::ac4::OutputConfig uncompressed = loro;
    uncompressed.drc = iclforge::ac4::DrcMode::kOff;
    iclforge::ac4::Decoder compressing(iclforge::ac4::DecoderConfig{.syntax = {}, .output = loro});
    iclforge::ac4::Decoder flat(iclforge::ac4::DecoderConfig{.syntax = {}, .output = uncompressed});
    REQUIRE(decode_all(compressing, frames).channels != decode_all(flat, frames).channels);
    require_change_lands_whole(frames, 30, loro, ltrt);
}

TEST_CASE("set_presentation switches presentations from the next frame", "[ac4dec][api]") {
    const auto frames =
        frames_of(fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4");
    REQUIRE(frames.size() >= 12);
    iclforge::ac4::Decoder decoder;
    iclforge::ac4::PresentationChoice second;
    second.index = 1;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f == 6) {
            decoder.set_presentation(second);
        }
        const auto decoded = decoder.decode(frames[f]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK((**decoded).presentation == (f < 6 ? 0U : 1U));
        CHECK(decoder.metadata().presentation == (f < 6 ? 0U : 1U));
    }
    // The presentation decoded afterwards is the one a decoder configured
    // with it from the start decodes, once its substreams' signal has run
    // through the transforms and the QMF banks.
    iclforge::ac4::Decoder from_start(iclforge::ac4::DecoderConfig{
        .syntax = {}, .output = {}, .concealment = {}, .presentation = second});
    iclforge::ac4::Decoder switched;
    std::vector<float> a;
    std::vector<float> b;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f == 6) {
            switched.set_presentation(second);
        }
        const auto x = from_start.decode(frames[f]);
        const auto y = switched.decode(frames[f]);
        REQUIRE((x.has_value() && x->has_value() && y.has_value() && y->has_value()));
        if (f >= 9) {
            a.insert(a.end(), (**x).channels[0].begin(), (**x).channels[0].end());
            b.insert(b.end(), (**y).channels[0].begin(), (**y).channels[0].end());
        }
    }
    REQUIRE(a.size() == b.size());
    double peak = 0.0;
    double difference = 0.0;
    for (std::size_t n = 0; n < a.size(); ++n) {
        peak = std::max(peak, static_cast<double>(std::abs(a[n])));
        difference = std::max(difference, static_cast<double>(std::abs(a[n] - b[n])));
    }
    REQUIRE(peak > 0.0);
    CHECK(difference <= peak * 1e-4);
}

// --- Decoding by block ---------------------------------------------------------

TEST_CASE(
    "decode_by_block hands the output over in blocks of 256 samples whatever the frame length",
    "[ac4dec][api]") {
    // Index 13's 2 048-sample frames, and 24 fps IMS, whose frames come to 2 000
    // samples at 48 kHz: seven blocks and 208 held for the next.
    for (const std::string_view leg :
         {"ac4-20-tones-192", "ac4-ims-film-96-24", "ac4-ims-music-64-2997"}) {
        CAPTURE(leg);
        const auto frames = frames_of(baseline(leg));
        iclforge::ac4::Decoder whole;
        const Decoded reference = decode_all(whole, frames);

        iclforge::ac4::Decoder by_block;
        std::vector<std::vector<float>> out;
        std::uint64_t next = 0;
        std::size_t short_blocks = 0;
        const auto sink = [&](const iclforge::ac4::PcmBlock& block) {
            REQUIRE(block.position == next);
            REQUIRE(block.channels.size() == block.speakers.size());
            if (block.samples != iclforge::ac4::kBlockSamples) {
                ++short_blocks;
            }
            if (out.empty()) {
                out.resize(block.channels.size());
            }
            for (std::size_t c = 0; c < block.channels.size(); ++c) {
                REQUIRE(block.channels[c].size() == block.samples);
                out[c].insert(out[c].end(), block.channels[c].begin(), block.channels[c].end());
            }
            next += block.samples;
        };
        std::size_t total = 0;
        for (const std::vector<std::byte>& frame : frames) {
            const auto info = by_block.decode_by_block(frame, sink);
            REQUIRE(info.has_value());
            REQUIRE(info->has_value());
            total += (**info).samples;
            CHECK((**info).speakers.size() == reference.speakers.size());
        }
        CHECK(short_blocks == 0);
        const std::size_t held = by_block.flush(sink);
        CHECK(held < iclforge::ac4::kBlockSamples);
        CHECK(held == total % iclforge::ac4::kBlockSamples);
        CHECK(by_block.flush(sink) == 0);
        CHECK(short_blocks == (held > 0 ? 1U : 0U));
        REQUIRE(out.size() == reference.channels.size());
        for (std::size_t c = 0; c < out.size(); ++c) {
            REQUIRE(out[c] == reference.channels[c]);
        }
    }
}

TEST_CASE("decode_by_block hands over what it holds before a change of layout", "[ac4dec][api]") {
    const auto frames = frames_of(baseline("ac4-51-music-384"));
    iclforge::ac4::Decoder decoder;
    std::vector<std::size_t> widths;
    std::vector<std::size_t> lengths;
    const auto sink = [&](const iclforge::ac4::PcmBlock& block) {
        widths.push_back(block.channels.size());
        lengths.push_back(block.samples);
    };
    for (std::size_t f = 0; f < 8; ++f) {
        if (f == 4) {
            decoder.set_output(
                iclforge::ac4::OutputConfig{.downmix = iclforge::ac4::DownmixTarget::kLoRo});
        }
        REQUIRE(decoder.decode_by_block(frames[f], sink).has_value());
    }
    // 2 048-sample frames leave nothing held at index 13, so every block is
    // whole: 6 channels, then 2.
    REQUIRE(widths.size() == 64);
    CHECK(std::ranges::all_of(lengths,
                              [](std::size_t n) { return n == iclforge::ac4::kBlockSamples; }));
    CHECK(std::count(widths.begin(), widths.end(), 6U) == 32);
    CHECK(std::count(widths.begin(), widths.end(), 2U) == 32);

    // At 29.97 fps a frame leaves samples over, and the change hands them over
    // first as a shorter block of the old layout.
    const auto ims = frames_of(baseline("ac4-ims-music-64-2997"));
    iclforge::ac4::Decoder held;
    widths.clear();
    lengths.clear();
    for (std::size_t f = 0; f < 6; ++f) {
        if (f == 3) {
            held.set_output(
                iclforge::ac4::OutputConfig{.downmix = iclforge::ac4::DownmixTarget::kMono});
        }
        REQUIRE(held.decode_by_block(ims[f], sink).has_value());
    }
    const auto change = std::ranges::find(widths, 1U);
    REQUIRE(change != widths.end());
    const auto at = static_cast<std::size_t>(change - widths.begin());
    REQUIRE(at > 0);
    CHECK(widths[at - 1] == 2U);
    CHECK(lengths[at - 1] < iclforge::ac4::kBlockSamples);
}

// --- What the decoder reports ----------------------------------------------------

namespace {

// One frame of one alternative stereo presentation, whose presentation
// substream sends `name` as presentation_name (name_len bytes) or no name.
// Its audio substream holds no audio: the presentation substream is what the
// test reads.
std::vector<std::byte> named_frame(int counter,
                                   const std::optional<std::vector<std::uint8_t>>& name) {
    BitWriter toc;
    toc.put(2, 2);                                            // bitstream_version
    toc.put(static_cast<std::uint64_t>(counter), 10);         // sequence_counter
    toc.flag(false);                                          // b_wait_frames
    toc.put(1, 1);                                            // fs_index: 48 kHz
    toc.put(13, 4);                                           // frame_rate_index
    toc.flag(true);                                           // b_iframe_global
    toc.flag(true);                                           // b_single_presentation
    toc.flag(false);                                          // b_payload_base
    toc.flag(false);                                          // b_program_id
    toc.flag(true);                                           // b_single_substream_group
    toc.flag(false);                                          // presentation_version 0
    toc.put(0, 3);                                            // md_compat
    toc.flag(false);                                          // b_presentation_id
    toc.put(0, 2).put(0, 3).flag(false).put(0, 2).put(0, 2);  // emdf_info()
    toc.flag(false);                                          // b_presentation_filter
    toc.put(0, 3);                                            // ac4_sgi_specifier(): group 0
    toc.flag(false);                                          // b_pre_virtualized
    toc.flag(false);                                          // b_add_emdf_substreams
    toc.flag(true);                                           // b_alternative
    toc.flag(true);                                           // b_pres_ndot
    toc.put(1, 2);                                            // presentation substream_index 1
    // Group 0: one channel-coded stereo substream.
    toc.flag(true);    // b_substreams_present
    toc.flag(false);   // b_hsf_ext
    toc.flag(true);    // b_single_substream
    toc.flag(true);    // b_channel_coded
    toc.put(0b10, 2);  // channel_mode: stereo
    toc.flag(false);   // b_sf_multiplier
    toc.flag(false);   // b_bitrate_info
    toc.flag(true);    // b_audio_ndot
    toc.put(0, 2);     // substream_index 0
    toc.flag(false);   // b_content_type

    BitWriter p;
    p.flag(name.has_value());  // b_name_present
    if (name) {
        // b_length, and name_len unless the name is the 32 bytes b_length 0 says.
        REQUIRE(name->size() <= 32);
        p.flag(name->size() != 32);
        if (name->size() != 32) {
            p.put(name->size(), 5);
        }
        for (const std::uint8_t b : *name) {
            p.put(b, 8);
        }
    }
    p.put(0, 2);                                                // n_targets_minus1
    p.put(0, 3).put(0, 4).flag(false).flag(false).flag(false);  // one target
    p.flag(false);                                              // b_active, one substream
    p.flag(false);                                              // b_additional_data
    p.put(124, 7);                                              // dialnorm_bits: -31 dBFS
    p.flag(false);                                              // b_further_loudness_info
    p.put(1, 5).flag(false);                                    // drc_metadata_size: 1
    p.flag(false);                                              // drc_frame(): b_drc_present
    p.flag(false);                                              // b_associated
    p.align();
    const std::vector<std::byte> presentation = p.bytes();
    const std::vector<std::byte> audio(4, std::byte{0});

    toc.put(2, 2);  // substream_index_table(): two substreams
    for (const std::size_t size : {audio.size(), presentation.size()}) {
        toc.flag(false);
        toc.put(size, 10);
    }
    toc.align();
    std::vector<std::byte> frame = toc.bytes();
    frame.insert(frame.end(), audio.begin(), audio.end());
    frame.insert(frame.end(), presentation.begin(), presentation.end());
    return frame;
}

std::vector<std::uint8_t> hex_bytes(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

}  // namespace

TEST_CASE("presentations() names an alternative presentation as its chunks arrive",
          "[ac4dec][api]") {
    // tests/golden/ac4dec/presentations/presentation-names.tsv: each case a sequence of
    // frames' presentation_name bytes, or "-" for a frame without one, and the
    // name after the last. tools/checks/test_ac4_presentation_names.py holds
    // the Python reference to the same table (src/ac4dec/ERRATA.md, "A
    // presentation name in chunks").
    std::ifstream in(fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentation-names.tsv");
    REQUIRE(in.good());
    std::string line;
    int cases = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream split(line);
        for (std::string field; std::getline(split, field, '\t');) {
            fields.push_back(field);
        }
        REQUIRE(fields.size() >= 2);
        const std::string expected = fields.size() > 2 ? fields[2] : std::string{};
        INFO("case " << fields[0]);
        iclforge::ac4::Decoder decoder;
        std::istringstream chunks(fields[1]);
        int counter = 1;
        for (std::string chunk; chunks >> chunk;) {
            const auto name = chunk == "-"
                                  ? std::optional<std::vector<std::uint8_t>>{}
                                  : std::optional<std::vector<std::uint8_t>>{hex_bytes(chunk)};
            REQUIRE(decoder.parse(named_frame(counter++, name)).has_value());
        }
        REQUIRE(decoder.presentations().size() == 1);
        const iclforge::ac4::PresentationInfo& info = decoder.presentations().front();
        CHECK(info.alternative);
        CHECK(info.name == expected);
        CHECK(decoder.metadata().loudness.dialnorm_dbfs == -31.0);
        ++cases;
    }
    CHECK(cases >= 10);
}

TEST_CASE("describe names every substream role", "[ac4dec][api]") {
    std::set<std::string_view> seen;
    for (const iclforge::ac4::SubstreamRole role :
         {iclforge::ac4::SubstreamRole::kMain, iclforge::ac4::SubstreamRole::kMusicAndEffects,
          iclforge::ac4::SubstreamRole::kDialogue,
          iclforge::ac4::SubstreamRole::kDialogueEnhancement,
          iclforge::ac4::SubstreamRole::kAssociated}) {
        const std::string_view text = iclforge::ac4::describe(role);
        CHECK_FALSE(text.empty());
        seen.insert(text);
    }
    CHECK(seen.size() == 5);
    CHECK(iclforge::ac4::describe(static_cast<iclforge::ac4::SubstreamRole>(99)) == "?");
}

TEST_CASE("presentations() lists each presentation of the table of contents", "[ac4dec][api]") {
    SECTION("a DEE stream's one presentation") {
        const auto frames = frames_of(baseline("ac4-51-music-192"));
        iclforge::ac4::Decoder decoder;
        CHECK(decoder.presentations().empty());
        REQUIRE(decoder.parse(frames.front()).has_value());
        REQUIRE(decoder.presentations().size() == 1);
        const iclforge::ac4::PresentationInfo& p = decoder.presentations().front();
        CHECK(p.index == 0);
        CHECK(p.decodable);
        CHECK(p.selectable);
        CHECK_FALSE(p.alternative);
        CHECK(p.name.empty());
        CHECK(p.speakers == std::vector<iclforge::ac4::Speaker>{
                                iclforge::ac4::Speaker::kLeft, iclforge::ac4::Speaker::kRight,
                                iclforge::ac4::Speaker::kCentre, iclforge::ac4::Speaker::kLfe,
                                iclforge::ac4::Speaker::kLeftSurround,
                                iclforge::ac4::Speaker::kRightSurround});
        REQUIRE(p.members.size() == 1);
        CHECK(p.members.front().role == iclforge::ac4::SubstreamRole::kMain);
        CHECK(p.members.front().speakers == p.speakers);
        CHECK(p.substream_groups == std::vector<int>{0});
    }
    SECTION("the multiplexer's presentations of several substreams") {
        const auto frames =
            frames_of(fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4");
        iclforge::ac4::Decoder decoder;
        REQUIRE(decoder.parse(frames.front()).has_value());
        const std::span<const iclforge::ac4::PresentationInfo> all = decoder.presentations();
        REQUIRE(all.size() == 17);
        for (std::size_t i = 0; i < all.size(); ++i) {
            CHECK(all[i].index == i);
            CHECK(all[i].decodable);
        }
        // Each presentation's language is its dialogue's, else its main
        // audio's, as selection compares it; each is what select_presentation()
        // finds for that language.
        std::map<std::string, int> languages;
        for (const iclforge::ac4::PresentationInfo& p : all) {
            ++languages[p.language];
            for (const iclforge::ac4::PresentationMember& m : p.members) {
                CHECK_FALSE(m.speakers.empty());
            }
        }
        CHECK(languages.contains("en"));
        CHECK(languages.contains("de"));
        const bool has_associated =
            std::ranges::any_of(all, [](const iclforge::ac4::PresentationInfo& p) {
                return std::ranges::any_of(
                    p.members, [](const iclforge::ac4::PresentationMember& m) {
                        return m.role == iclforge::ac4::SubstreamRole::kAssociated;
                    });
            });
        CHECK(has_associated);
    }
}

TEST_CASE("metadata() reports the values the selected presentation's frames sent",
          "[ac4dec][api]") {
    // Each DEE leg's frames traced: the last value of each element the trace
    // shows, which is what metadata() holds after the last frame.
    for (const std::string_view leg : {"ac4-20-music-192", "ac4-20-speech-128", "ac4-51-music-128",
                                       "ac4-51-drc-ltrt-192", "ac4-51-film-96"}) {
        CAPTURE(leg);
        std::map<std::string, std::uint64_t> last;
        std::map<std::string, int> seen;
        iclforge::ac4::DecoderConfig config;
        config.output.output_level_dbfs = -20.0;
        config.syntax = [&](const iclforge::ac4::SyntaxRecord& r) {
            last[std::string{r.name}] = r.value;
            ++seen[std::string{r.name}];
        };
        iclforge::ac4::Decoder decoder(config);
        for (const std::vector<std::byte>& frame : frames_of(baseline(leg))) {
            REQUIRE(decoder.decode(frame).has_value());
        }
        const iclforge::ac4::PresentationMetadata& m = decoder.metadata();
        REQUIRE(m.presentation == 0U);
        REQUIRE(seen.contains("dialnorm_bits"));
        CHECK(m.loudness.dialnorm_dbfs == -0.25 * static_cast<double>(last["dialnorm_bits"]));
        if (seen.contains("loudrelgat")) {
            CHECK(m.loudness.integrated_lkfs ==
                  (static_cast<double>(last["loudrelgat"]) - 1024.0) / 10.0);
        }
        if (seen.contains("max_truepk")) {
            CHECK(m.loudness.max_true_peak_dbtp ==
                  (static_cast<double>(last["max_truepk"]) - 1024.0) / 10.0);
        }
        if (seen.contains("drc_eac3_profile")) {
            REQUIRE(m.drc.has_value());
            CHECK(m.drc->eac3_profile == static_cast<int>(last["drc_eac3_profile"]));
            CHECK(m.drc->modes.size() == last["drc_decoder_nr_modes"] + 1);
            // At -20 dBFS the flat panel TV mode of Table 161 compresses.
            CHECK(m.drc->applied_mode == 1);
        }
        if (seen.contains("de_method")) {
            REQUIRE(m.dialogue_enhancement.has_value());
            CHECK(m.dialogue_enhancement->method == static_cast<int>(last["de_method"]));
            CHECK(m.dialogue_enhancement->max_gain_db ==
                  3.0 * static_cast<double>(last["de_max_gain"] + 1));
        }
        if (seen.contains("loro_centre_mixgain")) {
            REQUIRE(m.downmix.has_value());
            constexpr std::array<double, 7> kCentre = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
            const std::uint64_t code = last["loro_centre_mixgain"];
            if (code < 7) {
                CHECK(std::abs(m.downmix->loro_centre_db - kCentre[code]) < 1e-9);
            }
            CHECK(static_cast<int>(m.downmix->preferred) ==
                  static_cast<int>(last["preferred_dmx_method"]));
        }
    }
}

TEST_CASE("latency_samples() is the decoder delay the encoder counts on at every frame rate",
          "[ac4dec][api]") {
    // Encoder::decoder_delay_samples() gives the delay a decoder adds at each
    // rate from the encoder's own timing; the decoder measures its own.
    const std::vector<float> silence(12000, 0.0F);
    const std::array<std::span<const float>, 2> channels = {silence, silence};
    iclforge::ac4::Decoder never;
    CHECK(never.latency_samples() == 0);
    for (int index = 0; index <= 13; ++index) {
        CAPTURE(index);
        iclforge::ac4::EncoderConfig config;
        config.frame_rate_index = index;
        config.iframe_interval = 1;
        auto encoder = iclforge::ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        const auto frames = encoder->encode(channels);
        REQUIRE(frames.has_value());
        REQUIRE_FALSE(frames->empty());
        iclforge::ac4::Decoder decoder;
        const auto decoded = decoder.decode(frames->front().raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK(decoder.latency_samples() == encoder->decoder_delay_samples());
        if (index == 13) {
            CHECK(decoder.latency_samples() == 1313);
        }
    }
}

// --- An engine in the shape of Hearth's ---------------------------------------------

namespace {

// Hearth's StreamDecoder takes an access unit and hands over blocks of 256
// samples rendered onto its output layout; its AC-4 adapter (plan phase I2)
// has only the public API. This one renders each block onto a fixed layout by
// speaker, as a layout renderer places a bed, and changes the output
// processing half way through, as a listener would.
class Engine {
   public:
    static constexpr std::array<iclforge::ac4::Speaker, 12> kLayout = {
        iclforge::ac4::Speaker::kLeft,         iclforge::ac4::Speaker::kRight,
        iclforge::ac4::Speaker::kCentre,       iclforge::ac4::Speaker::kLfe,
        iclforge::ac4::Speaker::kLeftSurround, iclforge::ac4::Speaker::kRightSurround,
        iclforge::ac4::Speaker::kLeftBack,     iclforge::ac4::Speaker::kRightBack,
        iclforge::ac4::Speaker::kLeftWide,     iclforge::ac4::Speaker::kRightWide,
        iclforge::ac4::Speaker::kTopFrontLeft, iclforge::ac4::Speaker::kTopFrontRight,
    };

    // Decodes the unit: the error, with refusal() saying why, where it does
    // not decode.
    std::optional<iclforge::ac4::DecodeError> decode(std::span<const std::byte> unit) {
        const auto info = decoder_.decode_by_block(
            unit, [this](const iclforge::ac4::PcmBlock& block) { place(block); });
        if (!info) {
            refusal_ = std::string{decoder_.refusal_reason()};
            return info.error();
        }
        if (!info->has_value()) {
            ++waited_;
        } else {
            ++frames_;
            samples_ += (**info).samples;
        }
        return std::nullopt;
    }

    void finish() {
        decoder_.flush([this](const iclforge::ac4::PcmBlock& block) { place(block); });
    }

    void set_output(const iclforge::ac4::OutputConfig& output) { decoder_.set_output(output); }

    [[nodiscard]] const std::array<std::vector<float>, kLayout.size()>& out() const { return out_; }
    [[nodiscard]] std::size_t frames() const { return frames_; }
    [[nodiscard]] std::size_t waited() const { return waited_; }
    [[nodiscard]] std::size_t samples() const { return samples_; }
    [[nodiscard]] std::size_t blocks() const { return blocks_; }
    [[nodiscard]] std::size_t short_blocks() const { return short_blocks_; }
    [[nodiscard]] const std::string& refusal() const { return refusal_; }
    [[nodiscard]] bool finite() const { return finite_; }

   private:
    void place(const iclforge::ac4::PcmBlock& block) {
        ++blocks_;
        if (block.samples != iclforge::ac4::kBlockSamples) {
            ++short_blocks_;
        }
        for (std::size_t slot = 0; slot < kLayout.size(); ++slot) {
            const auto it = std::ranges::find(block.speakers, kLayout[slot]);
            if (it == block.speakers.end()) {
                out_[slot].insert(out_[slot].end(), block.samples, 0.0F);
                continue;
            }
            const std::span<const float> channel =
                block.channels[static_cast<std::size_t>(it - block.speakers.begin())];
            finite_ =
                finite_ && std::ranges::all_of(channel, [](float x) { return std::isfinite(x); });
            out_[slot].insert(out_[slot].end(), channel.begin(), channel.end());
        }
    }

    iclforge::ac4::Decoder decoder_;
    std::array<std::vector<float>, kLayout.size()> out_;
    std::size_t frames_ = 0;
    std::size_t waited_ = 0;
    std::size_t samples_ = 0;
    std::size_t blocks_ = 0;
    std::size_t short_blocks_ = 0;
    std::string refusal_;
    bool finite_ = true;
};

// What the engine made of one stream.
struct Played {
    bool refused = false;
    std::string refusal;
};

constexpr std::size_t kAllFrames = std::numeric_limits<std::size_t>::max();

// Plays the first `limit` frames of `path` through the engine, the output level
// and dialogue enhancement changed half way, and holds it to decode()
// configured the same way.
Played play(const fs::path& path, std::size_t limit = kAllFrames) {
    INFO("stream " << path.string());
    auto frames = frames_of(path);
    REQUIRE_FALSE(frames.empty());
    frames.resize(std::min(frames.size(), limit));
    const iclforge::ac4::OutputConfig later{.output_level_dbfs = -24.0,
                                            .drc = iclforge::ac4::DrcMode::kDefault,
                                            .dialogue_enhancement_db = 6.0};
    Engine engine;
    iclforge::ac4::Decoder reference;
    std::vector<std::vector<float>> expected(Engine::kLayout.size());
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f == frames.size() / 2) {
            engine.set_output(later);
            reference.set_output(later);
        }
        if (const std::optional<iclforge::ac4::DecodeError> error = engine.decode(frames[f])) {
            // Syntax this version refuses by name ends the stream; anything
            // else is a failure.
            INFO("frame " << f << ": " << engine.refusal());
            REQUIRE(*error == iclforge::ac4::DecodeError::kUnsupported);
            return Played{.refused = true, .refusal = engine.refusal()};
        }
        const auto decoded = reference.decode(frames[f]);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        const iclforge::ac4::DecodedFrame& d = **decoded;
        for (std::size_t slot = 0; slot < Engine::kLayout.size(); ++slot) {
            const auto it = std::ranges::find(d.speakers, Engine::kLayout[slot]);
            if (it == d.speakers.end()) {
                expected[slot].insert(expected[slot].end(), d.samples, 0.0F);
            } else {
                const auto& channel = d.channels[static_cast<std::size_t>(it - d.speakers.begin())];
                expected[slot].insert(expected[slot].end(), channel.begin(), channel.end());
            }
        }
    }
    engine.finish();
    // Only a stream joined before its first I-frame waits, and only there.
    CHECK(engine.frames() + engine.waited() == frames.size());
    CHECK(engine.frames() > 0);
    CHECK(engine.short_blocks() <= 1);
    CHECK(engine.finite());
    for (std::size_t slot = 0; slot < Engine::kLayout.size(); ++slot) {
        REQUIRE(engine.out()[slot].size() == engine.samples());
        REQUIRE(engine.out()[slot] == expected[slot]);
    }
    return {};
}

}  // namespace

TEST_CASE(
    "an engine in the shape of Hearth's decodes every committed AC-4 stream through the public API",
    "[ac4dec][api]") {
    const std::vector<fs::path> streams = committed_streams();
    REQUIRE(streams.size() >= 42);
    for (const fs::path& path : streams_to_play(streams)) {
        const Played played = play(path, kSanitized ? kSanitizedFrames : kAllFrames);
        INFO("stream " << path.string() << ": " << played.refusal);
        CHECK_FALSE(played.refused);
    }
}

TEST_CASE("an engine in the shape of Hearth's decodes the streams of AC4DEC_API_STREAM_DIR",
          "[ac4dec][api]") {
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
    std::map<std::string, int> refused;
    int decoded = 0;
    for (const fs::path& path : streams) {
        const Played played = play(path);
        if (played.refused) {
            ++refused[played.refusal];
        } else {
            ++decoded;
        }
    }
    std::ostringstream summary;
    summary << decoded << " of " << streams.size() << " streams decoded;";
    for (const auto& [reason, count] : refused) {
        summary << " " << count << " refused: " << reason << ";";
    }
    WARN(summary.str());
    CHECK(decoded > 0);
}
