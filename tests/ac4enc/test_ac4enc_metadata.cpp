// The AC-4 encoder's metadata (planning/ac4.md, phase E5): further loudness
// values, DRC's decoder modes, the stereo downmix's values and dialogue
// enhancement, read back by the decoder with the encoder's trace, and applied
// by the decoder's output processing (phase D6) with the gains their formulas
// give.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

struct Encoded {
    std::vector<ac4::EncodedFrame> frames;
    std::vector<ac4::SyntaxRecord> trace;
};

// Encodes planar input in pieces of `piece` samples, with the dialogue in it
// where there is a stem, then flushes.
Encoded encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input,
               const std::vector<std::vector<float>>* dialogue = nullptr,
               std::size_t piece = 4096) {
    Encoded out;
    ac4::EncoderConfig config = base;
    const auto sink = [&out](const ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    config.trace = sink;
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::size_t total = input.front().size();
    for (std::size_t at = 0; at < total; at += piece) {
        const std::size_t count = std::min(piece, total - at);
        std::vector<std::span<const float>> views;
        std::vector<std::span<const float>> stem;
        for (const auto& channel : input) {
            views.emplace_back(std::span<const float>(channel).subspan(at, count));
        }
        if (dialogue != nullptr) {
            for (const auto& channel : *dialogue) {
                stem.emplace_back(std::span<const float>(channel).subspan(at, count));
            }
        }
        auto frames = dialogue != nullptr ? encoder->encode(views, stem) : encoder->encode(views);
        REQUIRE(frames.has_value());
        out.frames.insert(out.frames.end(), frames->begin(), frames->end());
    }
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    return out;
}

// The decoder's output for the stream, as `output` configures it.
std::vector<std::vector<float>> decode(const std::vector<ac4::EncodedFrame>& frames,
                                       const ac4::OutputConfig& output) {
    ac4::Decoder decoder(ac4::DecoderConfig{.syntax = {}, .output = output, .concealment = {}});
    std::vector<std::vector<float>> out;
    for (const ac4::EncodedFrame& frame : frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        out.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    return out;
}

// Every substream of every frame reads to its end, and the decoder's trace is
// the encoder's record for record; returns the decoder's records.
std::vector<ac4::SyntaxRecord> read_back(const Encoded& encoded,
                                         std::vector<std::size_t>& frame_starts) {
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::Decoder decoder(ac4::DecoderConfig{.syntax = sink, .output = {}, .concealment = {}});
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        frame_starts.push_back(read.size());
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
    return read;
}

// The values of the records named `name` in frame f's records, in order.
std::vector<std::uint64_t> values(const std::vector<ac4::SyntaxRecord>& read,
                                  const std::vector<std::size_t>& starts, std::size_t f,
                                  std::string_view name) {
    std::vector<std::uint64_t> out;
    const std::size_t end = f + 1 < starts.size() ? starts[f + 1] : read.size();
    for (std::size_t i = starts[f]; i < end; ++i) {
        if (read[i].name == name) {
            out.push_back(read[i].value);
        }
    }
    return out;
}

std::vector<float> tone(double hz, double amplitude, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return x;
}

// The power of `samples` at `hz` by Goertzel's recurrence, past the first
// second, where the decoder's and encoder's delays and the first frames are.
// A Hann window keeps the other tones out: unwindowed, a tone 10 dB louder
// 300 Hz away moves the measurement by 0.02 dB.
double tone_power(std::span<const float> samples, double hz) {
    const auto body = samples.subspan(48000, samples.size() - 48000 - 8192);
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const double coeff = 2.0 * std::cos(w);
    const auto n = static_cast<double>(body.size());
    double s1 = 0.0;
    double s2 = 0.0;
    double window_sum = 0.0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / n);
        const double s0 = hann * static_cast<double>(body[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        window_sum += hann;
    }
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (window_sum * window_sum);
}

// An output configuration: the level, DRC mode, dialogue enhancement and
// downmix, the rest as the decoder's defaults.
ac4::OutputConfig output(std::optional<double> level, ac4::DrcMode drc, double dialogue_db,
                         ac4::DownmixTarget target) {
    ac4::OutputConfig out;
    out.output_level_dbfs = level;
    out.drc = drc;
    out.dialogue_enhancement_db = dialogue_db;
    out.downmix = target;
    return out;
}

double db_of_power(double ratio) {
    return 10.0 * std::log10(ratio);
}

// L R C LFE Ls Rs, each its own tone at -20 dBFS, for three seconds.
constexpr std::array<double, 6> kTone = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};

std::vector<std::vector<float>> tones_51() {
    std::vector<std::vector<float>> input;
    for (const double hz : kTone) {
        input.push_back(tone(hz, 0.1, 3 * 48000));
    }
    return input;
}

ac4::EncoderConfig config_51() {
    ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    config.codec_mode = ac4::CodecMode::kSimple;
    config.dialnorm_db = -24.0;
    config.loudness = ac4::FurtherLoudness{.practice = ac4::LoudnessPractice::kEbuR128,
                                           .corrected_with_gating = std::nullopt,
                                           .corrected_in_real_time = false,
                                           .integrated_lkfs = -23.0,
                                           .speech_gated_lkfs = -22.4,
                                           .speech_gating = ac4::DialogueGating::kLeftCentreRight,
                                           .max_short_term_lufs = -18.5,
                                           .max_true_peak_dbtp = -1.2,
                                           .loudness_range_lu = 7.5,
                                           .loudness_range_v2 = true,
                                           .max_momentary_lufs = -16.3};
    config.drc = ac4::DrcConfig{.profile = ac4::DrcProfile::kMusicStandard,
                                .modes = {{.id = 0,
                                           .output_level_from_db = 0,
                                           .output_level_to_db = 0,
                                           .profile = std::nullopt,
                                           .repeat_of = std::nullopt,
                                           .gains_config = std::nullopt},
                                          {.id = 1,
                                           .output_level_from_db = 0,
                                           .output_level_to_db = 0,
                                           .profile = ac4::DrcProfile::kSpeech,
                                           .repeat_of = std::nullopt,
                                           .gains_config = std::nullopt},
                                          {.id = 2,
                                           .output_level_from_db = 0,
                                           .output_level_to_db = 0,
                                           .profile = std::nullopt,
                                           .repeat_of = 1,
                                           .gains_config = std::nullopt},
                                          {.id = 5,
                                           .output_level_from_db = -20,
                                           .output_level_to_db = -12,
                                           .profile = ac4::DrcProfile::kFilmLight,
                                           .repeat_of = std::nullopt,
                                           .gains_config = std::nullopt}}};
    config.downmix = ac4::DownmixConfig{.loro_centre_db = -1.5,
                                        .loro_surround_db = -4.5,
                                        .ltrt_centre_db = -3.0,
                                        .ltrt_surround_db = -6.0,
                                        .lfe_db = -10.5,
                                        .preferred = ac4::PreferredDownmix::kLtRt,
                                        .loro_correction_db2 = 1.5,
                                        .ltrt_correction_db2 = std::nullopt};
    config.dialogue = ac4::DialogueConfig{.source = ac4::DialogueSource::kMarkedChannels,
                                          .left = false,
                                          .right = false,
                                          .centre = true,
                                          .max_gain_db = 12};
    return config;
}

}  // namespace

TEST_CASE("the encoder's metadata reads back as configured, with the encoder's trace",
          "[ac4enc][metadata]") {
    const Encoded encoded = encode(config_51(), tones_51());
    std::vector<std::size_t> starts;
    const std::vector<ac4::SyntaxRecord> read = read_back(encoded, starts);
    REQUIRE(encoded.frames.size() > 26);
    REQUIRE(encoded.frames[0].iframe);
    REQUIRE_FALSE(encoded.frames[1].iframe);
    const auto one = [&](std::size_t f, std::string_view name) {
        const std::vector<std::uint64_t> v = values(read, starts, f, name);
        INFO(name);
        REQUIRE(v.size() == 1);
        return v.front();
    };

    // Loudness: the practice every frame, the values in I-frames, at
    // floor(value x 10 + 1/2) + 1 024 (Part 1 clause 4.3.12.3).
    CHECK(one(0, "dialnorm_bits") == 96);
    CHECK(one(0, "loud_prac_type") == 2);
    CHECK(one(0, "loudrelgat") == 1024 - 230);
    CHECK(one(0, "loudspchgat") == 1024 - 224);
    CHECK(values(read, starts, 0, "dialgate_prac_type") == std::vector<std::uint64_t>{2});
    CHECK(one(0, "max_loudstrm3s") == 1024 - 185);
    CHECK(one(0, "max_truepk") == 1024 - 12);
    CHECK(one(0, "lra") == 75);
    CHECK(one(0, "lra_prac_type") == 1);
    CHECK(one(0, "max_loudmntry") == 1024 - 163);
    CHECK(one(1, "loud_prac_type") == 2);
    CHECK(one(1, "b_loudrelgat") == 0);

    // DRC in I-frames: the modes, the default profile's flag, the speech
    // profile as a curve, a repeat, and mode 5's output levels.
    CHECK(one(0, "b_drc_present") == 1);
    CHECK(one(0, "drc_decoder_nr_modes") == 3);
    CHECK(values(read, starts, 0, "drc_decoder_mode_id") == std::vector<std::uint64_t>{0, 1, 2, 5});
    CHECK(values(read, starts, 0, "drc_repeat_profile_flag") ==
          std::vector<std::uint64_t>{0, 0, 1, 0});
    CHECK(one(0, "drc_repeat_id") == 1);
    CHECK(values(read, starts, 0, "drc_default_profile_flag") ==
          std::vector<std::uint64_t>{1, 0, 0});
    CHECK(values(read, starts, 0, "drc_gain_max_boost") == std::vector<std::uint64_t>{15, 6});
    CHECK(one(0, "drc_output_level_from") == 20);
    CHECK(one(0, "drc_output_level_to") == 12);
    CHECK(one(0, "drc_eac3_profile") == 3);
    CHECK(one(1, "b_drc_present") == 0);

    // The downmix in I-frames: Tables 149 and 149a's codes, the LFE's, the
    // preferred method and Lo/Ro's correction, x = 15 - 2 g.
    CHECK(one(0, "b_stereo_dmx_coeff") == 1);
    CHECK(one(0, "loro_centre_mixgain") == 3);
    CHECK(one(0, "loro_surround_mixgain") == 5);
    CHECK(one(0, "b_ltrt_mixinfo") == 1);
    CHECK(one(0, "ltrt_centre_mixgain") == 4);
    CHECK(one(0, "ltrt_surround_mixgain") == 6);
    CHECK(one(0, "lfe_mixgain") == 16);
    CHECK(one(0, "preferred_dmx_method") == 2);
    CHECK(one(0, "loro_dmx_loud_corr") == 12);
    CHECK(one(0, "b_ltrt_loud_comp") == 0);
    CHECK(one(1, "b_stereo_dmx_coeff") == 0);

    // Dialogue enhancement: the configuration in I-frames, the parameters of
    // the marked centre, all 1 (Table 209's index 10), and kept after.
    CHECK(one(0, "de_method") == 0);
    CHECK(one(0, "de_max_gain") == 3);
    CHECK(one(0, "de_channel_config") == 1);
    CHECK(values(read, starts, 0, "de_par_code").size() == 8);
    CHECK(one(1, "b_de_config_flag") == 0);
    CHECK(one(1, "de_keep_data_flag") == 1);

    // The table of contents says the stream carries dialogue enhancement.
    auto encoder = ac4::Encoder::create(config_51());
    REQUIRE(encoder.has_value());
    REQUIRE_FALSE(encoder->toc().presentations_v1.empty());
    CHECK(encoder->toc().presentations_v1.front().de_indicator == true);
}

TEST_CASE("loudness values without a practice read back, with no correction flags",
          "[ac4enc][metadata]") {
    ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 128;
    config.loudness = ac4::FurtherLoudness{};
    config.loudness->integrated_lkfs = -23.0;
    config.loudness->max_true_peak_dbtp = -1.0;
    const Encoded encoded =
        encode(config, {tone(440.0, 0.1, 48000 / 2), tone(660.0, 0.1, 48000 / 2)});
    std::vector<std::size_t> starts;
    const std::vector<ac4::SyntaxRecord> read = read_back(encoded, starts);
    CHECK(values(read, starts, 0, "loud_prac_type") == std::vector<std::uint64_t>{0});
    CHECK(values(read, starts, 0, "b_loudcorr_dialgate").empty());
    CHECK(values(read, starts, 0, "loudrelgat") == std::vector<std::uint64_t>{1024 - 230});
    CHECK(values(read, starts, 0, "max_truepk") == std::vector<std::uint64_t>{1024 - 10});
}

TEST_CASE("the decoder applies the encoder's metadata with the gains its formulas give",
          "[ac4enc][metadata]") {
    const Encoded encoded = encode(config_51(), tones_51());
    const std::vector<std::vector<float>> coded = decode(encoded.frames, {});
    REQUIRE(coded.size() == 6);
    const auto level_db = [](double power) { return 10.0 * std::log10(power); };

    SECTION("the output level: 2^((Lout - dialnorm) / 6)") {
        const std::vector<std::vector<float>> out = decode(
            encoded.frames, output(-31.0, ac4::DrcMode::kOff, 0.0, ac4::DownmixTarget::kAsCoded));
        const double expected = 20.0 * std::log10(std::exp2((-31.0 + 24.0) / 6.0));
        for (std::size_t c = 0; c < 6; ++c) {
            CAPTURE(c);
            CHECK(std::abs(level_db(tone_power(out[c], kTone[c]) / tone_power(coded[c], kTone[c])) -
                           expected) < 0.01);
        }
    }
    SECTION("dialogue enhancement: the marked centre raised by the gain asked for, up to the cap") {
        for (const double asked : {6.0, 12.0, 15.0}) {
            CAPTURE(asked);
            const std::vector<std::vector<float>> out = decode(
                encoded.frames,
                output(std::nullopt, ac4::DrcMode::kDefault, asked, ac4::DownmixTarget::kAsCoded));
            CHECK(std::abs(level_db(tone_power(out[2], kTone[2]) / tone_power(coded[2], kTone[2])) -
                           std::min(asked, 12.0)) < 0.01);
            CHECK(std::abs(level_db(tone_power(out[0], kTone[0]) /
                                    tone_power(coded[0], kTone[0]))) < 0.01);
        }
    }
    SECTION("the downmixes: the stream's gains and Lo/Ro's correction") {
        const double correction_db = 20.0 * std::log10(std::exp2(1.5 / 6.0));
        struct Case {
            ac4::DownmixTarget target;
            double centre_db;
            double left_surround_db;
            double right_surround_db;
            double lfe_db;
        };
        // Lo/Ro: C at -1.5 dB and Ls at -4.5 into Lo, with the correction;
        // Lt/Rt: C at -3 dB and both surrounds at -6 into Lt, uncorrected.
        for (const Case& c :
             {Case{ac4::DownmixTarget::kLoRo, -1.5 + correction_db, -4.5 + correction_db,
                   -std::numeric_limits<double>::infinity(), -10.5 + correction_db},
              Case{ac4::DownmixTarget::kLtRt, -3.0, -6.0, -6.0, -10.5}}) {
            CAPTURE(static_cast<int>(c.target));
            const std::vector<std::vector<float>> out =
                decode(encoded.frames, output(std::nullopt, ac4::DrcMode::kDefault, 0.0, c.target));
            REQUIRE(out.size() == 2);
            const auto into_left = [&](std::size_t input) {
                return level_db(tone_power(out[0], kTone[input]) /
                                tone_power(coded[input], kTone[input]));
            };
            CHECK(std::abs(into_left(2) - c.centre_db) < 0.01);
            CHECK(std::abs(into_left(4) - c.left_surround_db) < 0.01);
            if (std::isfinite(c.right_surround_db)) {
                CHECK(std::abs(into_left(5) - c.right_surround_db) < 0.01);
            } else {
                CHECK(into_left(5) < -60.0);
            }
            CHECK(std::abs(into_left(3) - c.lfe_db) < 0.01);
        }
    }
}

TEST_CASE("dialogue enhancement from a stem raises the dialogue and leaves the rest",
          "[ac4enc][metadata]") {
    // Stereo: a 1 kHz tone, the dialogue, in L and R under noise-like music
    // of three far tones; the stem is the dialogue alone. Centred for the
    // channel-independent method and the Mid, and panned for the
    // cross-channel method, which follows the panning.
    const std::size_t count = 3 * 48000;
    const std::vector<float> dialogue_tone = tone(1000.0, 0.1, count);
    const std::vector<float> music_low = tone(90.0, 0.1, count);
    const std::vector<float> music_high = tone(9000.0, 0.05, count);
    struct Method {
        ac4::DialogueMethod method;
        std::array<float, 2> pan;
        std::uint64_t de_method;
    };
    for (const Method& m : {Method{ac4::DialogueMethod::kChannelIndependent, {1.0F, 1.0F}, 0},
                            Method{ac4::DialogueMethod::kMid, {1.0F, 1.0F}, 0},
                            Method{ac4::DialogueMethod::kCrossChannel, {1.2F, 0.6F}, 1}}) {
        CAPTURE(static_cast<int>(m.method));
        std::vector<std::vector<float>> programme(2, std::vector<float>(count));
        std::vector<std::vector<float>> dialogue(2, std::vector<float>(count));
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t n = 0; n < count; ++n) {
                dialogue[c][n] = m.pan[c] * dialogue_tone[n];
                programme[c][n] = dialogue[c][n] + music_low[n] + music_high[n];
            }
        }
        ac4::EncoderConfig config;
        config.channels = 2;
        config.bitrate_kbps = 192;
        config.codec_mode = ac4::CodecMode::kSimple;
        config.dialogue = ac4::DialogueConfig{.method = m.method,
                                              .source = ac4::DialogueSource::kStem,
                                              .left = true,
                                              .right = true,
                                              .centre = false,
                                              .max_gain_db = 9};
        const Encoded encoded = encode(config, programme, &dialogue);
        std::vector<std::size_t> starts;
        const std::vector<ac4::SyntaxRecord> read = read_back(encoded, starts);
        CHECK(values(read, starts, 0, "de_method") == std::vector<std::uint64_t>{m.de_method});
        const std::vector<std::uint64_t> ms = values(read, starts, 0, "de_ms_proc_flag");
        CHECK(ms ==
              (m.de_method == 0
                   ? std::vector<std::uint64_t>{m.method == ac4::DialogueMethod::kMid ? 1U : 0U}
                   : std::vector<std::uint64_t>{}));
        const std::vector<std::vector<float>> coded = decode(encoded.frames, {});
        const std::vector<std::vector<float>> raised =
            decode(encoded.frames,
                   output(std::nullopt, ac4::DrcMode::kDefault, 9.0, ac4::DownmixTarget::kAsCoded));
        for (std::size_t c = 0; c < 2; ++c) {
            CAPTURE(c);
            const double dialogue_db =
                db_of_power(tone_power(raised[c], 1000.0) / tone_power(coded[c], 1000.0));
            const double low_db =
                db_of_power(tone_power(raised[c], 90.0) / tone_power(coded[c], 90.0));
            const double high_db =
                db_of_power(tone_power(raised[c], 9000.0) / tone_power(coded[c], 9000.0));
            CAPTURE(dialogue_db, low_db, high_db);
            CHECK(dialogue_db > 8.5);
            CHECK(dialogue_db < 9.5);
            CHECK(std::abs(low_db) < 0.5);
            CHECK(std::abs(high_db) < 0.5);
        }
    }
    std::vector<std::vector<float>> programme(2, std::vector<float>(count));
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t n = 0; n < count; ++n) {
            programme[c][n] = dialogue_tone[n] + music_low[n] + music_high[n];
        }
    }
    ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 192;
    config.dialogue = ac4::DialogueConfig{.source = ac4::DialogueSource::kStem,
                                          .left = true,
                                          .right = true,
                                          .centre = false,
                                          .max_gain_db = 9};

    // A stem is given with the programme, and only where one is configured.
    auto stemmed = ac4::Encoder::create(config);
    REQUIRE(stemmed.has_value());
    const std::vector<std::span<const float>> views = {programme[0], programme[1]};
    const auto without = stemmed->encode(views);
    REQUIRE_FALSE(without.has_value());
    CHECK(without.error() == ac4::EncodeError::kInvalidInput);
    config.dialogue->source = ac4::DialogueSource::kMarkedChannels;
    auto marked = ac4::Encoder::create(config);
    REQUIRE(marked.has_value());
    const auto with = marked->encode(views, views);
    REQUIRE_FALSE(with.has_value());
    CHECK(with.error() == ac4::EncodeError::kInvalidInput);
}

TEST_CASE("DRC gains sent from a profile read back and compress as the profile's curve does",
          "[ac4enc][metadata]") {
    // Two seconds at -40 dBFS, which the profile boosts, then two at -12,
    // which it cuts, and back: 1 kHz in every channel but the LFE.
    const std::size_t second = 48000;
    const auto programme = [&](int channels) {
        std::vector<std::vector<float>> out;
        const std::vector<float> quiet = tone(1000.0, 0.01, 6 * second);
        for (int c = 0; c < channels; ++c) {
            std::vector<float> x(6 * second);
            for (std::size_t n = 0; n < x.size(); ++n) {
                const bool loud = n >= 2 * second && n < 4 * second;
                x[n] = (channels == 6 && c == 3) ? 0.0F : quiet[n] * (loud ? 25.0F : 1.0F);
            }
            out.push_back(std::move(x));
        }
        return out;
    };
    for (const int channels : {2, 6}) {
        for (const int gains_config : {0, 1, 2, 3}) {
            CAPTURE(channels, gains_config);
            ac4::EncoderConfig config;
            config.channels = channels;
            config.bitrate_kbps = channels == 2 ? 192 : 384;
            config.codec_mode = ac4::CodecMode::kSimple;
            config.dialnorm_db = -24.0;
            config.experimental.drc_gains = true;
            // Home theatre sends the film standard profile's gains, flat
            // panel applies the profile as a curve, and portable speakers
            // repeats the first.
            ac4::DrcModeConfig gains;
            gains.id = 0;
            gains.gains_config = gains_config;
            ac4::DrcModeConfig curve;
            curve.id = 1;
            ac4::DrcModeConfig repeat;
            repeat.id = 2;
            repeat.repeat_of = 0;
            config.drc = ac4::DrcConfig{.profile = ac4::DrcProfile::kFilmStandard,
                                        .modes = {gains, curve, repeat}};
            const Encoded encoded = encode(config, programme(channels));
            std::vector<std::size_t> starts;
            const std::vector<ac4::SyntaxRecord> read = read_back(encoded, starts);
            CHECK(values(read, starts, 0, "drc_gains_config") ==
                  std::vector<std::uint64_t>{static_cast<std::uint64_t>(gains_config)});
            // Gains in every frame, for the mode and its repeat.
            CHECK(values(read, starts, 1, "drc_gain_val").size() == 2);

            // The two modes' outputs follow each other within the gains'
            // whole dB2 steps, once the smoothing has settled, where the
            // profile moves the level by several dB.
            ac4::OutputConfig as_gains =
                output(-24.0, ac4::DrcMode::kHomeTheatre, 0.0, ac4::DownmixTarget::kAsCoded);
            ac4::OutputConfig as_curve =
                output(-24.0, ac4::DrcMode::kFlatPanelTv, 0.0, ac4::DownmixTarget::kAsCoded);
            ac4::OutputConfig as_is =
                output(-24.0, ac4::DrcMode::kOff, 0.0, ac4::DownmixTarget::kAsCoded);
            const std::vector<std::vector<float>> by_gains = decode(encoded.frames, as_gains);
            const std::vector<std::vector<float>> by_curve = decode(encoded.frames, as_curve);
            const std::vector<std::vector<float>> plain = decode(encoded.frames, as_is);
            const auto rms_db = [](const std::vector<float>& x, std::size_t from,
                                   std::size_t count) {
                double sum = 0.0;
                for (std::size_t n = from; n < from + count; ++n) {
                    sum += static_cast<double>(x[n]) * static_cast<double>(x[n]);
                }
                return 10.0 * std::log10(sum / static_cast<double>(count) + 1e-30);
            };
            const std::size_t lag = 3072 + 1313;
            for (const std::size_t at :
                 {second + second / 2, 3 * second + second / 2, 5 * second + second / 2}) {
                CAPTURE(at);
                const double gains_db =
                    rms_db(by_gains[0], lag + at, 4800) - rms_db(plain[0], lag + at, 4800);
                const double curve_db =
                    rms_db(by_curve[0], lag + at, 4800) - rms_db(plain[0], lag + at, 4800);
                CAPTURE(gains_db, curve_db);
                CHECK(std::abs(curve_db) > 2.0);
                CHECK(std::abs(gains_db - curve_db) < 1.0);
            }
        }
    }
    // Gains are experimental.
    ac4::EncoderConfig config;
    config.channels = 2;
    ac4::DrcModeConfig gains;
    gains.gains_config = 0;
    config.drc = ac4::DrcConfig{.profile = ac4::DrcProfile::kFilmStandard, .modes = {gains}};
    CHECK_FALSE(ac4::Encoder::create(config).has_value());
    config.experimental.drc_gains = true;
    CHECK(ac4::Encoder::create(config).has_value());
    config.drc->modes.front().gains_config = 4;
    CHECK_FALSE(ac4::Encoder::create(config).has_value());
}

TEST_CASE("the encoder refuses metadata the syntax cannot send", "[ac4enc][metadata]") {
    const auto refused = [](const ac4::EncoderConfig& config) {
        const auto encoder = ac4::Encoder::create(config);
        return !encoder.has_value() && encoder.error() == ac4::EncodeError::kInvalidConfig;
    };
    ac4::EncoderConfig stereo;
    stereo.channels = 2;
    stereo.bitrate_kbps = 128;

    SECTION("a downmix for stereo, and gains outside their tables") {
        ac4::EncoderConfig config = stereo;
        config.downmix = ac4::DownmixConfig{};
        CHECK(refused(config));
        config = config_51();
        config.downmix->loro_centre_db = -2.0;
        CHECK(refused(config));
        config = config_51();
        config.downmix->loro_surround_db = 3.0;
        CHECK(refused(config));
        config = config_51();
        config.downmix->lfe_db = -10.0;
        CHECK(refused(config));
        config = config_51();
        config.downmix->loro_correction_db2 = 8.0;
        CHECK(refused(config));
    }
    SECTION("dialogue enhancement on a channel the layout lacks, or at a cap it cannot send") {
        ac4::EncoderConfig config = stereo;
        config.dialogue = ac4::DialogueConfig{};  // the centre
        CHECK(refused(config));
        config = config_51();
        config.dialogue->max_gain_db = 10;
        CHECK(refused(config));
        // The Mid is L and R's alone; the cross-channel method pans over two
        // channels or three, from a stem.
        config = config_51();
        config.dialogue = ac4::DialogueConfig{.method = ac4::DialogueMethod::kMid,
                                              .source = ac4::DialogueSource::kMarkedChannels,
                                              .left = true,
                                              .right = true,
                                              .centre = true,
                                              .max_gain_db = 9};
        CHECK(refused(config));
        config.dialogue->method = ac4::DialogueMethod::kCrossChannel;
        CHECK(refused(config));
        config.dialogue->source = ac4::DialogueSource::kStem;
        CHECK_FALSE(refused(config));
        config.dialogue->left = false;
        config.dialogue->right = false;
        CHECK(refused(config));
    }
    SECTION("DRC modes: an id past 7, one twice, a repeat of none, output levels out of order") {
        ac4::EncoderConfig config = config_51();
        ac4::DrcModeConfig past;
        past.id = 8;
        config.drc->modes.push_back(past);
        CHECK(refused(config));
        config = config_51();
        ac4::DrcModeConfig twice;
        twice.id = 0;
        config.drc->modes.push_back(twice);
        CHECK(refused(config));
        config = config_51();
        config.drc->modes[2].repeat_of = 6;
        CHECK(refused(config));
        config = config_51();
        config.drc->modes[3].output_level_from_db = -10;
        CHECK(refused(config));
    }
    SECTION("a loudness value past the 11 bits, and a correction without a practice") {
        ac4::EncoderConfig config = stereo;
        config.loudness = ac4::FurtherLoudness{};
        config.loudness->integrated_lkfs = -200.0;
        CHECK(refused(config));
        config.loudness = ac4::FurtherLoudness{};
        config.loudness->corrected_in_real_time = true;
        CHECK(refused(config));
        config.loudness = ac4::FurtherLoudness{};
        config.loudness->corrected_with_gating = ac4::DialogueGating::kManual;
        CHECK(refused(config));
    }
}
