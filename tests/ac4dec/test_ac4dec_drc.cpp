// The output level and dynamic range control (src/ac4dec/src/pcm/drc.hpp, ETSI
// TS 103 190-1 V1.4.1 clause 5.7.9): Table 162's profiles and Table 166's
// transmitted curves against the curves the text defines, DEE's transmitted
// curves against the profiles it named, Table 161's choice of a DRC decoder
// mode, the stage's static curve measured with stepped tones at steady state,
// its time constants and the transmitted gains; and through ac4::Decoder, the
// output level gain on the encoder's streams at dialnorms from -31 to -17 and
// DEE's compression in the modes it configures.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "dsp/qmf.hpp"
#include "pcm/drc.hpp"

namespace {

namespace detail = ac4::detail;
using QmfValue = std::complex<double>;

constexpr int kSlots = 32;  // num_qmf_timeslots at frame_rate_index 13
constexpr int kFrame = kSlots * 64;

// A steady sine through the QMF analysis at the decoder's scale (full scale
// 2^15), a frame's matrix at a time; its amplitude can change between frames.
class ToneFrames {
   public:
    explicit ToneFrames(double hz) : hz_(hz) {}

    std::vector<QmfValue> next(double amplitude) {
        std::vector<double> pcm(kFrame);
        for (double& x : pcm) {
            x = 32768.0 * amplitude *
                std::sin(2.0 * std::numbers::pi * hz_ * static_cast<double>(n_) / 48000.0);
            ++n_;
        }
        std::vector<QmfValue> q(pcm.size());
        analysis_.process(pcm, q);
        return q;
    }

   private:
    double hz_;
    long long n_ = 0;
    detail::dsp::QmfAnalysis<double> analysis_;
};

// The amplitude of a 997 Hz sine in one channel whose loudness is `relative`
// dB from dialnorm: BS.1770 reads a full-scale one at -3.01 LKFS.
double amplitude_for(double relative, double dialnorm) {
    return std::pow(10.0, (relative + dialnorm + 3.01) / 20.0);
}

// Runs one frame of a stereo pair, the tone in L and silence in R, through the
// stage.
void run_frame(detail::DrcStage& stage, const ac4::OutputConfig& output,
               const detail::DrcFrameValues& values, std::vector<QmfValue> left) {
    std::vector<QmfValue> right(left.size());
    std::array<std::vector<QmfValue>*, 2> matrices = {&left, &right};
    stage.process(output, values, matrices, matrices);
}

// DEE's transmitted curves for the modes of the gold leg
// 51-music-192-drc-per-device (tools/references/ac4_syntax.py trace), in
// DrcCompressionCurve's field order.
detail::DrcCompressionCurve transmitted(int nullband_low, int nullband_high, int max_boost,
                                        int lev_max_boost, int max_cut, int lev_max_cut, int nr_cut,
                                        int section_cut_gain, int section_cut_level, int release,
                                        int release_fast, int threshold_attack,
                                        int threshold_release) {
    return {.drc_lev_nullband_low = nullband_low,
            .drc_lev_nullband_high = nullband_high,
            .drc_gain_max_boost = max_boost,
            .drc_lev_max_boost = lev_max_boost,
            .drc_nr_boost_sections = 0,
            .drc_gain_section_boost = 0,
            .drc_lev_section_boost = 0,
            .drc_gain_max_cut = max_cut,
            .drc_lev_max_cut = lev_max_cut,
            .drc_nr_cut_sections = nr_cut,
            .drc_gain_section_cut = section_cut_gain,
            .drc_lev_section_cut = section_cut_level,
            .drc_tc_default_flag = false,
            .drc_tc_attack = 20,
            .drc_tc_release = release,
            .drc_tc_attack_fast = 2,
            .drc_tc_release_fast = release_fast,
            .drc_adaptive_smoothing_flag = true,
            .drc_attack_threshold = threshold_attack,
            .drc_release_threshold = threshold_release};
}

std::vector<std::byte> read_stream(const std::string& leg) {
    const std::filesystem::path path =
        std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

// Each channel of every frame of `frames`, decoded under `output` and joined.
std::vector<std::vector<float>> decode_all(std::span<const std::span<const std::byte>> frames,
                                           const ac4::OutputConfig& output) {
    ac4::Decoder decoder(ac4::DecoderConfig{.syntax = {}, .output = output});
    std::vector<std::vector<float>> out;
    for (const std::span<const std::byte> frame : frames) {
        const auto decoded = decoder.decode(frame);
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

double rms_db(std::span<const float> x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return 10.0 * std::log10(sum / static_cast<double>(x.size()));
}

}  // namespace

TEST_CASE("Table 162's profiles are the compression curves the text defines", "[ac4dec][drc]") {
    struct Point {
        int profile;
        double level;
        double gain;
    };
    // Table 162 through clause 5.7.9.3.1.2's sections: Film standard, Film
    // light, Music standard, Music light, Speech.
    constexpr std::array<Point, 30> kPoints{{
        {1, -40.0, 6.0},  {1, -12.0, 6.0},  {1, -6.0, 3.0},   {1, 0.0, 0.0},    {1, 5.0, 0.0},
        {1, 10.0, -2.5},  {1, 15.0, -5.0},  {1, 25.0, -14.5}, {1, 35.0, -24.0}, {1, 50.0, -24.0},
        {2, -30.0, 6.0},  {2, -16.0, 3.0},  {2, 0.0, 0.0},    {2, 15.0, -2.5},  {2, 30.0, -14.5},
        {2, 45.0, -24.0}, {3, -30.0, 12.0}, {3, -12.0, 6.0},  {3, 25.0, -14.5}, {4, -40.0, 12.0},
        {4, -22.0, 6.0},  {4, 10.0, 0.0},   {4, 25.0, -7.5},  {4, 50.0, -15.0}, {5, -30.0, 15.0},
        {5, -9.5, 7.5},   {5, 2.0, 0.0},    {5, 10.0, -2.5},  {5, 25.0, -14.5}, {5, 40.0, -24.0},
    }};
    for (const Point& point : kPoints) {
        CAPTURE(point.profile, point.level);
        const std::optional<detail::DrcCurve> curve = detail::drc_default_curve(point.profile);
        REQUIRE(curve.has_value());
        CHECK(std::abs(curve->gain(point.level) - point.gain) < 1e-12);
    }
    // None holds every level where it is; the reserved profiles have no curve.
    const detail::DrcCurve none = *detail::drc_default_curve(0);
    for (double level = -60.0; level <= 60.0; level += 7.5) {
        CHECK(none.gain(level) == 0.0);
    }
    CHECK_FALSE(detail::drc_default_curve(6).has_value());
    CHECK_FALSE(detail::drc_default_curve(7).has_value());
    // The time constants of Music standard and Speech.
    CHECK(detail::drc_default_curve(3)->release_ms == 10000.0);
    CHECK(detail::drc_default_curve(5)->release_fast_ms == 200.0);
    CHECK(detail::drc_default_curve(5)->attack_threshold == 10.0);
}

TEST_CASE("DEE's transmitted curves take Table 166's points to the profiles it names",
          "[ac4dec][drc]") {
    struct Mode {
        detail::DrcCompressionCurve sent;
        int profile;
    };
    // Home theatre as Music light, portable speakers as Music standard and
    // portable headphones as Speech, as DEE sent them.
    const std::array<Mode, 3> modes = {{
        {transmitted(10, 10, 12, 23, 15, 29, 0, 0, 0, 75, 50, 15, 20), 4},
        {transmitted(0, 5, 12, 23, 24, 19, 1, 4, 9, 250, 50, 15, 20), 3},
        {transmitted(0, 5, 15, 18, 24, 19, 1, 4, 9, 25, 10, 10, 10), 5},
    }};
    for (const Mode& mode : modes) {
        CAPTURE(mode.profile);
        const detail::DrcCurve sent = detail::drc_curve(mode.sent);
        const detail::DrcCurve profile = *detail::drc_default_curve(mode.profile);
        for (double level = -60.0; level <= 60.0; level += 0.5) {
            CAPTURE(level);
            CHECK(std::abs(sent.gain(level) - profile.gain(level)) < 1e-12);
        }
        CHECK(sent.attack_ms == profile.attack_ms);
        CHECK(sent.release_ms == profile.release_ms);
        CHECK(sent.attack_fast_ms == profile.attack_fast_ms);
        CHECK(sent.release_fast_ms == profile.release_fast_ms);
        CHECK(sent.attack_threshold == profile.attack_threshold);
        CHECK(sent.release_threshold == profile.release_threshold);
    }
}

TEST_CASE("Table 161 chooses the DRC decoder mode for the output level", "[ac4dec][drc]") {
    detail::DrcConfig config;
    for (std::size_t id = 0; id < 4; ++id) {
        config.mode[id].configured = true;
    }
    const auto chosen = [&config](double level, bool headphones = false) {
        return detail::drc_mode_for(config, ac4::DrcMode::kDefault, level, headphones);
    };
    // The ranges' edges belong to them, and a fractional level goes to its
    // nearest whole dB.
    CHECK(chosen(-31.0) == 0);
    CHECK(chosen(-27.0) == 0);
    CHECK(chosen(-26.6) == 0);
    CHECK(chosen(-26.4) == 1);
    CHECK(chosen(-17.0) == 1);
    CHECK(chosen(-16.0) == 2);
    CHECK(chosen(-16.0, true) == 3);
    CHECK(chosen(0.0) == 2);
    CHECK(chosen(0.0, true) == 3);
    CHECK_FALSE(chosen(-32.0).has_value());
    CHECK_FALSE(chosen(1.0).has_value());
    // Asked for by name, whatever the level; off, or a mode the stream does
    // not configure, compresses nothing.
    CHECK(detail::drc_mode_for(config, ac4::DrcMode::kHomeTheatre, -10.0, false) == 0);
    CHECK(detail::drc_mode_for(config, ac4::DrcMode::kPortableHeadphones, -31.0, false) == 3);
    CHECK_FALSE(detail::drc_mode_for(config, ac4::DrcMode::kOff, -31.0, false).has_value());
    config.mode[3].configured = false;
    CHECK_FALSE(chosen(-10.0, true).has_value());
    CHECK_FALSE(
        detail::drc_mode_for(config, ac4::DrcMode::kPortableHeadphones, -10.0, false).has_value());
    // A mode the stream adds, over its own range, and the largest id wins.
    config.mode[5].configured = true;
    config.mode[5].drc_output_level_from = 20;
    config.mode[5].drc_output_level_to = 10;
    CHECK(chosen(-12.0) == 5);
    CHECK(chosen(-20.0) == 5);
    CHECK(chosen(-25.0) == 1);
}

TEST_CASE(
    "the DRC stage's static curve, measured with stepped tones at steady state, is each profile's "
    "within 0.5 dB",
    "[ac4dec][drc]") {
    const std::array<ac4::Speaker, 2> speakers = {ac4::Speaker::kLeft, ac4::Speaker::kRight};
    constexpr double kDialnorm = -24.0;
    // Lout at dialnorm: the output level gain is 1, and the gain is the curve's.
    const ac4::OutputConfig output{
        .output_level_dbfs = kDialnorm, .drc = ac4::DrcMode::kDefault, .headphones = false};
    for (int profile = 1; profile <= 5; ++profile) {
        CAPTURE(profile);
        const detail::DrcCurve curve = *detail::drc_default_curve(profile);
        detail::DrcStage stage;
        stage.configure(48000.0, kSlots, speakers, false);
        ToneFrames tone(997.0);
        for (double relative = -50.0; relative <= 25.0; relative += 5.0) {
            CAPTURE(relative);
            const double amplitude = amplitude_for(relative, kDialnorm);
            // Each step starts the smoothing afresh, as drc_reset_flag does.
            for (int f = 0; f < 12; ++f) {
                const detail::DrcFrameValues values{
                    .dialnorm = kDialnorm, .curve = curve, .gains = std::nullopt, .reset = f == 0};
                run_frame(stage, output, values, tone.next(amplitude));
            }
            const double measured = 6.0 * std::log2(stage.last_gain());
            CHECK(std::abs(measured - curve.gain(relative)) < 0.5);
        }
    }
}

TEST_CASE("the DRC stage moves to a new gain with the attack and release time constants",
          "[ac4dec][drc]") {
    const std::array<ac4::Speaker, 2> speakers = {ac4::Speaker::kLeft, ac4::Speaker::kRight};
    constexpr double kDialnorm = -24.0;
    const ac4::OutputConfig output{
        .output_level_dbfs = kDialnorm, .drc = ac4::DrcMode::kDefault, .headphones = false};
    // Film standard, smoothing with its two time constants only.
    detail::DrcCurve curve = *detail::drc_default_curve(1);
    curve.adaptive = false;
    detail::DrcStage stage;
    stage.configure(48000.0, kSlots, speakers, false);
    ToneFrames tone(997.0);
    const auto frame = [&](double relative, bool reset) {
        run_frame(stage, output,
                  {.dialnorm = kDialnorm, .curve = curve, .gains = std::nullopt, .reset = reset},
                  tone.next(amplitude_for(relative, kDialnorm)));
    };
    // Steady in the null band, then 25 dB up into the cut: the gain falls
    // towards -14.5 dB2 with tau_attack, 100 ms. The analysis spreads the step
    // over its first ten or so slots, so the attack is held to 5% of the gap
    // (twice tau would miss by 12%).
    frame(0.0, true);
    for (int f = 0; f < 11; ++f) {
        frame(0.0, false);
    }
    const double start = stage.last_gain();
    const double cut = std::exp2(curve.gain(25.0) / 6.0);
    const double slot_ms = 64.0 * 1000.0 / 48000.0;
    for (int f = 1; f <= 4; ++f) {
        frame(25.0, false);
        const double t = f * kSlots * slot_ms;
        const double expected = cut + (start - cut) * std::exp2(-t / curve.attack_ms);
        CAPTURE(f, stage.last_gain(), expected);
        CHECK(std::abs(stage.last_gain() - expected) < 0.05 * std::abs(start - cut));
    }
    for (int f = 0; f < 20; ++f) {
        frame(25.0, false);
    }
    // Back to the null band: the gain rises back with tau_release, 3 s.
    const double held = stage.last_gain();
    for (int f = 1; f <= 72; ++f) {
        frame(0.0, false);
        if (f % 24 == 0) {
            const double t = f * kSlots * slot_ms;
            const double expected = 1.0 + (held - 1.0) * std::exp2(-t / curve.release_ms);
            CAPTURE(f, stage.last_gain(), expected);
            CHECK(std::abs(stage.last_gain() - expected) < 0.02 * std::abs(1.0 - held));
        }
    }
}

TEST_CASE("transmitted DRC gains apply by channel group, band and subframe", "[ac4dec][drc]") {
    const std::array<ac4::Speaker, 6> speakers = {
        ac4::Speaker::kLeft, ac4::Speaker::kRight,        ac4::Speaker::kCentre,
        ac4::Speaker::kLfe,  ac4::Speaker::kLeftSurround, ac4::Speaker::kRightSurround};
    detail::DrcGainset set;
    set.drc_gains_config = 3;  // four bands per group (Table 164)
    set.nr_drc_channels = 3;
    set.nr_drc_bands = 4;
    set.nr_drc_subframes = 8;  // Table 169 at 2 048 samples
    set.gains_present = true;
    for (int group = 0; group < 3; ++group) {
        for (int sf = 0; sf < 8; ++sf) {
            for (int band = 0; band < 4; ++band) {
                set.drc_gain[static_cast<std::size_t>(
                    (group * detail::kMaxDrcSubframes + sf) * detail::kMaxDrcBands + band)] =
                    static_cast<std::int16_t>(group * 10 + sf - band);
            }
        }
    }
    detail::DrcStage stage;
    stage.configure(48000.0, kSlots, speakers, false);
    // Dialnorm 6 dB2 under the output level: a gain of 2 besides the DRC's.
    const ac4::OutputConfig output{
        .output_level_dbfs = -24.0, .drc = ac4::DrcMode::kDefault, .headphones = false};
    std::vector<std::vector<QmfValue>> channels(
        speakers.size(), std::vector<QmfValue>(kSlots * 64, QmfValue{1.0, 0.0}));
    std::vector<std::vector<QmfValue>*> matrices;
    for (auto& channel : channels) {
        matrices.push_back(&channel);
    }
    stage.process(output, {.dialnorm = -30.0, .curve = std::nullopt, .gains = set, .reset = false},
                  matrices, matrices);
    // Table 168's groups for 5.1: L, R and the LFE; C; Ls and Rs.
    constexpr std::array<int, 6> kGroup = {0, 0, 1, 0, 2, 2};
    const auto band_of = [](int k) { return k == 0 ? 0 : k <= 4 ? 1 : k <= 16 ? 2 : 3; };
    for (std::size_t c = 0; c < channels.size(); ++c) {
        for (int slot = 0; slot < kSlots; ++slot) {
            for (int k = 0; k < 64; ++k) {
                const int sf = slot / 4;
                const double gain =
                    2.0 * std::exp2(static_cast<double>(kGroup[c] * 10 + sf - band_of(k)) / 6.0);
                const QmfValue got = channels[c][static_cast<std::size_t>(slot * 64 + k)];
                if (std::abs(got.real() - gain) > 1e-9 * gain) {
                    FAIL("channel " << c << " slot " << slot << " subband " << k << ": "
                                    << got.real() << ", expected " << gain);
                }
            }
        }
    }
}

TEST_CASE(
    "decode takes dialnorm to the output level by 2^((Lout - dialnorm) / 6), dialnorms -31 to -17",
    "[ac4dec][drc]") {
    // Two seconds of a 997 Hz tone at -20 dBFS in both channels.
    constexpr std::size_t kLength = 96000;
    std::vector<std::vector<float>> input(2, std::vector<float>(kLength));
    for (std::size_t n = 0; n < kLength; ++n) {
        const auto x = static_cast<float>(
            0.1 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(n) / 48000.0));
        input[0][n] = x;
        input[1][n] = x;
    }
    for (const double dialnorm : {-31.0, -27.0, -24.0, -20.0, -17.0}) {
        CAPTURE(dialnorm);
        ac4::EncoderConfig config;
        config.dialnorm_db = dialnorm;
        auto encoder = ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        std::vector<ac4::EncodedFrame> frames;
        std::vector<std::span<const float>> views = {input[0], input[1]};
        auto encoded = encoder->encode(views);
        REQUIRE(encoded.has_value());
        frames.insert(frames.end(), encoded->begin(), encoded->end());
        auto rest = encoder->flush();
        REQUIRE(rest.has_value());
        frames.insert(frames.end(), rest->begin(), rest->end());
        std::vector<std::span<const std::byte>> raw;
        for (const ac4::EncodedFrame& frame : frames) {
            raw.emplace_back(frame.raw_ac4_frame);
        }
        const auto coded = decode_all(raw, ac4::OutputConfig{});
        for (const double lout : {-31.0, -17.0}) {
            CAPTURE(lout);
            const auto levelled = decode_all(raw, ac4::OutputConfig{.output_level_dbfs = lout,
                                                                    .drc = ac4::DrcMode::kOff,
                                                                    .headphones = false});
            REQUIRE(levelled.size() == coded.size());
            // Past the first frames, which play before the first dialnorm's
            // signal arrives.
            const std::span<const float> window =
                std::span<const float>(coded[0]).subspan(8192, 65536);
            const std::span<const float> scaled =
                std::span<const float>(levelled[0]).subspan(8192, 65536);
            const double expected = 20.0 * std::log10(std::exp2((lout - dialnorm) / 6.0));
            CHECK(std::abs(rms_db(scaled) - rms_db(window) - expected) < 0.01);
        }
    }
}

TEST_CASE("DEE's 5.1 stream compresses in the modes it configures, within its curves",
          "[ac4dec][drc]") {
    // Made with the home theatre mode as Music light and portable headphones
    // as Speech (gen_ac4_baseline.py, ac4-51-drc-ltrt-192).
    const std::vector<std::byte> stream = read_stream("ac4-51-drc-ltrt-192");
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    std::vector<std::span<const std::byte>> raw;
    for (const ac4::SyncFrame& frame : scan.frames) {
        raw.push_back(frame.raw_ac4_frame);
    }
    const auto off = decode_all(
        raw, {.output_level_dbfs = -31.0, .drc = ac4::DrcMode::kOff, .headphones = false});
    struct Mode {
        ac4::DrcMode mode;
        double most_boost;  // dB2
        double most_cut;
    };
    for (const Mode mode : {Mode{ac4::DrcMode::kHomeTheatre, 12.0, -15.0},
                            Mode{ac4::DrcMode::kPortableHeadphones, 15.0, -24.0}}) {
        CAPTURE(ac4::describe(mode.mode));
        const auto compressed =
            decode_all(raw, {.output_level_dbfs = -31.0, .drc = mode.mode, .headphones = false});
        REQUIRE(compressed.size() == off.size());
        // Frame by frame, the level against the uncompressed one stays inside
        // what the curve can give, and somewhere the curve works.
        double largest = 0.0;
        for (std::size_t start = 8192; start + 2048 <= off[0].size(); start += 2048) {
            const double reference = rms_db(std::span<const float>(off[0]).subspan(start, 2048));
            if (reference < -70.0) {
                continue;
            }
            const double change =
                rms_db(std::span<const float>(compressed[0]).subspan(start, 2048)) - reference;
            CHECK(change < 20.0 * std::log10(std::exp2(mode.most_boost / 6.0)) + 0.5);
            CHECK(change > 20.0 * std::log10(std::exp2(mode.most_cut / 6.0)) - 0.5);
            largest = std::max(largest, std::abs(change));
        }
        CHECK(largest > 1.0);
    }
}
