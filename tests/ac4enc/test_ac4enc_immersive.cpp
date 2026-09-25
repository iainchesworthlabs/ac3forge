// ac4::Encoder's immersive layouts end to end (planning/ac4.md, phase E8): 5.1.4
// and 5.0.4 in the immersive element of ETSI TS 103 190-2 V1.3.1 clause 6.2.4,
// as DEE writes it, in SCPL, ASPX_SCPL and ASPX_ACPL_2 by the rate, and the
// experimental 7.1.4 with the back pair and ASPX_ACPL_1. Each stream reads back
// with the trace the encoder recorded and decodes, in full decoding, with each
// channel's tone on its own channel, and in core decoding with each on the
// core layout's speaker at the core gain, as src/ac4dec's tests hold DEE's
// 5.1.4 streams to. The signals are half a second long, which is what the
// sanitizer builds can afford for every mode.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

using ac4::Speaker;

// The decoder's delay at frame_rate_index 13 (d_pcm, the QMF banks' 577
// samples and six QMF slots) and the encoder's, a frame and a half.
constexpr std::size_t kLag = 3072 + 352 + 577 + 6 * 64;
constexpr std::size_t kSamples = 24000;
constexpr double kAmplitude = 0.1;  // -20 dBFS

// gen_ac4_baseline.py's tones for L R C LFE Ls Rs Tfl Tfr Tbl Tbr, and two more
// primes for Lb and Rb, 150 Hz or more from the rest.
struct Channel {
    Speaker speaker;
    double hz;
};

// A layout's channels in the decoder's order, which is the encoder's input
// order: L R C, the LFE, Ls Rs, the back pair, Tfl Tfr Tbl Tbr.
std::vector<Channel> layout(bool lfe, bool backs) {
    std::vector<Channel> out = {
        {Speaker::kLeft, 331.0}, {Speaker::kRight, 457.0}, {Speaker::kCentre, 613.0}};
    if (lfe) {
        out.push_back({Speaker::kLfe, 47.0});
    }
    out.insert(out.end(), {{Speaker::kLeftSurround, 787.0}, {Speaker::kRightSurround, 953.0}});
    if (backs) {
        out.insert(out.end(), {{Speaker::kLeftBack, 1777.0}, {Speaker::kRightBack, 1931.0}});
    }
    out.insert(out.end(), {{Speaker::kTopFrontLeft, 1117.0},
                           {Speaker::kTopFrontRight, 1289.0},
                           {Speaker::kTopBackLeft, 1453.0},
                           {Speaker::kTopBackRight, 1621.0}});
    return out;
}

std::vector<float> tone(double hz, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            kAmplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return x;
}

std::vector<std::vector<float>> tones(const std::vector<Channel>& channels) {
    std::vector<std::vector<float>> input;
    for (const Channel& c : channels) {
        input.push_back(tone(c.hz, kSamples));
    }
    return input;
}

struct Encoded {
    std::vector<ac4::EncodedFrame> frames;
    std::vector<ac4::SyntaxRecord> trace;
    ac4::CodecMode mode = ac4::CodecMode::kAuto;
};

Encoded encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input) {
    Encoded out;
    ac4::EncoderConfig config = base;
    const auto sink = [&out](const ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    config.trace = sink;
    auto encoder = ac4::Encoder::create(config);
    INFO(ac4::Encoder::refusal_reason(base));
    REQUIRE(encoder.has_value());
    out.mode = encoder->codec_mode();
    std::vector<std::span<const float>> views;
    for (const auto& channel : input) {
        views.emplace_back(channel);
    }
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    out.frames = *frames;
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    return out;
}

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
};

Decoded decode(const std::vector<ac4::EncodedFrame>& frames, ac4::DecodingMode mode,
               ac4::DownmixTarget target = ac4::DownmixTarget::kAsCoded) {
    ac4::DecoderConfig config;
    config.decoding = mode;
    config.output.downmix = target;
    ac4::Decoder decoder(config);
    Decoded out;
    for (const ac4::EncodedFrame& frame : frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        out.speakers = pcm.speakers;
        out.channels.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
    }
    return out;
}

// Every substream of every frame reads to its end, and the decoder's trace is
// the encoder's, record for record.
void check_frames_read_back(const Encoded& encoded) {
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        const auto report = decoder.parse(frame.raw_ac4_frame);
        REQUIRE(report.has_value());
        for (const ac4::SubstreamReport& substream : report->substreams) {
            CAPTURE(substream.index, substream.refused_reason);
            REQUIRE_FALSE(substream.refused.has_value());
            CHECK(substream.bits_read == substream.size_bits);
        }
    }
    REQUIRE(read.size() == encoded.trace.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < read.size(); ++i) {
        const bool same = read[i].substream == encoded.trace[i].substream &&
                          read[i].bit_offset == encoded.trace[i].bit_offset &&
                          read[i].bits == encoded.trace[i].bits &&
                          read[i].value == encoded.trace[i].value;
        if (!same && mismatches++ < 5) {
            CAPTURE(i, encoded.trace[i].name, read[i].name, encoded.trace[i].value, read[i].value);
            CHECK(same);
        }
    }
    CHECK(mismatches == 0);
}

std::size_t count_records(const Encoded& encoded, std::string_view name, std::uint64_t value) {
    return static_cast<std::size_t>(std::ranges::count_if(
        encoded.trace,
        [&](const ac4::SyntaxRecord& r) { return r.name == name && r.value == value; }));
}

// The amplitude of x's component at `hz` over the decoded output after the
// lag and a frame's settling, through a Hann window, as a fraction of the
// source's tone amplitude.
double level(std::span<const float> x, double hz) {
    const std::size_t first = kLag + 4096;
    const std::size_t count = kSamples - 8192;
    double re = 0.0;
    double im = 0.0;
    double weight = 0.0;
    for (std::size_t n = 0; n < count && first + n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                              static_cast<double>(count));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(first + n) / 48000.0;
        re += w * static_cast<double>(x[first + n]) * std::cos(phase);
        im -= w * static_cast<double>(x[first + n]) * std::sin(phase);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight / kAmplitude;
}

double db(double ratio) {
    return 20.0 * std::log10(std::max(ratio, 1e-30));
}

std::size_t index_of(const Decoded& d, Speaker speaker) {
    const auto it = std::ranges::find(d.speakers, speaker);
    REQUIRE(it != d.speakers.end());
    return static_cast<std::size_t>(it - d.speakers.begin());
}

// Each channel's own tone within 0.2 dB of unity, and every other tone 60 dB
// under it there: planning/ac4.md, E3's exit, held to E8's layouts. `skip`
// names channels held to other checks.
void check_routing(const std::vector<Channel>& channels, const Decoded& decoded,
                   std::span<const Speaker> skip = {}) {
    REQUIRE(decoded.speakers.size() == channels.size());
    for (const Channel& own : channels) {
        if (std::ranges::find(skip, own.speaker) != skip.end()) {
            continue;
        }
        const std::size_t c = index_of(decoded, own.speaker);
        CAPTURE(own.hz);
        const double gain = level(decoded.channels[c], own.hz);
        CHECK(std::abs(db(gain)) < 0.2);
        for (const Channel& other : channels) {
            if (other.speaker != own.speaker) {
                CAPTURE(other.hz);
                CHECK(db(gain / level(decoded.channels[c], other.hz)) > 60.0);
            }
        }
    }
}

// Core decoding (Part 2 clause 4.7): the 5.X.2 core, L R C, the LFE, Ls Rs and
// the top side pair, each top pair's two tones in its side of it 3 dB down, as
// DEE's 5.1.4 streams decode (src/ac4dec/ERRATA.md, "The core's top pair is
// Tsl and Tsr"), and every other channel's tone at unity on its own.
void check_core(const std::vector<Channel>& channels, const Decoded& core) {
    const bool lfe =
        std::ranges::any_of(channels, [](const Channel& c) { return c.speaker == Speaker::kLfe; });
    std::vector<Speaker> expected = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre};
    if (lfe) {
        expected.push_back(Speaker::kLfe);
    }
    expected.insert(expected.end(), {Speaker::kLeftSurround, Speaker::kRightSurround,
                                     Speaker::kTopSideLeft, Speaker::kTopSideRight});
    REQUIRE(core.speakers == expected);
    for (const Channel& c : channels) {
        CAPTURE(c.hz);
        Speaker where = c.speaker;
        double gain_db = 0.0;
        if (c.speaker == Speaker::kTopFrontLeft || c.speaker == Speaker::kTopBackLeft) {
            where = Speaker::kTopSideLeft;
            gain_db = -3.01;
        } else if (c.speaker == Speaker::kTopFrontRight || c.speaker == Speaker::kTopBackRight) {
            where = Speaker::kTopSideRight;
            gain_db = -3.01;
        }
        CHECK(std::abs(db(level(core.channels[index_of(core, where)], c.hz)) - gain_db) < 0.2);
    }
}

}  // namespace

TEST_CASE(
    "5.1.4 in each codec mode DEE writes decodes to each channel's tone, in full and core decoding",
    "[ac4enc][encoder][immersive]") {
    // kAuto takes DEE's modes by the rate: SCPL at 768 kbps, ASPX_SCPL at 512
    // and ASPX_ACPL_2 at 256 (Table 73's codes 0, 1 and 3). In ASPX_ACPL_2 the
    // top pairs are rebuilt from their sums band by band, which parts two tones
    // only where they lie in different parameter bands: there the top tones sit
    // mid-subband in subbands 3 to 6, each a band of its own (Part 1 Table
    // 197), where a tone near a subband's edge would reach the next band too.
    struct Case {
        int kbps;
        ac4::CodecMode mode;
        std::uint64_t code;
    };
    for (const Case c :
         {Case{768, ac4::CodecMode::kScpl, 0}, Case{512, ac4::CodecMode::kAspxScpl, 1},
          Case{256, ac4::CodecMode::kAspxAcpl2, 3}}) {
        CAPTURE(c.kbps);
        std::vector<Channel> channels = layout(true, false);
        if (c.mode == ac4::CodecMode::kAspxAcpl2) {
            channels[6].hz = 1310.0;  // Tfl, subband 3
            channels[7].hz = 1690.0;  // Tfr, subband 4
            channels[8].hz = 2060.0;  // Tbl, subband 5
            channels[9].hz = 2440.0;  // Tbr, subband 6
        }
        const std::vector<std::vector<float>> input = tones(channels);
        const Encoded encoded = encode({.channels = 10, .bitrate_kbps = c.kbps}, input);
        CHECK(encoded.mode == c.mode);
        CHECK(count_records(encoded, "immersive_codec_mode_code", c.code) == encoded.frames.size());
        // DEE's form: core_5ch_grouping 0, 2ch_mode 0 and b_use_sap_add_ch 0.
        CHECK(count_records(encoded, "core_5ch_grouping", 0) == encoded.frames.size());
        CHECK(count_records(encoded, "2ch_mode", 0) == encoded.frames.size());
        CHECK(count_records(encoded, "b_use_sap_add_ch", 0) == encoded.frames.size());
        check_frames_read_back(encoded);
        const Decoded full = decode(encoded.frames, ac4::DecodingMode::kFull);
        if (c.mode == ac4::CodecMode::kAspxAcpl2) {
            // A-CPL makes each top pair of its sum, which it keeps exactly
            // (Pseudocode 2: Tfl + Tbl = 2 sqrt 2 F''), and puts each tone on its
            // own channel of the pair.
            const std::array<Speaker, 4> tops = {Speaker::kTopFrontLeft, Speaker::kTopFrontRight,
                                                 Speaker::kTopBackLeft, Speaker::kTopBackRight};
            check_routing(channels, full, tops);
            for (const auto& [a, b] : {std::pair{0U, 2U}, std::pair{1U, 3U}}) {
                const Channel& x = channels[6 + a];
                const Channel& y = channels[6 + b];
                std::vector<float> sum = full.channels[index_of(full, x.speaker)];
                const std::vector<float>& second = full.channels[index_of(full, y.speaker)];
                for (std::size_t n = 0; n < sum.size(); ++n) {
                    sum[n] += second[n];
                }
                for (const Channel& t : {x, y}) {
                    CAPTURE(t.hz);
                    CHECK(std::abs(db(level(sum, t.hz))) < 0.2);
                    const Speaker other = t.speaker == x.speaker ? y.speaker : x.speaker;
                    CHECK(std::abs(db(level(full.channels[index_of(full, t.speaker)], t.hz))) <
                          0.5);
                    CHECK(db(level(full.channels[index_of(full, t.speaker)], t.hz) /
                             level(full.channels[index_of(full, other)], t.hz)) > 20.0);
                }
            }
        } else {
            check_routing(channels, full);
        }
        check_core(channels, decode(encoded.frames, ac4::DecodingMode::kCore));
    }
}

TEST_CASE("5.0.4, and 7.1.4 with the back pair, put each channel's tone on its own channel",
          "[ac4enc][encoder][immersive]") {
    SECTION("5.0.4 in ASPX_SCPL") {
        const std::vector<Channel> channels = layout(false, false);
        const Encoded encoded = encode({.channels = 9, .bitrate_kbps = 512}, tones(channels));
        CHECK(encoded.mode == ac4::CodecMode::kAspxScpl);
        check_frames_read_back(encoded);
        check_routing(channels, decode(encoded.frames, ac4::DecodingMode::kFull));
    }
    SECTION("7.1.4 in SCPL, experimental") {
        const std::vector<Channel> channels = layout(true, true);
        const Encoded encoded =
            encode({.channels = 12, .bitrate_kbps = 1024, .experimental = {.back_pair = true}},
                   tones(channels));
        CHECK(encoded.mode == ac4::CodecMode::kScpl);
        check_frames_read_back(encoded);
        check_routing(channels, decode(encoded.frames, ac4::DecodingMode::kFull));
    }
    SECTION("5.1.4 in ASPX_ACPL_1, experimental") {
        // The differences below acpl_qmf_band, 3 kHz, above every tone: the
        // pairs come back as simple coupling would make them.
        const std::vector<Channel> channels = layout(true, false);
        const Encoded encoded = encode({.channels = 10,
                                        .bitrate_kbps = 320,
                                        .codec_mode = ac4::CodecMode::kAspxAcpl1,
                                        .experimental = {.acpl = true}},
                                       tones(channels));
        CHECK(encoded.mode == ac4::CodecMode::kAspxAcpl1);
        CHECK(count_records(encoded, "immersive_codec_mode_code", 2) == encoded.frames.size());
        check_frames_read_back(encoded);
        check_routing(channels, decode(encoded.frames, ac4::DecodingMode::kFull));
    }
}

TEST_CASE("the immersive layouts' table of contents, levels and MP4 description",
          "[ac4enc][encoder][immersive]") {
    struct Case {
        int channels;
        int kbps;
        bool backs;
        int ch_mode;
        int md_compat;
        ac4::CodecMode mode;
    };
    for (const Case c : {Case{10, 192, false, 12, 2, ac4::CodecMode::kAspxAcpl2},
                         Case{10, 448, false, 12, 2, ac4::CodecMode::kAspxAcpl2},
                         Case{10, 512, false, 12, 2, ac4::CodecMode::kAspxScpl},
                         Case{10, 768, false, 12, 2, ac4::CodecMode::kScpl},
                         Case{9, 512, false, 11, 2, ac4::CodecMode::kAspxScpl},
                         Case{12, 768, true, 12, 3, ac4::CodecMode::kAspxScpl}}) {
        CAPTURE(c.channels, c.kbps);
        const ac4::EncoderConfig config{
            .channels = c.channels, .bitrate_kbps = c.kbps, .experimental = {.back_pair = c.backs}};
        auto encoder = ac4::Encoder::create(config);
        INFO(ac4::Encoder::refusal_reason(config));
        REQUIRE(encoder.has_value());
        CHECK(encoder->codec_mode() == c.mode);
        const ac4::Toc& toc = encoder->toc();
        const auto& chan = toc.substream_groups.at(0).substreams.at(0).chan;
        REQUIRE(chan.has_value());
        CHECK(chan->ch_mode == c.ch_mode);
        // The source's channels (Part 2 Tables 57 to 59): the back pair only
        // with it, the centre, and both top pairs.
        REQUIRE(chan->original_content.has_value());
        CHECK(chan->original_content->b_4_back_channels_present == c.backs);
        CHECK(chan->original_content->b_centre_present);
        CHECK(chan->original_content->top_channels_present == 3);
        // md_compat counts the source's channels but the LFE: 2 for DEE's
        // 5.1.4, 3 for 7.1.4 (Part 2 Table 55).
        REQUIRE(toc.presentations_v1.size() == 1);
        CHECK(toc.presentations_v1.front().md_compat == c.md_compat);
        CHECK(toc.presentations_v1.front().immersive_audio_indicator == true);
        CHECK_FALSE(ac4::build_dac4(toc).empty());
    }
}

TEST_CASE("the height downmix sends DEE's custom downmix data, which the renderer applies",
          "[ac4enc][encoder][immersive]") {
    // Tfl's and Tbl's tones alone; rendered to 5.1 the top pairs go where the
    // configuration sends them, at its gain (Part 2 clause 6.2.9.8,
    // tool_t4_to_f_s()): kFront both to L, kSurround both to Ls, and
    // kFrontAndSurround Tfl to L and Tbl to Ls. The tones are mid-subband, in
    // A-CPL parameter bands of their own, as ASPX_ACPL_2 needs to part them.
    struct Case {
        ac4::HeightDownmix mode;
        double gain_db;
        Speaker front_to;
        Speaker back_to;
    };
    constexpr double kTfl = 1310.0;
    constexpr double kTbl = 2060.0;
    std::vector<std::vector<float>> input(10, std::vector<float>(kSamples, 0.0F));
    input[6] = tone(kTfl, kSamples);
    input[8] = tone(kTbl, kSamples);
    for (const Case c :
         {Case{ac4::HeightDownmix::kFront, -6.0, Speaker::kLeft, Speaker::kLeft},
          Case{ac4::HeightDownmix::kSurround, -4.5, Speaker::kLeftSurround, Speaker::kLeftSurround},
          Case{ac4::HeightDownmix::kFrontAndSurround, -9.0, Speaker::kLeft,
               Speaker::kLeftSurround}}) {
        CAPTURE(static_cast<int>(c.mode), c.gain_db);
        const Encoded encoded =
            encode({.channels = 10,
                    .bitrate_kbps = 256,
                    .iframe_interval = 6,
                    .downmix = ac4::DownmixConfig{.height = c.mode, .height_db = c.gain_db}},
                   input);
        // In I-frames alone, as DEE sends it: one configuration, 5.X.0.
        const std::size_t iframes = static_cast<std::size_t>(std::ranges::count_if(
            encoded.frames, [](const ac4::EncodedFrame& f) { return f.iframe; }));
        CHECK(count_records(encoded, "b_cdmx_data_present", 1) == iframes);
        CHECK(count_records(encoded, "b_cdmx_data_present", 0) == encoded.frames.size() - iframes);
        CHECK(count_records(encoded, "out_ch_config", 0) == iframes);
        check_frames_read_back(encoded);
        const Decoded five =
            decode(encoded.frames, ac4::DecodingMode::kFull, ac4::DownmixTarget::k5X);
        CHECK(std::abs(db(level(five.channels[index_of(five, c.front_to)], kTfl)) - c.gain_db) <
              0.3);
        CHECK(std::abs(db(level(five.channels[index_of(five, c.back_to)], kTbl)) - c.gain_db) <
              0.3);
    }
}

TEST_CASE("the encoder refuses the immersive configurations it does not write",
          "[ac4enc][encoder][immersive]") {
    struct Case {
        const char* name;
        ac4::EncoderConfig config;
        std::string_view says;
    };
    const std::vector<Case> cases = {
        {"7.1.4 without the back pair's option",
         {.channels = 12, .bitrate_kbps = 768},
         "back_pair"},
        {"SCPL for 5.1",
         {.channels = 6, .bitrate_kbps = 768, .codec_mode = ac4::CodecMode::kScpl},
         "immersive element alone"},
        {"ASPX_ACPL_3 for 5.1.4",
         {.channels = 10, .bitrate_kbps = 256, .codec_mode = ac4::CodecMode::kAspxAcpl3},
         "immersive layouts do not take"},
        {"ASPX_ACPL_1 without experimental.acpl",
         {.channels = 10, .bitrate_kbps = 256, .codec_mode = ac4::CodecMode::kAspxAcpl1},
         "immersive layouts do not take"},
        {"a height downmix for 5.1",
         {.channels = 6,
          .bitrate_kbps = 384,
          .downmix = ac4::DownmixConfig{.height = ac4::HeightDownmix::kFront}},
         "height downmix"},
        {"a height gain off Table 129",
         {.channels = 10,
          .bitrate_kbps = 512,
          .downmix = ac4::DownmixConfig{.height = ac4::HeightDownmix::kFront, .height_db = -2.0}},
         "height downmix"},
        {"the coding configurations for 5.1.4",
         {.channels = 10, .bitrate_kbps = 512, .experimental = {.coding_configs = true}},
         "coding_configs"},
        {"DRC gains per group for 5.1.4",
         {.channels = 10,
          .bitrate_kbps = 512,
          .drc = ac4::DrcConfig{.modes = {{.id = 0, .gains_config = 1}}},
          .experimental = {.drc_gains = true}},
         "immersive presentation"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CHECK_FALSE(ac4::Encoder::create(c.config).has_value());
        const std::string_view reason = ac4::Encoder::refusal_reason(c.config);
        CAPTURE(reason);
        CHECK(reason.find(c.says) != std::string_view::npos);
    }
}
