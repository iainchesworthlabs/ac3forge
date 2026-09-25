#include "ac4enc/encoder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "acpl/acpl_encoder.hpp"
#include "acpl/acpl_syntax.hpp"
#include "asf/analysis.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/multichannel.hpp"
#include "asf/psycho.hpp"
#include "asf/stereo.hpp"
#include "aspx/aspx_encoder.hpp"
#include "bit_writer.hpp"
#include "dsp/resampler.hpp"
#include "frame/dialogue.hpp"
#include "frame/drc_gains.hpp"
#include "frame/frame_writer.hpp"
#include "frame/metadata.hpp"
#include "frame/timing.hpp"
#include "tables/sfb_tables.hpp"

namespace ac4 {

std::string_view describe(EncodeError error) {
    switch (error) {
        case EncodeError::kInvalidConfig:
            return "the configuration is not one this encoder writes";
        case EncodeError::kInvalidInput:
            return "the input does not match the configuration, or holds a sample that is not finite";
    }
    return "unknown error";
}

namespace {

using detail::BitWriter;
using detail::FrameLayout;

// Why a configuration is refused: a string literal naming the rule it breaks,
// which Encoder::refusal_reason() returns.
using Refusal = std::string_view;

// The frame grid is the stream's (frame/timing.hpp): frame_length samples a
// frame at the internal rate, 2 048 at frame_rate_index 13. Ahead of the
// input, a frame and a half of silence (Impl::delay): the frame's long window
// starts at its first output sample, so a frame codes input from half a frame
// before it to half a frame after it, and this delay puts the next frame's
// transients, which set the frame's last window, inside the input read before
// the frame is coded. At index 13 that is 3 072 samples, which is also what
// DEE's encoder gives.
constexpr int kQmfSlot = 64;        // samples per QMF slot
constexpr int kSubBlocks = 16;      // transient detection, a sixteenth of a frame each
constexpr double kAttackRatio = 10.0;     // 10 dB over the sub-blocks before
constexpr double kAttackFloor = 1e-7;     // per sample: nothing below -70 dBFS is an attack
// The rate loop's steps, and what one is worth: a scale factor step, 2^(1/4)
// on the quantiser's step, moves its noise by 2^(3/8). From kCapSteps on, the
// cap on each band's noise at its energy rises a step at a time as well.
constexpr int kLowestStep = -120;
constexpr int kHighestStep = 255;
constexpr int kCapSteps = 60;
constexpr double kStepDb = 1.1289;
// Under the pull, the caps on a band's noise against its energy, tightest
// first: the rate loop takes the first the budget holds. ViSQOL marks the
// holes a looser cap leaves; a tighter one than the budget holds leaves
// every band at the cap's SNR. Measured on the race's sources, 2026-09-25.
constexpr std::array<double, 4> kCaps = {0.7, 1.0, 1.4, 2.0};
// The allowance of a band the spectral frontend leaves silent: every line
// quantises to 0 at the scale factor it gives, 255.
constexpr double kSilencedAllowance = 1e200;
// Below the thresholds, how far towards the level a band's allowance is taken,
// in dB: 1 holds every band to the level, which gives the most SNR for the
// bits and leaves the quietest bands at their thresholds; 0 lowers every band
// together. Measured on the race's sources, 2026-09-25.
constexpr double kLevelWeight = 0.75;

// The average bit rate (Part 1 clauses 4.3.3.2.4 and 6.2.4, Part 2 Annex B).
// A channel at the rate delivers a frame's share of it, F bytes, every frame
// period into the decoder's input buffer of v F bytes (v six, twelve above 60
// fps), and each frame leaves it at its output time, a frame period after
// the last's. In shares: before frame i is taken the buffer holds x(i), and
// what frame i of S(i) bytes leaves, s(i) = x(i) - S(i) / F, is the frame
// periods between its arrival and its output; x(i + 1) = s(i) + 1. A decoder
// that starts at frame i waits floor(s(i)) frames, which wait_frames sends
// (in twos at indices 10 to 12), and outputs up to a frame (two) early, so a
// frame leaves at least 1 (2); and at most v - 1, for the buffer to hold the
// next: s(i) from 1 to 5, or 2 to 11. Over frames 1 to m the sizes then add
// up to m + s(0) - s(m) shares, within Annex B's N' + 1 and N' - 1 (N' + 2
// and N' - 2). br_code carries the rate: 0b11, then the first six base-3
// digits of the fraction of log2(the rate in kbps) (Annex B's steps 2 and 4).
// A variable rate lets s run from minus to plus two seconds' shares, with no
// wait to send (wait_frames 7).
constexpr int kBrCodeDigits = 6;

// Below this rate a channel, CodecMode::kAuto codes in the ASPX mode: in mono
// and stereo as DEE does from 144 kbps in stereo down, and in the 5.X and 7.X
// elements as DEE's 5.1 streams do up to 320 kbps, SIMPLE from 384. The LFE
// is not counted.
constexpr double kAspxBelowKbps = 96.0;
constexpr double kAspxBelowKbpsMultichannel = 76.8;
// Below these rates a channel CodecMode::kAuto codes the 5.X element in an
// A-CPL mode, between the rates at which DEE's 5.1 streams change mode:
// ASPX_ACPL_3 at 96 kbps, ASPX_ACPL_2 at 128 and 144, and ASPX from 192.
constexpr double kAcpl3BelowKbps = 22.4;
constexpr double kAcpl2BelowKbps = 33.6;
// A-CPL's configuration, as DEE's streams send it: 15 parameter bands, fine
// quantisation (acpl_num_param_bands_id 0, acpl_quant_mode 0).
constexpr int kAcplBandsId = 0;
constexpr int kAcplQuantMode = 0;
// ASPX_ACPL_1's acpl_qmf_band, the top of its residuals: 8, the most
// acpl_qmf_band_minus1 sends, which keeps the waveform to 3 kHz at 48 kHz (a
// QMF subband is 375 Hz wide).
constexpr int kAcplResidualQmfBand = 8;

// The LFE's coded band: the scale factor bands that start below 120 Hz, the
// first three at 2 048 samples, to 140.6 Hz at 48 kHz, which is what DEE's
// 5.1 streams send; at most what sf_info_lfe()'s max_sfb holds (Part 1 Table
// 106's n_msfbl_bits).
constexpr double kLfeCutoffHz = 120.0;

// The bandwidth the SIMPLE mode codes, by bit rate per channel.
[[nodiscard]] double cutoff_hz(double kbps_per_channel) {
    if (kbps_per_channel >= 96.0) {
        return 20000.0;
    }
    if (kbps_per_channel >= 64.0) {
        return 16000.0;
    }
    if (kbps_per_channel >= 48.0) {
        return 14000.0;
    }
    return 11000.0;
}

// The first bands of a transform whose lines start below `cutoff`, at most
// `limit`.
[[nodiscard]] int bands_below(int transform_length, double cutoff, int sample_rate, int limit) {
    const std::span<const std::uint16_t> offsets = detail::band_offsets(transform_length);
    const double line_hz = static_cast<double>(sample_rate) / (2.0 * transform_length);
    int bands = 0;
    while (bands + 1 < static_cast<int>(offsets.size()) &&
           static_cast<double>(offsets[static_cast<std::size_t>(bands)]) * line_hz < cutoff) {
        ++bands;
    }
    return std::min(bands, limit);
}

[[nodiscard]] int bands_below(int transform_length, double cutoff, int sample_rate) {
    return bands_below(transform_length, cutoff, sample_rate, (1 << detail::max_sfb_bits(transform_length)) - 1);
}

// sequence_counter: 0 in the first frame (Part 1 Annex E.1), then 1 to 1020
// and round again from 1 (Part 1 clause 4.3.3.2.2).
[[nodiscard]] int sequence_counter(std::int64_t frame) {
    return frame == 0 ? 0 : static_cast<int>((frame - 1) % 1020) + 1;
}

// The codec mode a configuration codes in, CodecMode::kAuto resolved by the
// rate a channel (the LFE not counted).
[[nodiscard]] CodecMode resolve_mode(const EncoderConfig& config) {
    if (config.codec_mode != CodecMode::kAuto) {
        return config.codec_mode;
    }
    const bool lfe = config.channels == 6 || config.channels == 8;
    const int full = std::max(config.channels - (lfe ? 1 : 0), 1);
    const double kbps_per_channel = static_cast<double>(config.bitrate_kbps) / full;
    // The experimental coding configurations code all five channels.
    const bool five_x = (config.channels == 5 || config.channels == 6) && !config.experimental.coding_configs;
    if (five_x && kbps_per_channel < kAcpl3BelowKbps) {
        return CodecMode::kAspxAcpl3;
    }
    if (five_x && kbps_per_channel < kAcpl2BelowKbps) {
        return CodecMode::kAspxAcpl2;
    }
    const double aspx_below = config.channels >= 5 ? kAspxBelowKbpsMultichannel : kAspxBelowKbps;
    return kbps_per_channel < aspx_below ? CodecMode::kAspx : CodecMode::kSimple;
}

[[nodiscard]] bool is_acpl(CodecMode mode) noexcept {
    return mode == CodecMode::kAspxAcpl1 || mode == CodecMode::kAspxAcpl2 || mode == CodecMode::kAspxAcpl3;
}

// The channels the spectral frontend codes, and where they go: Part 1 Table
// 88's channel mode, each coded channel by name (its index among the coded
// channels, -1 where the mode has none), the coded channels that share a
// transform layout, A-SPX's aspx_data elements in the syntax's order with the
// channels each carries (Table 213), and the channels companding_control()
// lists, in its order (Table 212). In SIMPLE and ASPX the coded channels are
// the input's; in the A-CPL modes they are the downmixes A-CPL rebuilds the
// input's from (acpl/acpl_encoder.hpp), and the LFE.
struct Plan {
    CodecMode mode = CodecMode::kSimple;
    int ch_mode = 1;
    int l = -1;
    int r = -1;
    int c = -1;
    int lfe = -1;
    int ls = -1;
    int rs = -1;
    int x1 = -1;  // the 7.X element's additional pair
    int x2 = -1;
    std::vector<std::vector<int>> groups{};
    std::vector<std::vector<int>> aspx_elements{};
    std::vector<int> companded{};
    int coded = 0;  // the coded channels
    // The A-CPL modes: the layout, the input channels its analysis reads, in
    // the layout's order, and the input's LFE, which is coded as it is; in
    // ASPX_ACPL_1 the residuals, coded below acpl_qmf_band, which share the
    // layout group of the channels A-CPL pairs them with.
    std::optional<detail::AcplLayout> acpl{};
    std::vector<int> source{};
    int input_lfe = -1;
    std::vector<int> residuals{};

    [[nodiscard]] bool five_x() const noexcept { return ch_mode == 3 || ch_mode == 4; }
    [[nodiscard]] bool seven_x() const noexcept { return ch_mode >= 5; }
};

// An A-CPL mode: in the channel pair the coded channel is (L + R) / 2; in the
// 5.X element they are ASPX_ACPL_1's and 2's downmixes A and B and C, or
// ASPX_ACPL_3's Lo and Ro over 1 + sqrt 2, then the LFE. ASPX_ACPL_1's
// residuals come last.
[[nodiscard]] Plan plan_acpl(const EncoderConfig& config, CodecMode mode) {
    Plan p;
    p.mode = mode;
    const bool residuals = mode == CodecMode::kAspxAcpl1;
    if (config.channels == 2) {
        p.ch_mode = 1;
        p.acpl = detail::AcplLayout::kPair;
        p.source = {0, 1};
        p.l = 0;
        p.groups = {{p.l}};
        p.aspx_elements = {{p.l}};
        p.companded = {p.l};
        p.coded = 1;
        if (residuals) {
            p.residuals = {1};
            p.groups = {{p.l, 1}};
            p.coded = 2;
        }
        return p;
    }
    const bool lfe = config.channels == 6;
    p.ch_mode = lfe ? 4 : 3;
    p.source = {0, 1, 2, lfe ? 4 : 3, lfe ? 5 : 4};
    p.input_lfe = lfe ? 3 : -1;
    p.l = 0;
    p.r = 1;
    if (mode == CodecMode::kAspxAcpl3) {
        p.acpl = detail::AcplLayout::kCoupling;
        p.lfe = lfe ? 2 : -1;
        p.groups = {{p.l, p.r}};
        p.aspx_elements = {{p.l, p.r}};
        p.companded = {p.l, p.r};
    } else {
        p.acpl = detail::AcplLayout::kFiveX;
        p.c = 2;
        p.lfe = lfe ? 3 : -1;
        p.groups = {{p.l, p.r}, {p.c}};
        p.aspx_elements = {{p.l, p.r}, {p.c}};
        p.companded = {p.l, p.r, p.c};
    }
    if (lfe) {
        p.groups.push_back({p.lfe});
    }
    p.coded = p.lfe >= 0 ? p.lfe + 1 : static_cast<int>(p.companded.size());
    if (residuals) {
        p.residuals = {p.coded, p.coded + 1};
        p.groups.front().insert(p.groups.front().end(), p.residuals.begin(), p.residuals.end());
        p.coded += 2;
    }
    return p;
}

[[nodiscard]] std::expected<Plan, Refusal> plan_for(const EncoderConfig& config, CodecMode mode) {
    Plan p;
    p.mode = mode;
    p.coded = config.channels;
    const AdditionalPair pair = config.experimental.seven_x;
    const bool seven = config.channels == 7 || config.channels == 8;
    if (seven && pair == AdditionalPair::kNone) {
        return std::unexpected(
            "seven or eight channels without experimental.seven_x's additional pair");
    }
    if (!seven && pair != AdditionalPair::kNone) {
        return std::unexpected(
            "experimental.seven_x's additional pair without seven or eight channels");
    }
    if (is_acpl(mode)) {
        // ASPX_ACPL_2 and 3 in the 5.X element; with experimental.acpl,
        // ASPX_ACPL_1 there, and ASPX_ACPL_1 and 2 in the channel pair.
        const bool five = config.channels == 5 || config.channels == 6;
        const bool options = config.experimental.acpl;
        const bool fits = five ? mode != CodecMode::kAspxAcpl1 || options
                               : config.channels == 2 && mode != CodecMode::kAspxAcpl3 && options;
        if (!fits) {
            return std::unexpected(
                "an A-CPL codec mode the layout does not take: ASPX_ACPL_2 and ASPX_ACPL_3 in 5.0 "
                "and 5.1, and with experimental.acpl ASPX_ACPL_1 there and ASPX_ACPL_1 and 2 in "
                "stereo");
        }
        if (config.experimental.coding_configs) {
            return std::unexpected("an A-CPL codec mode with experimental.coding_configs");
        }
        return plan_acpl(config, mode);
    }
    switch (config.channels) {
        case 1:
            p.ch_mode = 0;
            p.c = 0;
            p.groups = {{0}};
            p.aspx_elements = {{0}};
            p.companded = {0};
            return p;
        case 2:
            p.ch_mode = 1;
            p.l = 0;
            p.r = 1;
            p.groups = {{0, 1}};
            p.aspx_elements = {{0, 1}};
            p.companded = {0, 1};
            return p;
        case 3:
            // The 3.0 element (Part 1 Table 24) at 3_0_coding_config 0: L and
            // R as a pair and C alone (clause 5.3.4.2), each with its own
            // transform layout, and A-SPX's two elements in that order.
            if (!config.experimental.three_zero) {
                return std::unexpected("three channels without experimental.three_zero");
            }
            p.ch_mode = 2;
            p.l = 0;
            p.r = 1;
            p.c = 2;
            p.groups = {{0, 1}, {2}};
            p.aspx_elements = {{0, 1}, {2}};
            p.companded = {0, 1, 2};
            return p;
        case 5:
        case 6:
        case 7:
        case 8:
            break;
        default:
            return std::unexpected(
                "a channel count the encoder does not take: 1, 2, 5 or 6, and 3, 7 or 8 as "
                "experimental layouts");
    }
    const bool lfe = config.channels % 2 == 0;
    p.l = 0;
    p.r = 1;
    p.c = 2;
    p.lfe = lfe ? 3 : -1;
    p.ls = lfe ? 4 : 3;
    p.rs = p.ls + 1;
    if (seven) {
        p.x1 = p.rs + 1;
        p.x2 = p.rs + 2;
        const int base = pair == AdditionalPair::kBack ? 5 : (pair == AdditionalPair::kWide ? 7 : 9);
        p.ch_mode = base + (lfe ? 1 : 0);
    } else {
        p.ch_mode = lfe ? 4 : 3;
    }
    if (config.experimental.coding_configs) {
        p.groups = {{p.l, p.r, p.c, p.ls, p.rs}};
    } else {
        p.groups = {{p.l, p.r}, {p.ls, p.rs}, {p.c}};
    }
    if (seven) {
        p.groups.push_back({p.x1, p.x2});
    }
    if (lfe) {
        p.groups.push_back({p.lfe});
    }
    if (!seven) {
        p.aspx_elements = {{p.l, p.r}, {p.ls, p.rs}, {p.c}};
        p.companded = {p.l, p.r, p.c, p.ls, p.rs};
    } else if (pair == AdditionalPair::kWide) {
        // 5/2/0: the wide pair second and the surrounds last.
        p.aspx_elements = {{p.l, p.r}, {p.x1, p.x2}, {p.c}, {p.ls, p.rs}};
    } else {
        p.aspx_elements = {{p.l, p.r}, {p.ls, p.rs}, {p.c}, {p.x1, p.x2}};
    }
    return p;
}

// A coding unit: one channel data element, with its one sf_info() and its
// tracks. Its outputs are the input channels its matrix gives, in the order
// of the matrix's outputs, O0 first; once its matrix is undone they hold its
// tracks, I0 first.
enum class UnitKind : std::uint8_t {
    kLfe,    // mono_data(1)
    kMono,   // mono_data(0)
    kPair,   // stereo_data() or two_channel_data(), stereo processing on
    kThree,  // three_channel_data()
    kFour,   // four_channel_data()
    kFive,   // five_channel_data()
    // The channel pair's ASPX_ACPL_1: the coded channel and its side, with one
    // sf_info() and no stereo processing.
    kMidSide,
    // The 5.X element's ASPX_ACPL_1: max_sfb_master and the residuals, each
    // with the framing of the channel it pairs with.
    kResiduals,
};

struct Unit {
    UnitKind kind = UnitKind::kMono;
    std::vector<int> outputs{};
    bool additional = false;  // the 7.X element's additional pair
    detail::UnitChoice choice{};
};

// A frame's channel data: the 5.X and 7.X elements' coding_config and
// 2ch_mode, and the units in the syntax's order, the LFE's first.
struct Structure {
    int coding_config = 0;
    bool two_ch_mode = false;
    int chel_matsel = 0;
    std::vector<Unit> units{};
};

// Part 1 Tables 25 and 33 with Tables 180 and 182: the units of a coding
// configuration, in the syntax's order, their outputs as the tables place
// them, the 7.X element's preliminary channels A to G taken as L, R, C, Ls,
// Rs and the additional pair, which they are where b_use_sap_add_ch is 0.
[[nodiscard]] Structure structure_for(const Plan& p, int coding_config, bool two_ch_mode, int chel_matsel) {
    Structure s;
    s.coding_config = coding_config;
    s.two_ch_mode = two_ch_mode;
    s.chel_matsel = chel_matsel;
    if (p.acpl) {
        // Table 22: the channel pair's coded channel alone, or with its side.
        // Table 25: the LFE, the downmixes as two_channel_data(), ASPX_ACPL_1's
        // residuals and C's mono_data(), or ASPX_ACPL_3's as stereo_data().
        if (*p.acpl == detail::AcplLayout::kPair) {
            if (p.residuals.empty()) {
                s.units.push_back({.kind = UnitKind::kMono, .outputs = {p.l}});
            } else {
                s.units.push_back({.kind = UnitKind::kMidSide,
                                   .outputs = {p.l, p.residuals[0]},
                                   .choice = {.sets = {detail::StereoChoice{}}}});
            }
            return s;
        }
        if (p.lfe >= 0) {
            s.units.push_back({.kind = UnitKind::kLfe, .outputs = {p.lfe}});
        }
        s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.l, p.r}});
        if (!p.residuals.empty()) {
            s.units.push_back({.kind = UnitKind::kResiduals,
                               .outputs = p.residuals,
                               .choice = {.sets = {detail::StereoChoice{}, detail::StereoChoice{}}}});
        }
        if (p.c >= 0) {
            s.units.push_back({.kind = UnitKind::kMono, .outputs = {p.c}});
        }
        return s;
    }
    if (p.ch_mode == 0) {
        s.units.push_back({.kind = UnitKind::kMono, .outputs = {p.c}});
        return s;
    }
    if (p.ch_mode == 1) {
        s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.l, p.r}});
        return s;
    }
    if (p.ch_mode == 2) {
        s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.l, p.r}});
        s.units.push_back({.kind = UnitKind::kMono, .outputs = {p.c}});
        return s;
    }
    if (p.lfe >= 0) {
        s.units.push_back({.kind = UnitKind::kLfe, .outputs = {p.lfe}});
    }
    bool centre_last = false;
    switch (coding_config) {
        case 0:
            if (two_ch_mode) {
                s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.l, p.ls}});
                s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.r, p.rs}});
            } else {
                s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.l, p.r}});
                s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.ls, p.rs}});
            }
            centre_last = true;
            break;
        case 1:
            s.units.push_back({.kind = UnitKind::kThree, .outputs = {p.l, p.r, p.c}});
            s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.ls, p.rs}});
            break;
        case 2:
            s.units.push_back({.kind = UnitKind::kFour, .outputs = {p.l, p.r, p.ls, p.rs}});
            centre_last = true;
            break;
        default:
            s.units.push_back({.kind = UnitKind::kFive, .outputs = {p.l, p.r, p.c, p.ls, p.rs}});
            break;
    }
    if (p.seven_x()) {
        s.units.push_back({.kind = UnitKind::kPair, .outputs = {p.x1, p.x2}, .additional = true});
    }
    if (centre_last) {
        s.units.push_back({.kind = UnitKind::kMono, .outputs = {p.c}});
    }
    return s;
}

// The coding configurations the experimental option weighs: coding_config 0
// with either 2ch_mode, 1 with each chel_matsel, 2, and 3 with each
// chel_matsel.
struct Candidate {
    int coding_config = 0;
    bool two_ch_mode = false;
    int chel_matsel = 0;
};

[[nodiscard]] std::vector<Candidate> candidates() {
    std::vector<Candidate> out = {{0, false, 0}, {0, true, 0}, {2, false, 0}};
    for (int m = 0; m < 12; ++m) {
        out.push_back({1, false, m});
        out.push_back({3, false, m});
    }
    return out;
}

}  // namespace

// One channel-coded substream (Part 2 clause 6.2.2.2): its input at the
// internal rate, its analysis and coding tools, its dialogue enhancement, and
// the rate loop that codes a frame's channel element into the bytes the frame
// gives its substream. Encoder::Impl, the stream, holds one for each of its
// substreams and makes each frame of them: prepare() analyses the frame,
// code() fits it to its bytes, commit() keeps what the next frame codes
// against.
struct SubstreamCoder {
    // A coder in `mode` for `config`, which describes the substream alone (its
    // channels, codec mode, share of the rate and dialogue enhancement), or
    // why the configuration is not one the encoder writes in that mode.
    // Without `converts` the input arrives at the internal rate already, as a
    // dialogue enhancement substream's does.
    [[nodiscard]] static std::expected<std::unique_ptr<SubstreamCoder>, Refusal> make(
        const EncoderConfig& config, CodecMode mode, bool converts);

    EncoderConfig config{};
    Plan plan{};
    // The frame grid, and from it the frame's length, the silence ahead of
    // the input (a frame and a half), and transient detection's sub-block.
    detail::FrameTiming timing{};
    int frame_length = 2048;
    int rate_hz =
        48000;  // the internal rate, which lines and subbands are measured at, to the hertz
    int delay = 3072;
    int sub_block = 128;
    // At every frame_rate_index but 13, the input converted to the internal
    // rate, a converter per channel and per channel of a dialogue stem.
    std::vector<detail::dsp::Resampler<double>> converters;
    std::vector<detail::dsp::Resampler<double>> stem_converters;
    detail::Analysis analysis{2048, 1};
    detail::Psychoacoustics psycho{48000, 2048};
    double cutoff = 20000.0;

    // The input at the internal rate, from sample index `base` of the delayed
    // signal on; the first `delay` samples of that signal are the silence
    // ahead of the input.
    std::vector<std::vector<double>> signal;
    std::int64_t base = 0;

    // The metadata beside the audio (frame/metadata.hpp). With dialogue
    // enhancement: the input channels its parameters are for, in
    // de_channel_config's order; with a stem, those channels and the dialogue
    // in them on the signal's axis; and the parameters of the frame being
    // coded and of the last one sent, which the next codes against.
    detail::StreamMetadata metadata;
    std::vector<std::size_t> de_channels;
    std::vector<std::vector<double>> de_programme;
    std::vector<std::vector<double>> de_dialogue;
    detail::DeFrameParameters de_current{};
    detail::DeFrameParameters de_previous{};
    bool de_sent = false;
    // A dialogue substream's mixing values (b_dialog), and the EMDF payloads
    // its metadata() carries.
    std::optional<detail::DialogueMixCodes> dialogue_mix;
    std::vector<detail::EmdfPayloadCodes> emdf;

    [[nodiscard]] bool stem() const noexcept {
        return config.dialogue && config.dialogue->source == DialogueSource::kStem;
    }

    // The input as it arrived, at the internal rate on the signal's axis,
    // where a presentation this substream leads sends DRC gains computed from
    // it (frame/drc_gains.hpp).
    std::vector<std::vector<double>> kept_input;

    [[nodiscard]] double kept_sample(std::size_t c, std::int64_t s) const noexcept {
        if (s < base || s >= signal_end()) {
            return 0.0;
        }
        return kept_input[c][static_cast<std::size_t>(s - base)];
    }

    // Each layout group's transform layouts, decided for the frame being coded
    // and the frame after, and the length of the last window of the frame
    // before.
    struct Group {
        std::vector<int> channels;
        bool lfe = false;  // sf_info_lfe(): always one long block
        std::deque<FrameLayout> layouts;
        int previous_last = 2048;  // the frame's length at first
    };
    std::vector<Group> groups;
    std::vector<std::size_t> group_of;  // per input channel

    // The ASPX mode: the stream's A-SPX configuration, and the QMF domain of
    // each channel A-SPX codes (all but the LFE), with qmf_of mapping an input
    // channel to its own, or -1. With interleaving, each channel's QMF
    // subbands the last frame's A-SPX data had the spectral frontend code.
    std::optional<detail::AspxSetup> aspx;
    // The A-CPL modes: the analysis and parameters, and the input channels it
    // reads (Plan::source), on the signal's axis.
    std::optional<detail::AcplEncoder> acpl;
    std::vector<std::vector<double>> source;
    std::vector<detail::AspxChannelEncoder> qmf;
    std::vector<int> qmf_of;
    std::vector<int> qmf_channel;
    std::vector<std::vector<std::pair<int, int>>> interleaved_prev;

    // A frame's A-SPX data: companding_control()'s fields and each aspx_data
    // element's, in the syntax's order.
    struct AspxFrame {
        detail::CompandingFields companding;
        std::vector<detail::AspxElement> elements;
    };

    // What a frame's channel element is written from.
    struct Coding {
        bool iframe = false;
        Structure structure;
        std::vector<FrameLayout> layout;           // per group
        std::vector<std::array<int, 2>> max_sfb;   // per group
        std::vector<detail::CodedTrack> tracks;    // per coded channel: the track its unit leaves there
        std::optional<AspxFrame> aspx;
        std::optional<detail::AcplFrameFields> acpl;
        // ASPX_ACPL_1: the residuals' max_sfb per half, and in the 5.X element
        // the max_sfb_master they follow from.
        std::array<int, 2> residual_max_sfb{};
        int residual_master = 0;
    };

    [[nodiscard]] bool residual(std::size_t c) const noexcept {
        return std::ranges::find(plan.residuals, static_cast<int>(c)) != plan.residuals.end();
    }

    // A coded channel's max_sfb per half: its layout group's, or a residual's.
    [[nodiscard]] std::array<int, 2> max_sfb_of(const Coding& f, std::size_t c) const {
        return residual(c) ? f.residual_max_sfb : f.max_sfb[group_of[c]];
    }

    // ASPX_ACPL_1: the residuals' bands for the frame's layout, those below
    // acpl_qmf_band. The channel pair's side sends its own max_sfb_side; the
    // 5.X element's residuals take max_sfb_master, in n_side_bits of the
    // largest transform length, as a window of that length's max_sfb and
    // Tables B.8 to B.19's value for it at a shorter one (clause 4.3.5.13).
    void limit_residuals(Coding& f) const {
        if (plan.residuals.empty()) {
            return;
        }
        const FrameLayout& layout = f.layout[group_of[static_cast<std::size_t>(plan.residuals.front())]];
        const double top = kAcplResidualQmfBand * static_cast<double>(rate_hz) / 128.0;
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        if (*plan.acpl == detail::AcplLayout::kPair) {
            f.residual_max_sfb = {bands_below(first, top, rate_hz),
                                  bands_below(last, top, rate_hz)};
            return;
        }
        const int largest = std::max(first, last);
        f.residual_master =
            bands_below(largest, top, rate_hz, (1 << detail::side_bits(largest)) - 1);
        const auto from_master = [&](int length) {
            if (length == largest) {
                return f.residual_master;
            }
            return std::max(detail::tables::max_sfb_from_master(largest, f.residual_master, length), 0);
        };
        f.residual_max_sfb = {from_master(first), from_master(last)};
    }

    // With interleaving, the bands above the crossover the spectral frontend
    // leaves silent: all but those that meet `waveform_hz`, the frequency
    // ranges it codes there.
    [[nodiscard]] std::vector<std::vector<bool>> silenced_bands(
        const detail::Grouped& grouped, const FrameLayout& layout,
        const std::vector<std::pair<double, double>>& waveform_hz) const {
        std::vector<std::vector<bool>> out(grouped.offset.size());
        const double crossover =
            aspx ? aspx->groups.sbx * static_cast<double>(rate_hz) / 128.0 : 0.0;
        for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
            const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
            out[g].assign(bands, false);
            if (waveform_hz.empty()) {
                continue;
            }
            const int length = layout.group_length[g];
            const std::span<const std::uint16_t> offsets = detail::band_offsets(length);
            const double line_hz = static_cast<double>(rate_hz) / (2.0 * length);
            for (std::size_t b = 0; b < bands; ++b) {
                const double lo = offsets[b] * line_hz;
                const double hi = offsets[b + 1] * line_hz;
                const bool coded = std::ranges::any_of(
                    waveform_hz, [&](const std::pair<double, double>& range) { return lo < range.second && hi > range.first; });
                out[g][b] = lo >= crossover && !coded;
            }
        }
        return out;
    }

    [[nodiscard]] std::int64_t signal_end() const noexcept {
        return base + static_cast<std::int64_t>(signal.front().size());
    }

    [[nodiscard]] double sample(std::size_t c, std::int64_t s) const noexcept {
        if (s < base || s >= signal_end()) {
            return 0.0;
        }
        return signal[c][static_cast<std::size_t>(s - base)];
    }

    // Sample s of the input channel A-CPL's analysis reads k-th.
    [[nodiscard]] double source_sample(std::size_t k, std::int64_t s) const noexcept {
        if (s < base || s >= signal_end()) {
            return 0.0;
        }
        return source[k][static_cast<std::size_t>(s - base)];
    }

    // Analyses the QMF slots of the input channels A-CPL rebuilds that frame
    // f's parameters read.
    void analyse_acpl(std::int64_t frame) {
        std::vector<std::array<double, kQmfSlot>> chunk(acpl->channels());
        while (acpl->slots() < acpl->slots_needed(frame)) {
            const std::int64_t from = kQmfSlot * acpl->slots() - timing.alignment_delay;
            for (std::size_t k = 0; k < chunk.size(); ++k) {
                for (std::size_t i = 0; i < chunk[k].size(); ++i) {
                    chunk[k][i] = source_sample(k, from + static_cast<std::int64_t>(i));
                }
            }
            acpl->push_slot(chunk);
        }
    }

    // What the spectral frontend codes: the signal, or with companding its
    // compressed low band.
    [[nodiscard]] double coded_sample(std::size_t c, std::int64_t s) const noexcept {
        if (aspx && aspx->companding && qmf_of[c] >= 0) {
            return qmf[static_cast<std::size_t>(qmf_of[c])].companded(s);
        }
        return sample(c, s);
    }

    // The QMF slots frame f needs analysed: its interval's and the
    // ts_offset_hfgen after it that a variable border can reach, and with
    // companding those whose synthesis reaches the end of its transform
    // window.
    [[nodiscard]] std::int64_t qmf_slots_needed(std::int64_t frame) const noexcept {
        std::int64_t end =
            static_cast<std::int64_t>(timing.qmf_slots) * (frame + timing.control_delay + 1);
        if (aspx->companding) {
            const std::int64_t window_last = (frame + 2) * frame_length - 1;
            end = std::max(end, (window_last + timing.alignment_delay + 577) / kQmfSlot + 1);
        }
        return end;
    }

    void analyse_qmf(std::int64_t frame) {
        const std::int64_t end = qmf_slots_needed(frame);
        std::array<double, kQmfSlot> chunk{};
        for (std::size_t q = 0; q < qmf.size(); ++q) {
            const auto c = static_cast<std::size_t>(qmf_channel[q]);
            while (qmf[q].slots() < end) {
                const std::int64_t from = kQmfSlot * qmf[q].slots() - timing.alignment_delay;
                for (std::size_t i = 0; i < chunk.size(); ++i) {
                    chunk[i] = sample(c, from + static_cast<std::int64_t>(i));
                }
                qmf[q].push_slot(chunk);
            }
        }
    }

    // A frame's A-SPX data before the rate loop: each channel's proposal,
    // with companding as the stream has it; or with `fallback`, what costs
    // least (AspxChannelEncoder::fallback()), its interval from `start`
    // where that is given.
    [[nodiscard]] AspxFrame aspx_frame(std::int64_t frame, bool iframe,
                                       std::optional<bool> fallback,
                                       std::optional<int> start = std::nullopt) {
        AspxFrame out;
        out.companding.num_chan = static_cast<int>(plan.companded.size());
        for (std::size_t i = 0; i < plan.companded.size(); ++i) {
            out.companding.compand_on[i] = aspx->companding;
        }
        for (const std::vector<int>& channels : plan.aspx_elements) {
            detail::AspxElement element;
            element.companding = out.companding;
            for (const int c : channels) {
                const auto q = static_cast<std::size_t>(qmf_of[static_cast<std::size_t>(c)]);
                element.channels.push_back(fallback ? qmf[q].fallback(iframe, *fallback, start)
                                                    : qmf[q].propose(frame, iframe));
            }
            if (!fallback && channels.size() == 2 && aspx->balance) {
                const auto q0 = static_cast<std::size_t>(qmf_of[static_cast<std::size_t>(channels[0])]);
                const auto q1 = static_cast<std::size_t>(qmf_of[static_cast<std::size_t>(channels[1])]);
                const auto pair =
                    qmf[q0].balanced_with(qmf[q1], {element.channels[0], element.channels[1]}, iframe);
                if (pair) {
                    element.channels = {(*pair)[0], (*pair)[1]};
                    element.balance = true;
                }
            }
            out.elements.push_back(std::move(element));
        }
        return out;
    }

    // Transient detection over the frame's centre, where its blocks are:
    // the first difference's energy per sub-block against the four before,
    // summed over the group's channels.
    [[nodiscard]] FrameLayout decide(std::int64_t frame, const Group& group) const {
        if (group.lfe) {
            return detail::long_layout(frame_length);
        }
        const std::int64_t centre = frame * frame_length + frame_length / 2;
        std::array<double, kSubBlocks + 4> energy{};
        for (int k = -4; k < kSubBlocks; ++k) {
            double e = 0.0;
            for (const int channel : group.channels) {
                const auto c = static_cast<std::size_t>(channel);
                const std::int64_t start = centre + static_cast<std::int64_t>(k) * sub_block;
                for (std::int64_t s = start; s < start + sub_block; ++s) {
                    const double d = sample(c, s) - sample(c, s - 1);
                    e += d * d;
                }
            }
            energy[static_cast<std::size_t>(k + 4)] = e;
        }
        std::array<int, 2> attack{-1, -1};
        int first_attack = -1;
        const double quietest =
            kAttackFloor * sub_block * static_cast<double>(group.channels.size());
        for (int k = 0; k < kSubBlocks; ++k) {
            const auto i = static_cast<std::size_t>(k + 4);
            const double before = (energy[i - 1] + energy[i - 2] + energy[i - 3] + energy[i - 4]) / 4.0;
            if (energy[i] > quietest && energy[i] > kAttackRatio * std::max(before, quietest)) {
                const auto half = static_cast<std::size_t>(k / (kSubBlocks / 2));
                if (attack[half] < 0) {
                    attack[half] = k % (kSubBlocks / 2);
                }
                if (first_attack < 0) {
                    first_attack = k;
                }
            }
        }
        if (first_attack < 0) {
            return detail::long_layout(frame_length);
        }
        if (!timing.long_family()) {
            // Below 1 536 samples the whole frame splits, into its shortest
            // blocks: eight, or four at 512 and 384 samples.
            const int windows = 1 << detail::whole_frame_index(frame_length);
            return detail::short_layout(frame_length, 0, first_attack * windows / kSubBlocks);
        }
        // An attack's half splits into eight blocks; the other half stays one
        // block of half the frame.
        const std::array<int, 2> transf_length{attack[0] >= 0 ? 0 : 3, attack[1] >= 0 ? 0 : 3};
        return detail::split_layout(frame_length, transf_length, attack);
    }

    [[nodiscard]] std::array<int, 2> max_sfb_for(const FrameLayout& layout, const Group& group) const {
        if (group.lfe) {
            const int bands = bands_below(frame_length, kLfeCutoffHz, rate_hz,
                                          (1 << detail::lfe_max_sfb_bits(frame_length)) - 1);
            return {bands, bands};
        }
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        return {bands_below(first, cutoff, rate_hz), bands_below(last, cutoff, rate_hz)};
    }

    // What the audio substream's metadata() carries in a frame: its dialogue
    // enhancement with the frame's parameters and the last sent, a dialogue
    // substream's mixing values and the EMDF payloads.
    [[nodiscard]] detail::AudioSubstreamFields fields_for(bool iframe) const {
        detail::AudioSubstreamFields fields;
        fields.ch_mode = plan.ch_mode;
        fields.iframe = iframe;
        fields.de_config = metadata.de ? &*metadata.de : nullptr;
        fields.de = &de_current;
        fields.de_previous = de_sent ? &de_previous : nullptr;
        fields.dialogue = dialogue_mix ? &*dialogue_mix : nullptr;
        fields.emdf = emdf;
        return fields;
    }

    // Where frame f's dialogue enhancement parameters are estimated from: a
    // long block's window, two frames of the signal, centred where the
    // decoder's interpolation reaches them. They reach the QMF domain d_ctrl
    // frames on (frame/timing.hpp) and apply to the block the output stages
    // work on then, ts_offset_hfgen slots behind the analysis: the signal from
    // frame_length (f + d_ctrl) - 64 ts_offset_hfgen - d_pcm, reaching their
    // full value at its end.
    [[nodiscard]] std::int64_t dialogue_window(std::int64_t frame) const noexcept {
        return frame_length * (frame + timing.control_delay) - kQmfSlot * timing.hfgen_slots -
               timing.alignment_delay;
    }

    // Dialogue enhancement's parameters for frame f from the stem: the
    // long-block spectra of each channel and of its dialogue over
    // dialogue_window().
    void estimate_dialogue(std::int64_t frame) {
        const std::int64_t start = dialogue_window(frame);
        const FrameLayout layout = detail::long_layout(frame_length);
        std::vector<double> window(2 * static_cast<std::size_t>(frame_length));
        std::vector<std::vector<double>> programme(de_channels.size());
        std::vector<std::vector<double>> dialogue(de_channels.size());
        for (std::size_t i = 0; i < de_channels.size(); ++i) {
            for (const auto& [from, to] : {std::pair{&de_programme[i], &programme[i]},
                                           std::pair{&de_dialogue[i], &dialogue[i]}}) {
                for (std::size_t n = 0; n < window.size(); ++n) {
                    const std::int64_t s = start + static_cast<std::int64_t>(n);
                    window[n] = s >= base && s < signal_end()
                                    ? (*from)[static_cast<std::size_t>(s - base)]
                                    : 0.0;
                }
                analysis.transform(window, layout, frame_length, frame_length, *to);
            }
        }
        switch (config.dialogue->method) {
            case DialogueMethod::kChannelIndependent:
                for (std::size_t i = 0; i < de_channels.size(); ++i) {
                    de_current.par[i] =
                        detail::de_parameters(programme[i], dialogue[i], frame_length);
                }
                break;
            case DialogueMethod::kMid: {
                // L and R's Mids, (L + R) / 2, the transform being linear.
                std::vector<double> mid(programme[0].size());
                std::vector<double> dialogue_mid(mid.size());
                for (std::size_t k = 0; k < mid.size(); ++k) {
                    mid[k] = 0.5 * (programme[0][k] + programme[1][k]);
                    dialogue_mid[k] = 0.5 * (dialogue[0][k] + dialogue[1][k]);
                }
                de_current.par[0] = detail::de_parameters(mid, dialogue_mid, frame_length);
                break;
            }
            case DialogueMethod::kCrossChannel:
                de_current = detail::de_cross_parameters(programme, dialogue, frame_length);
                break;
        }
    }
    // Undoes each unit's matrix: `spectra`, per input channel, then holds each
    // unit's tracks where its outputs were.
    static void undo(Structure& s, std::vector<detail::Channel>& spectra) {
        for (Unit& unit : s.units) {
            const auto at = [&](std::size_t k) { return &spectra[static_cast<std::size_t>(unit.outputs[k])]; };
            switch (unit.kind) {
                case UnitKind::kPair:
                    unit.choice = detail::undo_pair({at(0), at(1)});
                    break;
                case UnitKind::kThree:
                    unit.choice = detail::undo_three(s.chel_matsel, {at(0), at(1), at(2)});
                    break;
                case UnitKind::kFour:
                    unit.choice = detail::undo_four({at(0), at(1), at(2), at(3)});
                    break;
                case UnitKind::kFive:
                    unit.choice = detail::undo_five(s.chel_matsel, {at(0), at(1), at(2), at(3), at(4)});
                    break;
                case UnitKind::kLfe:
                case UnitKind::kMono:
                    unit.choice.bits = detail::perceptual_entropy(at(0)->grouped, at(0)->allowed);
                    break;
                case UnitKind::kMidSide:
                case UnitKind::kResiduals:
                    unit.choice.bits = detail::perceptual_entropy(at(0)->grouped, at(0)->allowed) +
                                       detail::perceptual_entropy(at(1)->grouped, at(1)->allowed);
                    break;
            }
        }
    }

    // The experimental coding configurations: each candidate's matrices
    // undone on a copy of the five channels, and the one whose tracks and side
    // information cost fewest bits kept, its tracks put back in `spectra`. The
    // five share one layout group, so every candidate's sf_info()s are alike.
    [[nodiscard]] Structure choose_structure(std::vector<detail::Channel>& spectra, const FrameLayout& layout,
                                             std::array<int, 2> max_sfb) const {
        const std::array<int, 5> five = {plan.l, plan.r, plan.c, plan.ls, plan.rs};
        const double sf_info = static_cast<double>(detail::sf_info_bits(layout, max_sfb));
        std::optional<Structure> best;
        double best_bits = std::numeric_limits<double>::max();
        std::vector<detail::Channel> best_spectra;
        std::vector<detail::Channel> trial = spectra;
        for (const Candidate& candidate : candidates()) {
            for (const int c : five) {
                trial[static_cast<std::size_t>(c)] = spectra[static_cast<std::size_t>(c)];
            }
            Structure s = structure_for(plan, candidate.coding_config, candidate.two_ch_mode, candidate.chel_matsel);
            std::erase_if(s.units, [](const Unit& u) { return u.kind == UnitKind::kLfe || u.additional; });
            undo(s, trial);
            // coding_config, 2ch_mode, and each unit's sf_info() with a pair's
            // b_enable_mdct_stereo_proc and a mono_data()'s spec_frontend.
            double bits = 2.0 + (candidate.coding_config == 0 ? 1.0 : 0.0);
            for (const Unit& unit : s.units) {
                bits += unit.choice.bits + sf_info;
                if (unit.kind == UnitKind::kPair || unit.kind == UnitKind::kMono) {
                    bits += 1.0;
                }
            }
            if (bits < best_bits) {
                best_bits = bits;
                best = structure_for(plan, candidate.coding_config, candidate.two_ch_mode, candidate.chel_matsel);
                for (Unit& unit : best->units) {
                    const auto same = std::ranges::find_if(s.units, [&](const Unit& u) { return u.outputs == unit.outputs; });
                    if (same != s.units.end()) {
                        unit.choice = same->choice;
                    }
                }
                best_spectra = trial;
            }
        }
        for (const int c : five) {
            spectra[static_cast<std::size_t>(c)] = std::move(best_spectra[static_cast<std::size_t>(c)]);
        }
        // The LFE and the additional pair, which every candidate shares.
        for (Unit& unit : best->units) {
            if (unit.kind == UnitKind::kLfe || unit.additional) {
                Structure one;
                one.units.push_back(unit);
                undo(one, spectra);
                unit.choice = one.units.front().choice;
            }
        }
        return *best;
    }

    // One unit: its channel data element, and without `data` all of it but
    // the sf_data() elements.
    void write_unit(BitWriter& w, const Unit& unit, const Coding& f, bool data) const {
        const std::size_t group = group_of[static_cast<std::size_t>(unit.outputs.front())];
        const FrameLayout& layout = f.layout[group];
        const std::array<int, 2> max_sfb = f.max_sfb[group];
        switch (unit.kind) {
            case UnitKind::kLfe:
                // sf_info_lfe() (Table 35): one long block, max_sfb alone.
                w.write(static_cast<unsigned>(detail::lfe_max_sfb_bits(frame_length)),
                        static_cast<std::uint64_t>(max_sfb[0]), "max_sfb");
                break;
            case UnitKind::kMono:
                // Table 21, mono_data(0) with the ASF.
                w.write(1, 0, "spec_frontend");
                detail::write_sf_info(w, layout, max_sfb);
                break;
            case UnitKind::kPair:
                // Table 23's stereo_data() and Table 26's two_channel_data():
                // one sf_info() for both tracks.
                w.write(1, 1, "b_enable_mdct_stereo_proc");
                detail::write_sf_info(w, layout, max_sfb);
                detail::write_chparam_info(w, unit.choice.sets.at(0));
                break;
            case UnitKind::kThree:
            case UnitKind::kFive:
                // Tables 27 and 29, with three_channel_info() and
                // five_channel_info() (Tables 30 and 32).
                detail::write_sf_info(w, layout, max_sfb);
                w.write(4, static_cast<std::uint64_t>(unit.choice.chel_matsel), "chel_matsel");
                for (const detail::StereoChoice& set : unit.choice.sets) {
                    detail::write_chparam_info(w, set);
                }
                break;
            case UnitKind::kFour:
                // Table 28, with four_channel_info() (Table 31).
                detail::write_sf_info(w, layout, max_sfb);
                for (const detail::StereoChoice& set : unit.choice.sets) {
                    detail::write_chparam_info(w, set);
                }
                break;
            case UnitKind::kMidSide:
                // Table 22's ASPX_ACPL_1 with b_enable_mdct_stereo_proc: one
                // sf_info() with the side's max_sfb_side after each max_sfb,
                // and chparam_info() at sap_mode 0.
                w.write(1, 1, "b_enable_mdct_stereo_proc");
                detail::write_sf_info_dual(w, layout, max_sfb, f.residual_max_sfb);
                detail::write_chparam_info(w, unit.choice.sets.at(0));
                break;
            case UnitKind::kResiduals: {
                // Table 25's ASPX_ACPL_1: max_sfb_master, and each residual's
                // chparam_info() at sap_mode 0, which leaves it as it is.
                const int largest = std::max(layout.window_length.front(), layout.window_length.back());
                w.write(static_cast<unsigned>(detail::side_bits(largest)),
                        static_cast<std::uint64_t>(f.residual_master), "max_sfb_master");
                for (const detail::StereoChoice& set : unit.choice.sets) {
                    detail::write_chparam_info(w, set);
                }
                break;
            }
        }
        if (data) {
            for (const int c : unit.outputs) {
                detail::write_sf_data(w, f.tracks[static_cast<std::size_t>(c)], layout);
            }
        }
    }

    // Part 1 Tables 20, 22, 25 and 33: the channel element, audio_data_chan()
    // of the substream. Without `data`, all of it but the sf_data() elements:
    // the side information the rate loop's budget leaves out.
    void write_element(BitWriter& w, const Coding& f, bool data) const {
        const bool with_aspx = f.aspx.has_value();
        const auto units = [&](const auto& pick) {
            for (const Unit& unit : f.structure.units) {
                if (pick(unit)) {
                    write_unit(w, unit, f, data);
                }
            }
        };
        const auto tails = [&]() {
            if (with_aspx) {
                for (const detail::AspxElement& element : f.aspx->elements) {
                    detail::write_aspx_tail(w, f.iframe, *aspx, element);
                }
            }
        };
        if (plan.acpl) {
            write_acpl_element(w, f, data);
            return;
        }
        if (plan.ch_mode <= 1) {
            // Tables 20 and 22, single_channel_element() and
            // channel_pair_element(): aspx_config() (in an I-frame) and
            // companding_control() before the channel data, aspx_data after.
            if (plan.ch_mode == 1) {
                w.write(2, with_aspx ? 1U : 0U, "stereo_codec_mode");
            } else {
                w.write(1, with_aspx ? 1U : 0U, "mono_codec_mode");
            }
            if (with_aspx) {
                detail::write_aspx_head(w, f.iframe, *aspx, f.aspx->elements.front());
            }
            units([](const Unit&) { return true; });
            tails();
            return;
        }
        if (plan.ch_mode == 2) {
            // Table 24, 3_0_channel_element(): aspx_config() in an I-frame and
            // companding_control(3) before the channel data, which
            // 3_0_coding_config 0 sends as stereo_data() and mono_data(0),
            // and aspx_data_2ch() and aspx_data_1ch() after it.
            w.write(1, with_aspx ? 1U : 0U, "3_0_codec_mode");
            if (with_aspx) {
                if (f.iframe) {
                    detail::write_aspx_config(w, aspx->config);
                }
                detail::write_companding_control(w, f.aspx->companding);
            }
            w.write(1, 0, "3_0_coding_config");
            units([](const Unit&) { return true; });
            tails();
            return;
        }
        const bool five = plan.five_x();
        if (five) {
            w.write(3, with_aspx ? 1U : 0U, "5_X_codec_mode");
        } else {
            w.write(2, with_aspx ? 1U : 0U, "7_X_codec_mode");
        }
        if (with_aspx && f.iframe) {
            detail::write_aspx_config(w, aspx->config);
        }
        units([](const Unit& u) { return u.kind == UnitKind::kLfe; });
        if (with_aspx && five) {
            detail::write_companding_control(w, f.aspx->companding);
        }
        w.write(2, static_cast<std::uint64_t>(f.structure.coding_config), "coding_config");
        if (f.structure.coding_config == 0) {
            w.write(1, f.structure.two_ch_mode ? 1U : 0U, "2ch_mode");
        }
        // The coding configuration's units; in 7.X the additional pair, and
        // then C's mono_data() where coding_config 0 and 2 send one.
        const auto additional = std::ranges::find_if(f.structure.units, [](const Unit& u) { return u.additional; });
        for (auto it = f.structure.units.begin(); it != f.structure.units.end(); ++it) {
            if (it->kind == UnitKind::kLfe) {
                continue;
            }
            if (it == additional) {
                w.write(1, 0, "b_use_sap_add_ch");
            }
            write_unit(w, *it, f, data);
        }
        tails();
    }

    // Part 1 Tables 22 and 25 in the A-CPL modes: aspx_config() and
    // acpl_config_1ch() or acpl_config_2ch() in an I-frame, the LFE,
    // companding_control(), the coded channels' data, the aspx_data elements
    // and the A-CPL data.
    void write_acpl_element(BitWriter& w, const Coding& f, bool data) const {
        const bool coupling = *plan.acpl == detail::AcplLayout::kCoupling;
        const bool residuals = plan.mode == CodecMode::kAspxAcpl1;
        if (*plan.acpl == detail::AcplLayout::kPair) {
            w.write(2, residuals ? 2U : 3U, "stereo_codec_mode");
        } else {
            w.write(3, residuals ? 2U : (coupling ? 4U : 3U), "5_X_codec_mode");
        }
        if (f.iframe) {
            detail::write_aspx_config(w, aspx->config);
            if (coupling) {
                detail::write_acpl_config_2ch(w, acpl->config_2ch());
            } else {
                detail::write_acpl_config_1ch(w, acpl->config_1ch());
            }
        }
        for (const Unit& unit : f.structure.units) {
            if (unit.kind == UnitKind::kLfe) {
                write_unit(w, unit, f, data);
            }
        }
        detail::write_companding_control(w, f.aspx->companding);
        if (*plan.acpl == detail::AcplLayout::kFiveX) {
            w.write(1, 0, "coding_config");
        }
        for (const Unit& unit : f.structure.units) {
            if (unit.kind != UnitKind::kLfe) {
                write_unit(w, unit, f, data);
            }
        }
        for (const detail::AspxElement& element : f.aspx->elements) {
            detail::write_aspx_tail(w, f.iframe, *aspx, element);
        }
        if (coupling) {
            detail::write_acpl_data_2ch(w, acpl->config_2ch(), f.acpl->coupling);
            return;
        }
        // The channel pair's one module, or the 5.X element's two.
        const std::size_t modules = *plan.acpl == detail::AcplLayout::kPair ? 1 : 2;
        for (std::size_t m = 0; m < modules; ++m) {
            detail::write_acpl_data_1ch(w, acpl->config_1ch(), f.acpl->modules[m]);
        }
    }

    // The frame's channel element with no bands, sap_mode 0 in every
    // chparam_info() and coding_config 0: what a frame falls back to when no
    // step of the rate loop fits it, and what create() checks the rate holds.
    [[nodiscard]] Coding silent(bool iframe, const std::vector<FrameLayout>& layout) const {
        Coding f;
        f.iframe = iframe;
        f.structure = structure_for(plan, 0, false, 0);
        for (Unit& unit : f.structure.units) {
            if (unit.kind == UnitKind::kPair) {
                unit.choice.sets = {detail::StereoChoice{}};
            }
        }
        f.layout = layout;
        f.max_sfb.assign(groups.size(), {0, 0});
        f.tracks.resize(signal.size());
        for (std::size_t c = 0; c < signal.size(); ++c) {
            const FrameLayout& l = layout[group_of[c]];
            const detail::Grouped grouped = detail::regroup({}, l, {0, 0});
            f.tracks[c] = detail::code_track(grouped, std::vector<std::vector<int>>(grouped.offset.size()), 0, l);
        }
        return f;
    }

    // What code() lowers, in turn, when not even a frame with no bands fits.
    enum class Fallback : std::uint8_t { kProposed, kHeld, kLeast, kLeastMetadata };

    // The frame prepare() analysed, which code() fits to its bytes: its coding,
    // the spectra and allowances the rate loop brings to a level, the tracks
    // in the syntax's order, and the side information the rate loop's budget
    // leaves out. As prepared, since code() may be run again with more bytes.
    struct Pending {
        std::int64_t frame = 0;
        detail::AudioSubstreamFields fields{};
        Coding prepared{};
        Coding f{};
        detail::DeFrameParameters de_prepared{};
        std::vector<detail::Channel> spectra;
        std::vector<std::vector<std::vector<bool>>> silenced;
        std::vector<std::vector<std::vector<double>>> energy;
        std::vector<std::size_t> order;
        std::size_t side_bits = 0;
        double top_level = 0.0;  // the highest allowance per line
        std::vector<std::vector<std::vector<int>>> sf;
        std::vector<std::vector<double>> capped;
        double kappa = kCaps.front();
    };
    Pending pending;

    // Frame f's analysis, up to the rate loop: its transform layouts are the
    // first of each group's (before_frame()).
    void prepare(std::int64_t frame, bool iframe) {
        Pending& p = pending;
        const std::size_t channels = signal.size();
        const std::int64_t start = frame * frame_length;
        if (stem()) {
            estimate_dialogue(frame);
        }
        if (metadata.de) {
            de_current.signal_contribution = metadata.de->signal_contribution;
        }
        p.frame = frame;
        p.fields = fields_for(iframe);
        p.de_prepared = de_current;
        Coding& f = p.f;
        f = Coding{};
        f.iframe = iframe;
        std::vector<int> next_first(groups.size());
        for (std::size_t g = 0; g < groups.size(); ++g) {
            f.layout.push_back(groups[g].layouts[0]);
            next_first[g] = groups[g].layouts[1].window_length.front();
            f.max_sfb.push_back(max_sfb_for(f.layout[g], groups[g]));
        }

        // A-SPX's parameters for the frame's interval, and with companding
        // the compressed low band its transform window takes.
        if (aspx) {
            analyse_qmf(frame);
            f.aspx = aspx_frame(frame, iframe, std::nullopt);
        }
        if (acpl) {
            analyse_acpl(frame);
            f.acpl = acpl->propose(frame, iframe);
        }
        const auto aspx_fields = [&](std::size_t c) -> const detail::AspxChannelFields& {
            for (std::size_t e = 0; e < plan.aspx_elements.size(); ++e) {
                for (std::size_t i = 0; i < plan.aspx_elements[e].size(); ++i) {
                    if (static_cast<std::size_t>(plan.aspx_elements[e][i]) == c) {
                        return f.aspx->elements[e].channels[i];
                    }
                }
            }
            return f.aspx->elements.front().channels.front();
        };

        // With interleaving, the spectral frontend codes above the crossover
        // the groups this frame's A-SPX data marks and the last frame's,
        // whose slots its transform window overlaps, and nothing else there,
        // in each layout group's channels.
        std::vector<std::vector<std::pair<double, double>>> waveform_hz(groups.size());
        std::vector<std::vector<std::pair<int, int>>> interleaved(channels);
        if (aspx && aspx->interleave) {
            const double subband_hz = static_cast<double>(rate_hz) / 128.0;
            for (std::size_t g = 0; g < groups.size(); ++g) {
                for (const int channel : groups[g].channels) {
                    const auto c = static_cast<std::size_t>(channel);
                    if (qmf_of[c] < 0) {
                        continue;
                    }
                    interleaved[c] = qmf[static_cast<std::size_t>(qmf_of[c])].interleaved_subbands(aspx_fields(c));
                    for (const auto& ranges : {interleaved[c], interleaved_prev[c]}) {
                        for (const auto& [first, last] : ranges) {
                            waveform_hz[g].emplace_back(first * subband_hz, last * subband_hz);
                        }
                    }
                }
                double top = 0.0;
                for (const auto& range : waveform_hz[g]) {
                    top = std::max(top, range.second);
                }
                const int first_length = f.layout[g].window_length.front();
                const int last_length = f.layout[g].window_length.back();
                f.max_sfb[g] = {std::max(f.max_sfb[g][0], bands_below(first_length, top, rate_hz)),
                                std::max(f.max_sfb[g][1], bands_below(last_length, top, rate_hz))};
            }
        }

        limit_residuals(f);
        std::vector<detail::Channel>& spectra = p.spectra;
        std::vector<std::vector<std::vector<bool>>>& silenced = p.silenced;
        spectra.assign(channels, detail::Channel{});
        silenced.assign(channels, {});
        std::vector<double> window(2 * static_cast<std::size_t>(frame_length));
        std::vector<double> spectrum;
        for (std::size_t g = 0; g < groups.size(); ++g) {
            for (const int channel : groups[g].channels) {
                const auto c = static_cast<std::size_t>(channel);
                for (std::size_t i = 0; i < window.size(); ++i) {
                    window[i] = coded_sample(c, start + static_cast<std::int64_t>(i));
                }
                analysis.transform(window, f.layout[g], groups[g].previous_last, next_first[g], spectrum);
                spectra[c].grouped = detail::regroup(spectrum, f.layout[g], max_sfb_of(f, c));
                spectra[c].allowed = psycho.thresholds(spectra[c].grouped, f.layout[g]);
                silenced[c] = silenced_bands(spectra[c].grouped, f.layout[g], waveform_hz[g]);
                for (std::size_t gr = 0; gr < spectra[c].allowed.size(); ++gr) {
                    for (std::size_t b = 0; b < spectra[c].allowed[gr].size(); ++b) {
                        if (silenced[c][gr][b]) {
                            spectra[c].allowed[gr][b] = kSilencedAllowance;
                        }
                    }
                }
            }
        }
        if (config.experimental.coding_configs && plan.ch_mode >= 3) {
            const std::size_t g = group_of[static_cast<std::size_t>(plan.l)];
            f.structure = choose_structure(spectra, f.layout[g], f.max_sfb[g]);
        } else {
            f.structure = structure_for(plan, 0, false, 0);
            undo(f.structure, spectra);
        }
        // The tracks in the syntax's order, each where its unit left it.
        p.order.clear();
        for (const Unit& unit : f.structure.units) {
            for (const int c : unit.outputs) {
                p.order.push_back(static_cast<std::size_t>(c));
            }
        }
        f.tracks.resize(channels);
        BitWriter side = BitWriter::buffered();
        write_element(side, f, false);
        p.side_bits = side.bit_position();

        // The rate loop's level of noise per line is measured from the
        // highest allowance per line, and its cap from each band's energy.
        p.top_level = 0.0;
        p.energy.assign(channels, {});
        for (const std::size_t c : p.order) {
            const detail::Channel& track = spectra[c];
            p.energy[c].resize(track.allowed.size());
            for (std::size_t g = 0; g < track.allowed.size(); ++g) {
                p.energy[c][g].assign(track.allowed[g].size(), 0.0);
                for (std::size_t b = 0; b < track.allowed[g].size(); ++b) {
                    if (silenced[c][g][b]) {
                        continue;
                    }
                    const std::size_t begin = track.grouped.offset[g][b];
                    const std::size_t end = track.grouped.offset[g][b + 1];
                    p.top_level = std::max(p.top_level,
                                           track.allowed[g][b] / static_cast<double>(end - begin));
                    for (std::size_t k = begin; k < end; ++k) {
                        p.energy[c][g][b] += track.grouped.lines[k] * track.grouped.lines[k];
                    }
                }
            }
        }
        p.sf.assign(channels, {});
        p.prepared = f;
    }

    // The rate loop: a level of noise per line, top_level * 10^(kStepDb p /
    // 10) at step p, and two laws that bring the bands' allowances to it.
    // Where the budget holds every band at its masking threshold (p = 0), the
    // bits left lower the level: each band whose allowance per line is over
    // it is brought kLevelWeight of the way to it, in dB, so that the bits go
    // first where the noise is loudest. Where it does not, every band is
    // pulled kLevelWeight of the way to the level from above or below: the
    // loudest bands keep noise under their thresholds, as a waveform coder at
    // a low rate must, and the quietest give up theirs first. No band's noise
    // is let past its energy, which would leave a hole, until the last steps
    // (kCapSteps on) relax that too.
    void set_step(bool pull, int step) {
        Pending& p = pending;
        const double level = p.top_level * std::pow(10.0, kStepDb * step / 10.0);
        const double cap =
            p.kappa *
            (step > kCapSteps ? std::pow(10.0, kStepDb * (step - kCapSteps) / 10.0) : 1.0);
        for (const std::size_t c : p.order) {
            const detail::Channel& track = p.spectra[c];
            p.capped = track.allowed;
            for (std::size_t g = 0; g < p.capped.size(); ++g) {
                for (std::size_t b = 0; b < p.capped[g].size(); ++b) {
                    if (p.silenced[c][g][b]) {
                        continue;
                    }
                    const auto lines = static_cast<double>(track.grouped.offset[g][b + 1] -
                                                           track.grouped.offset[g][b]);
                    double& allowance = p.capped[g][b];
                    if (!pull) {
                        if (allowance > level * lines) {
                            allowance *= std::pow(level * lines / allowance, kLevelWeight);
                        }
                        continue;
                    }
                    allowance *= std::pow(level * lines / allowance, kLevelWeight);
                    if (p.energy[c][g][b] > 0.0) {
                        allowance = std::min(allowance, cap * p.energy[c][g][b]);
                    }
                }
            }
            p.sf[c] = detail::scale_factors_for(track.grouped, p.capped);
        }
    }

    [[nodiscard]] const FrameLayout& layout_of(std::size_t c) const {
        return pending.f.layout[group_of[c]];
    }

    [[nodiscard]] std::size_t bits_at(bool pull, int step) {
        set_step(pull, step);
        std::size_t total = 0;
        for (const std::size_t c : pending.order) {
            total += detail::code_track(pending.spectra[c].grouped, pending.sf[c], 0, layout_of(c))
                         .bits();
        }
        return total;
    }

    // The bits the prepared frame's channel element needs at its masking
    // thresholds (step 0), side information included: what an average or
    // variable rate gives the frame where its buffer allows.
    [[nodiscard]] std::size_t needed_bits() {
        pending.kappa = kCaps.front();
        return pending.side_bits + bits_at(false, 0);
    }

    // The prepared frame, coded into an audio substream of exactly
    // `substream_bytes`: a buffered writer, its records kept; nothing where
    // not even the frame the fallbacks end at fits.
    [[nodiscard]] std::optional<BitWriter> code(std::size_t substream_bytes) {
        Pending& p = pending;
        Coding& f = p.f;
        f = p.prepared;
        de_current = p.de_prepared;
        p.kappa = kCaps.front();
        const std::int64_t frame = p.frame;
        const std::size_t overhead =
            detail::audio_substream_overhead_bits(p.fields, substream_bytes);
        const std::size_t budget = 8 * substream_bytes > overhead + p.side_bits
                                       ? 8 * substream_bytes - overhead - p.side_bits
                                       : 0;
        // The lowest step of a law that fits: the bits fall as it rises.
        const auto lowest_fitting = [&](bool pull, int low, int high) {
            while (low < high) {
                const int mid = low + (high - low) / 2;
                if (bits_at(pull, mid) <= budget) {
                    high = mid;
                } else {
                    low = mid + 1;
                }
            }
            return low;
        };
        const auto write = [&]() {
            BitWriter audio = BitWriter::buffered();
            write_element(audio, f, true);
            return detail::write_audio_substream(p.fields, audio, substream_bytes);
        };
        const auto write_at = [&](bool pull, int step) {
            set_step(pull, step);
            for (const std::size_t c : p.order) {
                f.tracks[c] = detail::code_track(p.spectra[c].grouped, p.sf[c], 0, layout_of(c));
            }
            return write();
        };
        std::optional<BitWriter> raw;
        if (bits_at(false, 0) <= budget) {
            for (int step = lowest_fitting(false, kLowestStep, 0); step <= 0 && !raw; ++step) {
                raw = write_at(false, step);
            }
        }
        // The pull, with the tightest cap on each band's noise the budget
        // holds before the last steps relax it.
        int first = kCapSteps + 1;
        for (const double k : kCaps) {
            if (raw) {
                break;
            }
            p.kappa = k;
            const int step = lowest_fitting(true, kLowestStep, kCapSteps);
            if (bits_at(true, step) <= budget) {
                first = step;
                break;
            }
        }
        if (!raw) {
            int step = first;
            if (first > kCapSteps) {
                // No cap held: the last steps relax the loosest.
                p.kappa = kCaps.back();
                step = lowest_fitting(true, kCapSteps, kHighestStep);
            }
            for (; step <= kHighestStep && !raw; ++step) {
                raw = write_at(true, step);
            }
        }
        if (!raw) {
            // Lines so far past full scale that the coarsest step still codes
            // more than the frame holds, or A-SPX data that leave too little:
            // the frame goes out with no bands, and then with the A-SPX data
            // that cost least. With A-CPL, the parameters as proposed, then
            // those the decoder holds already, then in an I-frame those a
            // stream starts from. Last, the per-frame metadata a stream
            // starts from too, or kept from the last frame: this is the frame
            // create() checks the rate holds, whatever the frame's content.
            const std::optional<AspxFrame> proposed = std::move(f.aspx);
            std::optional<detail::AcplFrameFields> parameters = std::move(f.acpl);
            const bool coupled = parameters.has_value();
            f = silent(f.iframe, f.layout);
            for (const Fallback step : {Fallback::kProposed, Fallback::kHeld, Fallback::kLeast,
                                        Fallback::kLeastMetadata}) {
                if (step == Fallback::kHeld || step == Fallback::kLeast) {
                    if (!coupled) {
                        continue;
                    }
                    parameters =
                        step == Fallback::kHeld ? acpl->held(f.iframe) : acpl->least(f.iframe);
                }
                if (step == Fallback::kLeastMetadata) {
                    if (stem()) {
                        de_current = least_parameters(f.iframe);
                    }
                }
                f.acpl = parameters;
                f.aspx = proposed;
                raw = write();
                for (const bool silence : {false, true}) {
                    if (raw || !f.aspx) {
                        break;
                    }
                    f.aspx = aspx_frame(frame, f.iframe, silence);
                    raw = write();
                }
                if (raw) {
                    break;
                }
            }
        }
        return raw;
    }

    // Dialogue enhancement's parameters a frame falls back to: in an I-frame
    // or before any was sent those a stream starts from, 0 in every band,
    // and otherwise the last frame's again; the waveform's share as
    // configured.
    [[nodiscard]] detail::DeFrameParameters least_parameters(bool iframe) const {
        detail::DeFrameParameters least =
            iframe || !de_sent ? detail::DeFrameParameters{} : de_previous;
        if (metadata.de) {
            least.signal_contribution = metadata.de->signal_contribution;
        }
        return least;
    }

    // What the next frame codes against, once the frame code() wrote is out.
    void commit() {
        const Coding& f = pending.f;
        const std::int64_t frame = pending.frame;
        if (f.aspx) {
            for (std::size_t e = 0; e < plan.aspx_elements.size(); ++e) {
                const detail::AspxElement& element = f.aspx->elements[e];
                for (std::size_t i = 0; i < plan.aspx_elements[e].size(); ++i) {
                    const auto c = static_cast<std::size_t>(plan.aspx_elements[e][i]);
                    detail::AspxChannelEncoder& channel = qmf[static_cast<std::size_t>(qmf_of[c])];
                    channel.commit(frame, element.channels[i], element.balance && i == 1);
                    channel.drop_before_frame(frame + 1);
                    interleaved_prev[c] = channel.interleaved_subbands(element.channels[i]);
                }
            }
        }
        if (f.acpl) {
            acpl->commit(*f.acpl);
            acpl->drop_before_frame(frame + 1);
        }
        for (std::size_t g = 0; g < groups.size(); ++g) {
            groups[g].previous_last = f.layout[g].window_length.back();
        }
        if (metadata.de) {
            de_previous = de_current;
            de_sent = true;
        }
    }

    // The least frames, for create() to check the rate holds them: a frame
    // with no bands, the A-SPX data and A-CPL parameters that cost least, and
    // the per-frame metadata a stream starts from, in an I-frame, whatever its
    // blocks. `use` takes each's audio substream fields and channel element,
    // and says whether to go on.
    template <typename Use>
    void each_least(const Use& use) {
        const detail::DeFrameParameters kept = de_current;
        if (stem()) {
            de_current = least_parameters(true);
        }
        const detail::AudioSubstreamFields fields = fields_for(true);
        // A silent frame's A-SPX data with its interval from the frame's
        // start, and from a slot into it, as where the last frame's interval
        // ran on, which costs the more.
        std::vector<std::optional<AspxFrame>> aspx_data = {std::nullopt};
        if (aspx) {
            aspx_data = {aspx_frame(0, true, true), aspx_frame(0, true, true, 1)};
        }
        const int n = timing.frame_length;
        const FrameLayout shortest = timing.long_family() ? detail::split_layout(n, {0, 0}, {0, 0})
                                                          : detail::short_layout(n, 0, -1);
        bool going = true;
        for (const FrameLayout& layout : {detail::long_layout(n), shortest}) {
            std::vector<FrameLayout> layouts;
            for (const Group& group : groups) {
                layouts.push_back(group.lfe ? detail::long_layout(n) : layout);
            }
            for (const std::optional<AspxFrame>& data : aspx_data) {
                if (!going) {
                    break;
                }
                Coding least = silent(true, layouts);
                least.aspx = data;
                if (acpl) {
                    least.acpl = acpl->least(true);
                }
                BitWriter audio = BitWriter::buffered();
                write_element(audio, least, true);
                going = use(fields, audio);
            }
        }
        de_current = kept;
    }

    // Whether every least frame fits an audio substream of `substream_bytes`.
    [[nodiscard]] bool least_fits(std::size_t substream_bytes) {
        bool fits = true;
        each_least([&](const detail::AudioSubstreamFields& fields, const BitWriter& audio) {
            fits = detail::write_audio_substream(fields, audio, substream_bytes).has_value();
            return fits;
        });
        return fits;
    }

    // The bytes the largest least frame's audio substream takes.
    [[nodiscard]] std::size_t least_bytes() {
        std::size_t most = 0;
        each_least([&](const detail::AudioSubstreamFields& fields, const BitWriter& audio) {
            most = std::max(most,
                            detail::audio_substream_bytes(fields, (audio.bit_position() + 7) / 8));
            return true;
        });
        return most;
    }

    // Keeps the input as it arrives, for DRC gains a presentation computes
    // from it: from before any input, the silence ahead of it first.
    void keep_input() {
        kept_input.assign(static_cast<std::size_t>(config.channels),
                          std::vector<double>(static_cast<std::size_t>(delay), 0.0));
    }

    // The frame's transform layouts, decided for each group before it is
    // prepared (with the next frame's, whose first window this one's last
    // meets), and dropped once it is committed.
    void before_frame(std::int64_t frame) {
        for (Group& group : groups) {
            while (static_cast<std::int64_t>(group.layouts.size()) < 2) {
                group.layouts.push_back(
                    decide(frame + static_cast<std::int64_t>(group.layouts.size()), group));
            }
        }
    }

    void after_frame(std::int64_t frames_out) {
        for (Group& group : groups) {
            group.layouts.pop_front();
        }
        // Nothing before the next frame's window is read again.
        const std::int64_t keep_from =
            (frames_out * frame_length) - static_cast<std::int64_t>(sub_block) * 5;
        if (keep_from > base) {
            const auto drop =
                static_cast<std::size_t>(std::min(keep_from - base, signal_end() - base));
            for (auto* buffers : {&signal, &source, &de_programme, &de_dialogue, &kept_input}) {
                for (std::vector<double>& channel : *buffers) {
                    channel.erase(channel.begin(),
                                  channel.begin() + static_cast<std::ptrdiff_t>(drop));
                }
            }
            base += static_cast<std::int64_t>(drop);
        }
    }

    // The input at the internal rate: each channel as it is, or `through`
    // its converter.
    [[nodiscard]] static std::vector<std::vector<double>> internal(
        std::span<const std::span<const float>> input,
        std::vector<detail::dsp::Resampler<double>>& through);

    // Appends input at the internal rate, and with a stem the dialogue in it.
    void take(const std::vector<std::vector<double>>& programme_input,
              const std::vector<std::vector<double>>& stem_input);

    // The signal frame f needs read before it is coded: to half a frame past
    // its window, where the next frame's transients are; with A-SPX, what the
    // QMF slots it needs analysed read; with A-CPL, what its estimate reads;
    // and with a dialogue stem, the window its parameters come from. At
    // frame_rate_index 13 the first holds all the others but the last.
    [[nodiscard]] std::int64_t input_needed(std::int64_t frame) const {
        std::int64_t needed = (frame + 2) * frame_length + frame_length / 2;
        const auto through_slot = [&](std::int64_t slots) {
            return kQmfSlot * slots - timing.alignment_delay;
        };
        if (aspx) {
            needed = std::max(needed, through_slot(qmf_slots_needed(frame)));
        }
        if (acpl) {
            needed = std::max(needed, through_slot(acpl->slots_needed(frame)));
        }
        if (stem()) {
            needed = std::max(needed,
                              dialogue_window(frame) + 2 * static_cast<std::int64_t>(frame_length));
        }
        return needed;
    }
};

std::expected<std::unique_ptr<SubstreamCoder>, Refusal> SubstreamCoder::make(
    const EncoderConfig& config, CodecMode mode, bool converts) {
    const std::expected<Plan, Refusal> plan = plan_for(config, mode);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (config.sample_rate_hz != 48000 && config.sample_rate_hz != 44100) {
        return std::unexpected("a sample rate other than 48 kHz or 44.1 kHz");
    }
    if (config.bitrate_kbps < 1) {
        return std::unexpected("a substream's rate below 1 kbps");
    }
    const std::optional<detail::FrameTiming> timing =
        detail::frame_timing(config.frame_rate_index, config.sample_rate_hz);
    if (!timing) {
        return std::unexpected(
            "a frame_rate_index Part 1 Table 83 does not give at the sample rate: 0 to 13 at 48 "
            "kHz, 13 alone at 44.1 kHz");
    }
    auto coder = std::make_unique<SubstreamCoder>();
    coder->config = config;
    coder->plan = *plan;
    coder->timing = *timing;
    coder->frame_length = timing->frame_length;
    coder->delay = timing->frame_length * 3 / 2;
    coder->sub_block = timing->frame_length / kSubBlocks;
    // The internal rate: the sample rate over the decoder's resampling ratio.
    const double internal_rate =
        static_cast<double>(config.sample_rate_hz) * timing->decoder_down / timing->decoder_up;
    coder->rate_hz = static_cast<int>(std::lround(internal_rate));
    coder->analysis = detail::Analysis(timing->frame_length, 1);
    coder->psycho = detail::Psychoacoustics(coder->rate_hz, timing->frame_length);
    if (timing->resampled() && converts) {
        // The converters, the inverse of the decoder's.
        const auto filter = std::make_shared<const detail::dsp::ResamplerFilter>(
            timing->decoder_down, timing->decoder_up);
        const int stem_channels =
            config.dialogue && config.dialogue->source == DialogueSource::kStem ? config.channels
                                                                                : 0;
        coder->converters.assign(static_cast<std::size_t>(config.channels),
                                 detail::dsp::Resampler<double>(filter));
        coder->stem_converters.assign(static_cast<std::size_t>(stem_channels),
                                      detail::dsp::Resampler<double>(filter));
    }
    const auto channels = static_cast<std::size_t>(plan->coded);
    const int full_channels = config.channels - (plan->lfe >= 0 ? 1 : 0);
    const double kbps_per_channel = static_cast<double>(config.bitrate_kbps) / full_channels;
    const bool multichannel = plan->ch_mode >= 3;
    coder->cutoff = cutoff_hz(kbps_per_channel);
    coder->group_of.assign(channels, 0);
    for (std::size_t g = 0; g < plan->groups.size(); ++g) {
        SubstreamCoder::Group group;
        group.channels = plan->groups[g];
        group.lfe = group.channels.size() == 1 && group.channels.front() == plan->lfe;
        for (const int c : group.channels) {
            coder->group_of[static_cast<std::size_t>(c)] = g;
        }
        group.previous_last = timing->frame_length;
        coder->groups.push_back(std::move(group));
    }
    if (plan->acpl) {
        if (*plan->acpl == detail::AcplLayout::kPair) {
            // As stereo is coded in the ASPX mode at the rate, without
            // companding, which DEE's A-CPL streams never turn on.
            coder->aspx =
                detail::aspx_setup_for(kbps_per_channel, config.sample_rate_hz, false, *timing);
            if (coder->aspx) {
                coder->aspx->companding = false;
            }
        } else {
            coder->aspx = detail::aspx_setup_for_acpl(*plan->acpl == detail::AcplLayout::kCoupling,
                                                      config.sample_rate_hz, *timing);
        }
        coder->acpl.emplace(*plan->acpl, kAcplBandsId, kAcplQuantMode,
                            plan->residuals.empty() ? 0 : kAcplResidualQmfBand, *timing);
        coder->source.assign(plan->source.size(),
                             std::vector<double>(static_cast<std::size_t>(coder->delay), 0.0));
    } else if (mode == CodecMode::kAspx) {
        coder->aspx =
            detail::aspx_setup_for(kbps_per_channel, config.sample_rate_hz, multichannel, *timing);
    }
    if (mode != CodecMode::kSimple) {
        if (!coder->aspx) {
            return std::unexpected(
                "the ASPX or an A-CPL codec mode at a rate A-SPX has no configuration for");
        }
        coder->aspx->varvar = config.experimental.aspx_varvar;
        coder->aspx->balance = config.experimental.aspx_balance;
        coder->aspx->interleave = config.experimental.aspx_interleave;
        coder->interleaved_prev.resize(channels);
        // The spectral frontend codes up to the crossover, subband sbx of 64
        // across half the sampling rate.
        coder->cutoff = static_cast<double>(coder->aspx->groups.sbx) * coder->rate_hz / 128.0;
        coder->qmf_of.assign(channels, -1);
        for (std::size_t c = 0; c < channels; ++c) {
            const bool coded = std::ranges::any_of(plan->aspx_elements, [&](const std::vector<int>& element) {
                return std::ranges::find(element, static_cast<int>(c)) != element.end();
            });
            if (!coded) {
                continue;
            }
            coder->qmf_of[c] = static_cast<int>(coder->qmf.size());
            coder->qmf_channel.push_back(static_cast<int>(c));
            coder->qmf.emplace_back(*coder->aspx);
        }
    }
    coder->signal.assign(channels,
                         std::vector<double>(static_cast<std::size_t>(coder->delay), 0.0));

    // Of the metadata, the substream carries its dialogue enhancement; the
    // rest is its presentations'.
    if (config.dialogue) {
        coder->metadata.de = detail::resolve_dialogue(*config.dialogue, plan->ch_mode);
        if (!coder->metadata.de) {
            return std::unexpected(
                "dialogue enhancement on a channel the layout lacks, a cap other than 3, 6, 9 or "
                "12 dB, the Mid without L and R, the cross-channel method without a stem over two "
                "or three channels, or a waveform share outside 0 to 1");
        }
        // de_channel_config's L, R and C (Table 171) as input channels: C
        // alone in mono, L and R in stereo, and L, R and C the first three
        // otherwise.
        const int channel_config = coder->metadata.de->channel_config;
        for (const auto& [bit, input] :
             {std::pair{4, 0}, std::pair{2, 1}, std::pair{1, config.channels == 1 ? 0 : 2}}) {
            if ((channel_config & bit) != 0) {
                coder->de_channels.push_back(static_cast<std::size_t>(input));
            }
        }
        if (coder->stem()) {
            coder->de_programme.assign(
                coder->de_channels.size(),
                std::vector<double>(static_cast<std::size_t>(coder->delay), 0.0));
            coder->de_dialogue = coder->de_programme;
        } else {
            // Marked channels carry dialogue alone: 1 in every band.
            const int one = detail::de_parameter_index(1.0);
            for (std::size_t i = 0; i < coder->de_channels.size(); ++i) {
                coder->de_current.par[i].fill(one);
            }
        }
        coder->de_current.signal_contribution = coder->metadata.de->signal_contribution;
    }
    return coder;
}

namespace {

// Part 2 Table 53 and Table 54: a substream's role in a presentation.
enum class Role : std::uint8_t { kMain, kMusicAndEffects, kDialogue, kEnhancement, kAssociated };

// Table 54: the role a presentation_config 5 group takes from its content
// classifier.
[[nodiscard]] Role role_from_classifier(ContentClassifier c) noexcept {
    switch (c) {
        case ContentClassifier::kVisuallyImpaired:
        case ContentClassifier::kHearingImpaired:
        case ContentClassifier::kCommentary:
            return Role::kAssociated;
        case ContentClassifier::kDialogue:
            return Role::kDialogue;
        case ContentClassifier::kMusicAndEffects:
            return Role::kMusicAndEffects;
        default:
            return Role::kMain;
    }
}

// The channels a Part 1 Table 88 channel mode holds, one bit per
// loudspeaker, for the rule that dialogue and associated audio add none
// (Part 1 clause 6.2.16.0) and for superset() (Part 2 clause 6.3.3.1.27).
constexpr std::uint32_t kL = 1U << 0U;
constexpr std::uint32_t kR = 1U << 1U;
constexpr std::uint32_t kC = 1U << 2U;
constexpr std::uint32_t kLfe = 1U << 3U;
constexpr std::uint32_t kLs = 1U << 4U;
constexpr std::uint32_t kRs = 1U << 5U;
constexpr std::uint32_t kLb = 1U << 6U;
constexpr std::uint32_t kRb = 1U << 7U;
constexpr std::uint32_t kLw = 1U << 8U;
constexpr std::uint32_t kRw = 1U << 9U;
constexpr std::uint32_t kTfl = 1U << 10U;
constexpr std::uint32_t kTfr = 1U << 11U;
constexpr std::uint32_t kFive = kL | kR | kC | kLs | kRs;
constexpr std::array<std::uint32_t, 11> kModeChannels = {
    kC,                          // 0 mono
    kL | kR,                     // 1 stereo
    kL | kR | kC,                // 2 3.0
    kFive,                       // 3 5.0
    kFive | kLfe,                // 4 5.1
    kFive | kLb | kRb,           // 5 7.0 3/4/0
    kFive | kLb | kRb | kLfe,    // 6 7.1 3/4/0.1
    kFive | kLw | kRw,           // 7 7.0 5/2/0
    kFive | kLw | kRw | kLfe,    // 8 7.1 5/2/0.1
    kFive | kTfl | kTfr,         // 9 7.0 3/2/2
    kFive | kTfl | kTfr | kLfe,  // 10 7.1 3/2/2.1
};

// Part 2 clause 6.3.3.1.27's superset() over Table 88's channel modes: the
// lowest mode holding every channel of both, superset(0, 1) being 1; -1
// where none of 0 to 10 does, which the channel rule above leaves no
// presentation of this encoder's.
[[nodiscard]] int superset(int a, int b) noexcept {
    if (a < 0 || b < 0) {
        return a < 0 ? b : a;
    }
    if ((a == 0 && b == 1) || (a == 1 && b == 0)) {
        return 1;
    }
    const std::uint32_t wanted =
        kModeChannels[static_cast<std::size_t>(a)] | kModeChannels[static_cast<std::size_t>(b)];
    for (std::size_t mode = 0; mode < kModeChannels.size(); ++mode) {
        if ((kModeChannels[mode] & wanted) == wanted) {
            return static_cast<int>(mode);
        }
    }
    return -1;
}

[[nodiscard]] bool mode_has_lfe(int ch_mode) noexcept {
    return ch_mode == 4 || ch_mode == 6 || ch_mode == 8 || ch_mode == 10;
}

// Part 2 Table 55 at presentation_version 1: the least md_compat whose track
// count, the channels of every substream the presentation plays but their
// LFEs, holds `tracks`; 7, unrestricted, above 11.
[[nodiscard]] int least_md_compat(int tracks) noexcept {
    if (tracks <= 2) {
        return 0;
    }
    if (tracks <= 6) {
        return 1;
    }
    if (tracks <= 9) {
        return 2;
    }
    return tracks <= 11 ? 3 : 7;
}

// A gain on Part 2 Table 70's scale, -0.25 dB a step from 0 to 62 and 63
// silence; nothing for a value it does not have.
[[nodiscard]] std::optional<int> group_gain_code(double db) noexcept {
    if (std::isinf(db) && db < 0.0) {
        return 63;
    }
    const double code = -db / 0.25;
    if (!(code >= 0.0 && code <= 62.0) || code != std::floor(code)) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

// A gain on Part 1 clauses 4.3.12.4.4 to 4.3.12.4.8's scale, -0.3 dB a step
// from 0 to 254 and 255 silence (src/ac4dec/ERRATA.md, "The main audio's and
// the dialogue's scaling with associated audio").
[[nodiscard]] std::optional<int> scale_code(double db) noexcept {
    if (std::isinf(db) && db < 0.0) {
        return 255;
    }
    const double code = std::round(-db / 0.3);
    if (!(code >= 0.0 && code <= 254.0) || std::abs(code * 0.3 + db) > 1e-9) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

// A substream as the stream holds it: its coder; where its input channels
// start in encode()'s input and how many it takes (none for a dialogue
// enhancement substream); the substream whose hybrid dialogue enhancement's
// waveform it codes; its weight in sharing the rate; and its group's
// content_type(). A cross-channel waveform's estimate of the dialogue's
// panning, from its energy in each channel as it arrives, leaky.
struct StreamSubstream {
    std::unique_ptr<SubstreamCoder> coder;
    std::size_t first_input = 0;
    int inputs = 0;
    std::optional<std::size_t> enhances;
    double weight = 1.0;
    std::optional<int> content_classifier;
    std::string language;
    std::array<double, 3> pan_energy{};
};

// A presentation as the stream writes it: its ac4_presentation_v1_info(), the
// substreams it plays, and what its presentation substream carries.
struct StreamPresentation {
    detail::TocPresentation toc{};
    std::vector<std::size_t> members;
    std::optional<std::size_t> anchor;  // its main or music and effects substream
    std::optional<detail::AlternativeCodes> alternative;
    int dialnorm_bits = 124;
    double dialnorm_db = -31.0;
    std::optional<detail::LoudnessCodes> loudness;
    std::optional<detail::DrcCodes> drc;
    std::optional<detail::DownmixCodes> downmix;
    detail::PresentationMixCodes mix{};  // as an I-frame sends them
    int pres_ch_mode = 1;
    bool pres_has_lfe = false;
    std::vector<detail::EmdfPayloadCodes> emdf;
    // DRC modes that send gains: a computer for each, by the mode's place in
    // drc_config(), fed the input of the presentation's main or music and
    // effects substream; each mode's gains for the frame being coded, and
    // those a stream starts from, 0 dB throughout.
    std::vector<std::optional<detail::DrcGainEncoder>> drc_gain_encoders;
    std::vector<detail::DrcModeGains> drc_gains;
    std::vector<detail::DrcModeGains> least_drc_gains;
};

// The modes kAuto tries, in turn, where the rate cannot hold the one it
// resolves to: ASPX_ACPL_3's least frame, with eleven parameters a band,
// needs 25 kbps in 5.1 at 48 kHz, ASPX_ACPL_2's 15, and ASPX's 20.
[[nodiscard]] std::vector<CodecMode> modes_for(const EncoderConfig& config) {
    const CodecMode mode = resolve_mode(config);
    std::vector<CodecMode> modes = {mode};
    if (config.codec_mode == CodecMode::kAuto) {
        if (mode == CodecMode::kAspxAcpl3) {
            modes.push_back(CodecMode::kAspxAcpl2);
        }
        if (is_acpl(mode)) {
            modes.push_back(CodecMode::kAspx);
        }
    }
    return modes;
}

// The DRC channels of an input layout as BS.1770 weights them and Table 168
// groups them, in the input's order: L R C, the LFE, Ls Rs, and a 7.X pair.
[[nodiscard]] std::vector<detail::DrcChannel> drc_channels_of(int channels, AdditionalPair pair) {
    using detail::DrcChannel;
    if (channels == 1) {
        return {DrcChannel::kCentre};
    }
    if (channels == 2) {
        return {DrcChannel::kFront, DrcChannel::kFront};
    }
    std::vector<DrcChannel> out = {DrcChannel::kFront, DrcChannel::kFront, DrcChannel::kCentre};
    if (channels == 3) {
        return out;
    }
    if (channels % 2 == 0) {
        out.push_back(DrcChannel::kLfe);
    }
    out.insert(out.end(), {DrcChannel::kSide, DrcChannel::kSide});
    if (channels > 6) {
        const DrcChannel extra = pair == AdditionalPair::kBack   ? DrcChannel::kBack
                                 : pair == AdditionalPair::kWide ? DrcChannel::kWide
                                                                 : DrcChannel::kFront;
        out.insert(out.end(), {extra, extra});
    }
    return out;
}

}  // namespace

// Nested in an exported class, Impl takes its visibility, so each member
// function defined out of line below would be exported from libac4enc.so with
// it. AC4ENC_NO_EXPORT on each keeps them to the library, and the exported set
// to the header's API (tools/ci/abi-allowlist/libac4enc.so.txt).
struct Encoder::Impl {
    // The stream `config` asks for, or why it is not one the encoder writes,
    // or its rate cannot hold its least frame.
    [[nodiscard]] AC4ENC_NO_EXPORT static std::expected<std::unique_ptr<Impl>, Refusal> make(
        const EncoderConfig& config);

    EncoderConfig config{};
    detail::FrameTiming timing{};
    int delay = 3072;
    int fs_index = 1;
    // What flush() codes past the decoder's delay: its converter's, where
    // the frame rate needs one.
    int flush_extra = 0;
    std::vector<StreamSubstream> substreams;
    std::vector<StreamPresentation> presentations;
    // The EMDF payload substreams, each's payloads.
    std::vector<std::vector<detail::EmdfPayloadCodes>> emdf_substreams;
    // The table of contents every frame carries, its counter, rate fields and
    // I-frame flags set frame by frame; and where in substream_index_table()
    // the audio substreams and the EMDF payload substreams start, the
    // presentation substreams coming first. librempeg takes the substream
    // after the presentation substreams as the first group's audio.
    detail::TocLayout layout{};
    std::size_t first_audio = 0;
    std::size_t first_emdf = 0;

    // Where the k-th of the substreams a frame writes before its audio, the
    // presentation substreams and then the EMDF payload substreams, goes in
    // substream_index_table().
    [[nodiscard]] std::size_t fixed_index(std::size_t k) const noexcept {
        return k < first_audio ? k : k + substreams.size();
    }
    // The substream whose size a frame's fit settles, the one with the most
    // of the rate: the others take their shares.
    std::size_t slack = 0;
    Toc toc{};
    // A frame lasts frame_length samples of the internal rate.
    double bytes_per_frame = 0.0;
    double byte_carry = 0.0;
    // Average and variable rates (the model above): the decoder's input
    // buffer in frames' shares of the rate before the next frame is taken,
    // the least and the most a frame may leave there, and Part 2 Annex B's
    // br_code sequence.
    double rate_level = 0.0;
    double rate_low = 0.0;
    double rate_high = 0.0;
    std::vector<int> br_codes{3};
    // The frames the configuration makes I-frames besides the interval's:
    // those it names, and those whose output starts a fragment, sorted.
    std::vector<std::int64_t> forced_iframes;
    // The coders kAuto would try next, while create() checks the rate holds
    // the first's least frame; and each substream's least frame, which no
    // frame's share takes it below.
    std::vector<std::vector<std::unique_ptr<SubstreamCoder>>> fallbacks;
    std::vector<std::size_t> least_sizes;
    std::size_t input_channels = 0;
    bool stem = false;
    std::int64_t input_samples = 0;  // at the internal rate
    std::int64_t frames_out = 0;
    bool flushed = false;

    // Table 81's wait_frames for a frame that leaves `left` shares in the
    // buffer: the whole frames a decoder that starts at it waits, counted in
    // twos at indices 10 to 12.
    [[nodiscard]] int wait_frames_for(double left) const noexcept {
        if (config.rate_mode == RateMode::kVariable) {
            return 7;
        }
        if (timing.frame_rate_index >= 10 && timing.frame_rate_index <= 12) {
            return std::clamp(static_cast<int>(std::floor(left / 2.0)), 0, 5) + 1;
        }
        return std::clamp(static_cast<int>(std::floor(left)), 0, 5) + 1;
    }

    [[nodiscard]] bool iframe(std::int64_t frame) const {
        return frame % config.iframe_interval == 0 ||
               std::ranges::binary_search(forced_iframes, frame);
    }

    // The table of contents of frame f, with `wait_frames` (and br_code where
    // it is above 0).
    [[nodiscard]] detail::TocLayout layout_for(std::int64_t frame, bool is_iframe,
                                               int wait_frames) const {
        detail::TocLayout out = layout;
        out.sequence_counter = sequence_counter(frame);
        out.wait_frames = wait_frames;
        out.br_code =
            wait_frames > 0 ? br_codes[static_cast<std::size_t>(frame % std::ssize(br_codes))] : 0;
        out.iframe_global = is_iframe;
        for (detail::TocPresentation& p : out.presentations) {
            p.pres_ndot = is_iframe;
        }
        for (detail::TocGroup& g : out.groups) {
            for (detail::TocSubstream& s : g.substreams) {
                s.iframe = is_iframe;
            }
        }
        return out;
    }

    // Presentation p's substream in a frame: in an I-frame everything it is
    // configured with; between them the group gains kept (b_keep) and the
    // associated audio's values left to hold (Part 1 clause 6.2.16.0), with
    // DRC's gains where a mode sends them, or those a stream starts from.
    [[nodiscard]] BitWriter presentation_substream(const StreamPresentation& p, bool is_iframe,
                                                   bool least_gains) const {
        detail::PresentationSubstreamFields f;
        f.iframe = is_iframe;
        f.alternative = p.alternative ? &*p.alternative : nullptr;
        f.dialnorm_bits = p.dialnorm_bits;
        f.loudness = p.loudness ? &*p.loudness : nullptr;
        f.drc = p.drc ? &*p.drc : nullptr;
        f.drc_gains = least_gains ? p.least_drc_gains : p.drc_gains;
        f.mix = p.mix;
        if (!is_iframe) {
            f.mix.keep = f.mix.sg_gain.has_value();
            f.mix.associated.reset();
        }
        f.pres_ch_mode = p.pres_ch_mode;
        f.pres_has_lfe = p.pres_has_lfe;
        f.downmix = p.downmix ? &*p.downmix : nullptr;
        return detail::write_presentation_substream(f);
    }

    // Analyses the input's QMF slots frame f's DRC gains read, for each mode
    // of presentation p that sends them, and computes the gains; a repeat
    // takes those of the mode it repeats.
    void drc_frame_gains(StreamPresentation& p, std::int64_t frame) {
        const SubstreamCoder& anchor = *substreams[*p.anchor].coder;
        std::vector<std::array<double, kQmfSlot>> chunk(anchor.kept_input.size());
        for (std::size_t m = 0; m < p.drc_gain_encoders.size(); ++m) {
            std::optional<detail::DrcGainEncoder>& encoder = p.drc_gain_encoders[m];
            if (!encoder) {
                continue;
            }
            while (encoder->slots() < encoder->slots_needed(frame)) {
                const std::int64_t from = kQmfSlot * encoder->slots() - timing.alignment_delay;
                for (std::size_t c = 0; c < chunk.size(); ++c) {
                    for (std::size_t i = 0; i < chunk[c].size(); ++i) {
                        chunk[c][i] = anchor.kept_sample(c, from + static_cast<std::int64_t>(i));
                    }
                }
                encoder->push_slot(chunk);
            }
            p.drc_gains[m] = encoder->gains(frame);
        }
        const std::vector<detail::DrcModeCodes>& modes = p.drc->modes;
        for (std::size_t m = 0; m < modes.size(); ++m) {
            if (modes[m].repeat_id) {
                for (std::size_t other = 0; other < modes.size(); ++other) {
                    if (modes[other].id == *modes[m].repeat_id) {
                        p.drc_gains[m] = p.drc_gains[other];
                    }
                }
            }
        }
    }

    // The sizes of a frame of `frame_bytes`: every substream's in index order,
    // the presentation and EMDF payload substreams as written, each audio
    // substream but the slack its share of what they leave, or with `needs`
    // (an average or variable rate) what it needs, both less where the frame
    // holds less, and never below `least`; and the slack's, with
    // payload_base, what makes the frame exactly that long. Nothing where no
    // slack does.
    [[nodiscard]] std::optional<std::pair<std::vector<std::size_t>, detail::FrameFit>> sizes_for(
        const detail::TocLayout& frame_layout, std::span<const BitWriter> fixed,
        std::size_t frame_bytes, std::span<const std::size_t> needs,
        std::span<const std::size_t> least) const {
        std::vector<std::size_t> sizes(fixed.size() + substreams.size(), 0);
        std::size_t fixed_bytes = 0;
        for (std::size_t k = 0; k < fixed.size(); ++k) {
            sizes[fixed_index(k)] = fixed[k].byte_size();
            fixed_bytes += fixed[k].byte_size();
        }
        const std::size_t n = substreams.size();
        if (n > 1) {
            // What the audio substreams share, the table of contents taken at
            // their sizes had they the whole frame each: an upper bound.
            for (std::size_t i = 0; i < n; ++i) {
                sizes[first_audio + i] = frame_bytes;
            }
            const std::size_t toc_size = detail::toc_bytes(frame_layout, 0, sizes);
            const std::size_t available =
                frame_bytes > fixed_bytes + toc_size ? frame_bytes - fixed_bytes - toc_size : 0;
            double total_weight = 0.0;
            std::size_t total_needs = 0;
            for (std::size_t i = 0; i < n; ++i) {
                total_weight += substreams[i].weight;
                total_needs += needs.empty() ? 0 : needs[i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (i == slack) {
                    continue;
                }
                double share = 0.0;
                if (needs.empty()) {
                    share = static_cast<double>(available) * substreams[i].weight / total_weight;
                } else {
                    share = static_cast<double>(needs[i]);
                    if (total_needs > available) {
                        share *= static_cast<double>(available) / static_cast<double>(total_needs);
                    }
                }
                sizes[first_audio + i] = std::max(least[i], static_cast<std::size_t>(share));
            }
        }
        const SubstreamCoder& slack_coder = *substreams[slack].coder;
        const detail::AudioSubstreamFields slack_fields = slack_coder.pending.fields;
        const std::optional<detail::FrameFit> fit =
            detail::fit_frame(frame_layout, sizes, first_audio + slack, frame_bytes,
                              [&slack_fields](std::size_t bytes) {
                                  return detail::audio_substream_size_possible(slack_fields, bytes);
                              });
        if (!fit) {
            return std::nullopt;
        }
        sizes[first_audio + slack] = fit->slack_bytes;
        return std::pair{std::move(sizes), *fit};
    }

    [[nodiscard]] AC4ENC_NO_EXPORT EncodedFrame encode_frame(std::int64_t frame);

    // Takes the input, and with a stem the dialogue in it, and returns the
    // frames it completes.
    [[nodiscard]] AC4ENC_NO_EXPORT std::expected<std::vector<EncodedFrame>, EncodeError> push(
        std::span<const std::span<const float>> channels,
        std::span<const std::span<const float>> dialogue);

    // Appends one piece of input at the internal rate to every substream:
    // each its own channels (and their dialogue), and each dialogue
    // enhancement substream the waveform it derives from the substream it
    // enhances.
    AC4ENC_NO_EXPORT void take(std::span<const std::vector<std::vector<double>>> programmes,
                               std::span<const std::vector<std::vector<double>>> stems);

    // The dialogue enhancement substream's waveform (DialogueConfig::hybrid)
    // from what arrived for the substream it enhances.
    [[nodiscard]] AC4ENC_NO_EXPORT std::vector<std::vector<double>> waveform(
        StreamSubstream& de, const std::vector<std::vector<double>>& programme,
        const std::vector<std::vector<double>>& dialogue) const;

    // The frames the input read so far lets through; after flush(), until the
    // output covers the input, the delays and this project's decoder's
    // converter.
    [[nodiscard]] AC4ENC_NO_EXPORT std::vector<EncodedFrame> drain();
};

EncodedFrame Encoder::Impl::encode_frame(std::int64_t frame) {
    const bool is_iframe = iframe(frame);
    for (StreamSubstream& s : substreams) {
        s.coder->before_frame(frame);
        s.coder->prepare(frame, is_iframe);
    }
    for (StreamPresentation& p : presentations) {
        if (!p.drc_gain_encoders.empty()) {
            drc_frame_gains(p, frame);
        }
    }
    // The frame's size: at a constant rate its share of the rate; at the
    // others what its substreams need at their masking thresholds (step 0),
    // within what the buffer lets it borrow and makes it spend.
    const double exact = byte_carry + bytes_per_frame;
    auto frame_bytes = static_cast<std::size_t>(exact);
    std::vector<std::size_t> needs;
    int wait_frames = 0;
    std::optional<std::vector<std::byte>> raw;
    for (const bool least_gains : {false, true}) {
        // The presentation and EMDF payload substreams, fixed for the frame:
        // with DRC's gains, and where the audio does not fit beside them,
        // with the gains a stream starts from.
        std::vector<BitWriter> fixed;
        for (const StreamPresentation& p : presentations) {
            if (p.toc.presentation_config != 6) {
                fixed.push_back(presentation_substream(p, is_iframe, least_gains));
            }
        }
        for (const std::vector<detail::EmdfPayloadCodes>& payloads : emdf_substreams) {
            fixed.push_back(detail::write_emdf_payloads_substream(payloads));
        }
        detail::TocLayout frame_layout = layout_for(frame, is_iframe, 0);
        if (config.rate_mode != RateMode::kConstant) {
            const double share = bytes_per_frame;
            const auto longest =
                static_cast<std::size_t>(std::floor(share * (rate_level - rate_low)));
            const auto shortest = static_cast<std::size_t>(
                std::max(0.0, std::ceil(share * (rate_level - rate_high))));
            // The table of contents and the fixed substreams with the audio
            // at the longest a frame may be, and each audio substream's
            // header, metadata() and alignment, and channel element.
            frame_layout =
                layout_for(frame, is_iframe, config.rate_mode == RateMode::kVariable ? 7 : 1);
            std::vector<std::size_t> sizes(fixed.size() + substreams.size(), longest);
            std::size_t needed = 0;
            for (std::size_t k = 0; k < fixed.size(); ++k) {
                sizes[fixed_index(k)] = fixed[k].byte_size();
                needed += fixed[k].byte_size();
            }
            needed += detail::toc_bytes(frame_layout, 0, sizes);
            needs.assign(substreams.size(), 0);
            for (std::size_t i = 0; i < substreams.size(); ++i) {
                SubstreamCoder& coder = *substreams[i].coder;
                needs[i] = (detail::audio_substream_overhead_bits(coder.pending.fields, longest) +
                            coder.needed_bits() + 7) /
                           8;
                needed += needs[i];
            }
            frame_bytes = std::clamp(needed, std::min(shortest, longest), longest);
            wait_frames = wait_frames_for(rate_level - static_cast<double>(frame_bytes) / share);
            frame_layout = layout_for(frame, is_iframe, wait_frames);
        }
        const auto sized = sizes_for(frame_layout, fixed, frame_bytes, needs, least_sizes);
        if (!sized) {
            continue;
        }
        const auto& [sizes, fit] = *sized;
        // Each audio substream coded into its size, and every substream in
        // its place.
        std::vector<BitWriter> written(sizes.size());
        for (std::size_t k = 0; k < fixed.size(); ++k) {
            written[fixed_index(k)] = fixed[k];
        }
        bool fits = true;
        for (std::size_t i = 0; i < substreams.size() && fits; ++i) {
            std::optional<BitWriter> audio = substreams[i].coder->code(sizes[first_audio + i]);
            fits = audio.has_value();
            if (audio) {
                written[first_audio + i] = std::move(*audio);
            }
        }
        if (!fits) {
            continue;
        }
        raw = detail::assemble(frame_layout, written, fit.payload_base, {});
        if (!raw || raw->size() != frame_bytes) {
            raw.reset();
            continue;
        }
        if (config.trace) {
            for (std::size_t index = 0; index < written.size(); ++index) {
                for (SyntaxRecord record : written[index].kept()) {
                    record.substream = static_cast<int>(index);
                    config.trace(record);
                }
            }
        }
        break;
    }
    for (StreamSubstream& s : substreams) {
        s.coder->commit();
    }
    if (config.rate_mode == RateMode::kConstant) {
        byte_carry = exact - static_cast<double>(frame_bytes);
    } else {
        rate_level += 1.0 - static_cast<double>(frame_bytes) / bytes_per_frame;
    }
    EncodedFrame out;
    // create() checks the rate holds the frame every substream falls back
    // to, so a frame always fits.
    out.raw_ac4_frame = std::move(raw).value();
    out.samples = timing.output_samples(frame);
    out.iframe = is_iframe;
    return out;
}

std::vector<std::vector<double>> Encoder::Impl::waveform(
    StreamSubstream& de, const std::vector<std::vector<double>>& programme,
    const std::vector<std::vector<double>>& dialogue) const {
    const SubstreamCoder& main = *substreams[*de.enhances].coder;
    const DialogueConfig& dc = *main.config.dialogue;
    // The dialogue in each channel the enhancement raises: the stem's, or the
    // marked channels', which carry dialogue alone.
    const std::vector<std::vector<double>>& from =
        dc.source == DialogueSource::kStem ? dialogue : programme;
    const std::size_t count = programme.empty() ? 0 : programme.front().size();
    std::vector<std::vector<double>> in;
    for (const std::size_t c : main.de_channels) {
        in.push_back(from[c]);
    }
    if (dc.method == DialogueMethod::kChannelIndependent) {
        return in;  // one channel each, in L, R, C order
    }
    std::vector<std::vector<double>> out(1, std::vector<double>(count, 0.0));
    if (dc.method == DialogueMethod::kMid) {
        // 1/2 (1, 1) g_s d_c raises the Mid's dialogue, (L + R) / 2, in both
        // channels: d_c is L's and R's summed.
        for (std::size_t n = 0; n < count; ++n) {
            out[0][n] = in[0][n] + in[1][n];
        }
        return out;
    }
    // The cross-channel method renders d_c by r, the dialogue's panning:
    // d_c is the dialogue's projection on it, r estimated from the
    // dialogue's energy in each channel over the last half second or so.
    const double leak = std::exp(-1.0 / (0.5 * static_cast<double>(main.rate_hz)));
    std::array<double, 3>& energy = de.pan_energy;
    for (std::size_t n = 0; n < count; ++n) {
        double total = 0.0;
        for (std::size_t c = 0; c < in.size(); ++c) {
            energy[c] = leak * energy[c] + in[c][n] * in[c][n];
            total += energy[c];
        }
        double sum = 0.0;
        for (std::size_t c = 0; c < in.size(); ++c) {
            const double r = total > 0.0 ? std::sqrt(energy[c] / total)
                                         : 1.0 / std::sqrt(static_cast<double>(in.size()));
            sum += r * in[c][n];
        }
        out[0][n] = sum;
    }
    return out;
}

void Encoder::Impl::take(std::span<const std::vector<std::vector<double>>> programmes,
                         std::span<const std::vector<std::vector<double>>> stems) {
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        StreamSubstream& s = substreams[i];
        if (!s.enhances) {
            s.coder->take(programmes[i], stems[i]);
        }
    }
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        StreamSubstream& s = substreams[i];
        if (s.enhances) {
            s.coder->take(waveform(s, programmes[*s.enhances], stems[*s.enhances]), {});
        }
    }
    // Every substream's input is as long at the internal rate: the first's
    // with channels of its own says how long.
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        if (!substreams[i].enhances) {
            input_samples += static_cast<std::int64_t>(programmes[i].front().size());
            break;
        }
    }
}

std::vector<EncodedFrame> Encoder::Impl::drain() {
    std::vector<EncodedFrame> frames;
    for (;;) {
        const std::int64_t frame = frames_out;
        if (flushed && frame * timing.frame_length >=
                           input_samples + delay + timing.decoder_delay() + flush_extra) {
            break;
        }
        if (!flushed) {
            bool ready = true;
            for (const StreamSubstream& s : substreams) {
                ready = ready && s.coder->signal_end() >= s.coder->input_needed(frame);
            }
            for (const StreamPresentation& p : presentations) {
                for (const std::optional<detail::DrcGainEncoder>& encoder : p.drc_gain_encoders) {
                    if (encoder) {
                        const std::int64_t through =
                            kQmfSlot * encoder->slots_needed(frame) - timing.alignment_delay;
                        ready = ready && substreams[*p.anchor].coder->signal_end() >= through;
                    }
                }
            }
            if (!ready) {
                break;
            }
        }
        frames.push_back(encode_frame(frame));
        ++frames_out;
        for (StreamSubstream& s : substreams) {
            s.coder->after_frame(frames_out);
        }
    }
    return frames;
}

std::expected<std::unique_ptr<Encoder::Impl>, Refusal> Encoder::Impl::make(
    const EncoderConfig& config) {
    const auto invalid = [](Refusal why) { return std::unexpected(why); };
    if (config.sample_rate_hz != 48000 && config.sample_rate_hz != 44100) {
        return invalid("a sample rate other than 48 kHz or 44.1 kHz");
    }
    if (config.iframe_interval < 1) {
        return invalid("an I-frame interval below one frame");
    }
    if (config.bitrate_kbps < 8 || config.bitrate_kbps > 3000) {
        return invalid("a rate outside 8 to 3 000 kbps");
    }
    const auto dialnorm_ok = [](double db) { return db <= 0.0 && db >= -31.75; };
    if (!dialnorm_ok(config.dialnorm_db)) {
        return invalid("a dialnorm outside 0 to -31.75 dBFS");
    }
    const std::optional<detail::FrameTiming> timing =
        detail::frame_timing(config.frame_rate_index, config.sample_rate_hz);
    if (!timing) {
        return invalid(
            "a frame_rate_index Part 1 Table 83 does not give at the sample rate: 0 to 13 at 48 "
            "kHz, 13 alone at 44.1 kHz");
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->timing = *timing;
    impl->delay = timing->frame_length * 3 / 2;
    impl->fs_index = config.sample_rate_hz == 48000 ? 1 : 0;
    // The frames named as I-frames, and for each fragment start the first
    // frame whose output starts at or after it.
    for (const std::int64_t frame : config.iframes) {
        if (frame < 0) {
            return invalid("an I-frame named before frame 0");
        }
        impl->forced_iframes.push_back(frame);
    }
    for (const std::int64_t start : config.fragment_starts) {
        if (start < 0) {
            return invalid("a fragment starting before the output does");
        }
        std::int64_t frame = start * timing->decoder_down /
                             (static_cast<std::int64_t>(timing->frame_length) * timing->decoder_up);
        while (timing->output_before(frame) < start) {
            ++frame;
        }
        impl->forced_iframes.push_back(frame);
    }
    std::ranges::sort(impl->forced_iframes);
    const double internal_rate =
        static_cast<double>(config.sample_rate_hz) * timing->decoder_down / timing->decoder_up;
    impl->bytes_per_frame = static_cast<double>(config.bitrate_kbps) * 1000.0 *
                            timing->frame_length / (internal_rate * 8.0);
    if (config.rate_mode == RateMode::kAverage) {
        const bool fast = timing->frame_rate_index >= 10 && timing->frame_rate_index <= 12;
        impl->rate_low = fast ? 2.0 : 1.0;
        impl->rate_high = fast ? 11.0 : 5.0;
        impl->rate_level = (impl->rate_low + impl->rate_high) / 2.0 + 1.0;
    } else if (config.rate_mode == RateMode::kVariable) {
        impl->rate_high = 2.0 * internal_rate / timing->frame_length;
        impl->rate_low = -impl->rate_high;
        impl->rate_level = 1.0;
    }
    double octave = std::log2(static_cast<double>(config.bitrate_kbps));
    octave -= std::floor(octave);
    for (int digit = 0; digit < kBrCodeDigits; ++digit) {
        octave *= 3.0;
        const double whole = std::floor(octave);
        impl->br_codes.push_back(static_cast<int>(whole));
        octave -= whole;
    }
    if (timing->resampled()) {
        // The decoder's converter's delay, which flush() codes past as well.
        const detail::dsp::ResamplerFilter decoder(timing->decoder_up, timing->decoder_down);
        impl->flush_extra = static_cast<int>(std::ceil(decoder.delay()));
    }

    // --- The substreams and presentations the configuration names --------------
    //
    // With neither, one presentation of one complete main substream, as E1 to
    // E5 wrote; with presentations alone, they are of that one substream.
    std::vector<SubstreamConfig> subs = config.substreams;
    if (subs.empty()) {
        SubstreamConfig one;
        one.channels = config.channels;
        one.codec_mode = config.codec_mode;
        one.content = ContentClassifier::kCompleteMain;
        one.dialogue = config.dialogue;
        subs.push_back(std::move(one));
    }
    std::vector<PresentationConfig> presentations = config.presentations;
    if (presentations.empty()) {
        if (subs.size() != 1) {
            return invalid("several substreams and no presentation to play them");
        }
        presentations.push_back(PresentationConfig{});
        presentations.front().substreams = {0};
    }
    // CMAF's limit (Part 2 Annex H.1.2.1), and every substream played.
    if (presentations.size() > 64) {
        return invalid("more than CMAF's 64 presentations (Part 2 Annex H.1.2.1)");
    }
    const std::size_t n = subs.size();

    // Each substream's channel mode, as its coder would code it: what the
    // rules below take (a dialogue enhancement substream's follows its method).
    const auto channels_of = [&subs](std::size_t i) -> std::optional<int> {
        const SubstreamConfig& s = subs[i];
        if (!s.enhances) {
            return s.channels;
        }
        if (*s.enhances < 0 || static_cast<std::size_t>(*s.enhances) >= subs.size() ||
            subs[static_cast<std::size_t>(*s.enhances)].enhances) {
            return std::nullopt;
        }
        const std::optional<DialogueConfig>& dc =
            subs[static_cast<std::size_t>(*s.enhances)].dialogue;
        if (!dc || !dc->hybrid) {
            return std::nullopt;
        }
        if (dc->method != DialogueMethod::kChannelIndependent) {
            return 1;
        }
        return (dc->left ? 1 : 0) + (dc->right ? 1 : 0) + (dc->centre ? 1 : 0);
    };
    std::vector<int> ch_modes(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::optional<int> channels = channels_of(i);
        if (!channels) {
            return invalid(
                "a dialogue enhancement substream for no substream, for another dialogue "
                "enhancement substream, or for one without a hybrid method");
        }
        switch (*channels) {
            case 1:
                ch_modes[i] = 0;
                break;
            case 2:
                ch_modes[i] = 1;
                break;
            case 3:
                ch_modes[i] = 2;
                break;
            case 5:
                ch_modes[i] = 3;
                break;
            case 6:
                ch_modes[i] = 4;
                break;
            case 7:
            case 8: {
                const AdditionalPair pair = config.experimental.seven_x;
                if (pair == AdditionalPair::kNone) {
                    return invalid(
                        "seven or eight channels without experimental.seven_x's additional pair");
                }
                const int base =
                    pair == AdditionalPair::kBack ? 5 : (pair == AdditionalPair::kWide ? 7 : 9);
                ch_modes[i] = base + (*channels == 8 ? 1 : 0);
                break;
            }
            default:
                return invalid(
                    "a substream of a channel count the encoder does not take: 1, 2, 5 or 6, and "
                    "3, 7 or 8 as experimental layouts");
        }
        const SubstreamConfig& s = subs[i];
        if (s.language.size() > 63) {
            return invalid("a language tag longer than 63 bytes");
        }
        if (!s.language.empty() && !s.content) {
            return invalid("a language without a content classifier to carry it");
        }
        if (s.bitrate_kbps && *s.bitrate_kbps < 1) {
            return invalid("a substream's rate below 1 kbps");
        }
    }
    // One waveform for each hybrid dialogue enhancement at most.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (subs[i].enhances && subs[i].enhances == subs[j].enhances) {
                return invalid("two dialogue enhancement substreams for one substream");
            }
        }
    }

    // Each presentation's substreams, in the roles Table 53 gives its
    // configuration's positions or Table 54 each group's classifier.
    std::vector<bool> played(n, false);
    std::vector<bool> dialogue(n, false);  // a dialogue substream in some presentation
    bool downmix_sent = false;
    std::vector<bool> three_zero_dialogue(
        n, false);  // 3.0 dialogue of a music and effects presentation
    std::vector<std::optional<int>> ids;
    // A presentation_id left unset is the least no other presentation takes.
    std::vector<int> named_ids;
    for (const PresentationConfig& pc : presentations) {
        if (pc.presentation_id) {
            named_ids.push_back(*pc.presentation_id);
        }
    }
    int next_id = 0;
    for (const PresentationConfig& pc : presentations) {
        StreamPresentation p;
        p.toc.presentation_config = pc.config;
        if (pc.config == 6) {
            // EMDF payloads alone (Table 53): no substreams, no presentation
            // substream, and no presentation_id in the syntax.
            if (!pc.substreams.empty() || pc.emdf.empty() || !pc.name.empty() ||
                pc.presentation_id || pc.md_compat || pc.enabled || pc.pre_virtualized ||
                !pc.gains_db.empty() || pc.associated) {
                return invalid(
                    "an EMDF-only presentation (configuration 6) with substreams, an id, a level, "
                    "a name, a filter, gains or mixing values, or without payloads");
            }
            for (const EmdfPayload& payload : pc.emdf) {
                const auto codes = detail::resolve_emdf(payload);
                if (!codes) {
                    return invalid(
                        "an EMDF payload with an id below 1 or a field outside Part 1 Table 79's "
                        "range");
                }
                p.emdf.push_back(*codes);
            }
            impl->presentations.push_back(std::move(p));
            ids.emplace_back();
            continue;
        }
        const std::size_t count = pc.substreams.size();
        const std::size_t wanted = !pc.config                           ? 1
                                   : *pc.config == 3 || *pc.config == 4 ? 3
                                   : *pc.config == 5 ? std::max<std::size_t>(count, 2)
                                                     : 2;
        if (pc.config && (*pc.config < 0 || *pc.config > 5)) {
            return invalid("a presentation_config outside Part 2 Table 53's 0 to 6");
        }
        if (count != wanted) {
            return invalid(
                "a presentation of more or fewer substreams than its configuration plays");
        }
        std::vector<Role> roles;
        for (std::size_t position = 0; position < count; ++position) {
            const int index = pc.substreams[position];
            if (index < 0 || static_cast<std::size_t>(index) >= n ||
                std::ranges::count(pc.substreams, index) != 1) {
                return invalid("a presentation naming a substream the stream lacks, or one twice");
            }
            const auto i = static_cast<std::size_t>(index);
            Role role = Role::kMain;
            if (pc.config) {
                switch (*pc.config) {
                    case 0:
                        role = position == 0 ? Role::kMusicAndEffects : Role::kDialogue;
                        break;
                    case 1:
                        role = position == 0 ? Role::kMain : Role::kEnhancement;
                        break;
                    case 2:
                        role = position == 0 ? Role::kMain : Role::kAssociated;
                        break;
                    case 3:
                        role = position == 0
                                   ? Role::kMusicAndEffects
                                   : (position == 1 ? Role::kDialogue : Role::kAssociated);
                        break;
                    case 4:
                        role = position == 0
                                   ? Role::kMain
                                   : (position == 1 ? Role::kEnhancement : Role::kAssociated);
                        break;
                    default:
                        // Table 54: each group's content classifier.
                        if (!subs[i].content) {
                            return invalid(
                                "a configuration 5 presentation's substream without a content "
                                "classifier to give its role (Part 2 Table 54)");
                        }
                        role = role_from_classifier(*subs[i].content);
                        break;
                }
            }
            // A dialogue enhancement substream takes that role alone, or
            // plays alone; and one it takes, for the main substream it
            // enhances.
            if (role == Role::kEnhancement) {
                if (subs[i].enhances != pc.substreams[0]) {
                    return invalid(
                        "a dialogue enhancement position whose substream does not enhance the "
                        "presentation's main");
                }
            } else if (subs[i].enhances && pc.config) {
                return invalid("a dialogue enhancement substream in a role other than its own");
            }
            roles.push_back(role);
            played[i] = true;
            dialogue[i] = dialogue[i] || role == Role::kDialogue;
            p.members.push_back(i);
        }
        // The main or music and effects substream the others are mixed into.
        for (std::size_t m = 0; m < count && !p.anchor; ++m) {
            if (roles[m] == Role::kMain || roles[m] == Role::kMusicAndEffects) {
                p.anchor = p.members[m];
            }
        }
        if (!p.anchor) {
            return invalid("a presentation without main or music and effects audio");
        }
        const std::uint32_t anchor_channels =
            kModeChannels[static_cast<std::size_t>(ch_modes[*p.anchor])];
        bool associated = false;
        bool music_and_effects = false;
        for (std::size_t m = 0; m < count; ++m) {
            music_and_effects = music_and_effects ||
                                (p.members[m] == *p.anchor && roles[m] == Role::kMusicAndEffects);
        }
        int tracks = 0;
        int pres_ch_mode = -1;
        for (std::size_t m = 0; m < count; ++m) {
            const std::size_t i = p.members[m];
            const int mode = ch_modes[i];
            const std::uint32_t own = kModeChannels[static_cast<std::size_t>(mode)];
            // Part 1 clause 6.2.16.0: dialogue and associated audio add no
            // channel the main or music and effects substream lacks, but for
            // a mono one.
            if ((roles[m] == Role::kDialogue || roles[m] == Role::kAssociated) && mode != 0 &&
                (own & ~anchor_channels) != 0) {
                return invalid(
                    "dialogue or associated audio with a channel its main or music and effects "
                    "audio lacks (Part 1 clause 6.2.16.0)");
            }
            // Part 1 clause 4.3.3.7.1: 3.0 codes a dialogue enhancement signal,
            // or the dialogue of a music and effects presentation, alone; the
            // dialogue may also play by itself (src/ac4enc/ERRATA.md, "3.0
            // substreams").
            const bool three_dialogue = roles[m] == Role::kDialogue && music_and_effects;
            if (mode == 2 && !subs[i].enhances && !three_dialogue && pc.config) {
                return invalid(
                    "3.0 audio in a role other than dialogue enhancement or a music and effects "
                    "presentation's dialogue (Part 1 clause 4.3.3.7.1)");
            }
            three_zero_dialogue[i] = three_zero_dialogue[i] || three_dialogue;
            associated = associated || roles[m] == Role::kAssociated;
            tracks += static_cast<int>(std::popcount(own & ~kLfe));
            pres_ch_mode = superset(pres_ch_mode, mode);
        }
        if (pres_ch_mode < 0) {
            return invalid("substreams no channel mode holds together");
        }
        p.pres_ch_mode = pres_ch_mode;
        p.pres_has_lfe = mode_has_lfe(pres_ch_mode);
        // Part 2 Table 55: the least level its tracks allow, or one above.
        const int least = least_md_compat(tracks);
        const int md_compat = pc.md_compat.value_or(least);
        if (md_compat < least || (md_compat > 3 && md_compat != 7)) {
            return invalid(
                "an md_compat below the least its tracks need, or in 4 to 6 (Part 2 Table 55)");
        }
        if (!pc.presentation_id) {
            while (std::ranges::find(named_ids, next_id) != named_ids.end()) {
                ++next_id;
            }
        }
        const int id = pc.presentation_id ? *pc.presentation_id : next_id++;
        if (id < 0) {
            return invalid("a presentation_id below 0");
        }
        ids.emplace_back(id);
        p.toc.groups.assign(pc.substreams.begin(), pc.substreams.end());
        p.toc.md_compat = md_compat;
        p.toc.presentation_id = id;
        p.toc.enable = pc.enabled;
        p.toc.pre_virtualized = pc.pre_virtualized;
        // An alternative presentation's name: UTF-8 of at most 31 bytes, each
        // one not 0, sent whole with the 0 that says so.
        if (!pc.name.empty()) {
            if (pc.name.size() > 31 || std::ranges::find(pc.name, '\0') != pc.name.end()) {
                return invalid(
                    "an alternative presentation's name longer than 31 bytes or holding a 0 byte");
            }
            detail::AlternativeCodes alternative;
            for (const char c : pc.name) {
                alternative.name.push_back(static_cast<std::uint8_t>(c));
            }
            alternative.target_level = md_compat;
            alternative.substreams = static_cast<int>(count);
            p.alternative = alternative;
            p.toc.alternative = true;
        }
        // Its own values, or the stream's.
        p.dialnorm_db = pc.dialnorm_db.value_or(config.dialnorm_db);
        if (!dialnorm_ok(p.dialnorm_db)) {
            return invalid("a presentation's dialnorm outside 0 to -31.75 dBFS");
        }
        p.dialnorm_bits = static_cast<int>(std::lround(-p.dialnorm_db * 4.0));
        if (const std::optional<FurtherLoudness>& l = pc.loudness ? pc.loudness : config.loudness;
            l) {
            p.loudness = detail::resolve_loudness(*l);
            if (!p.loudness) {
                return invalid(
                    "a further loudness value outside further_loudness_info()'s codes, or a "
                    "correction without a practice");
            }
        }
        if (const std::optional<DrcConfig>& d = pc.drc ? pc.drc : config.drc; d) {
            p.drc = detail::resolve_drc(*d, config.experimental.drc_gains);
            if (!p.drc) {
                return invalid(
                    "a DRC mode past Part 1 Table 161's eight or named twice, a repeat of no mode, "
                    "an output level range outside 0 to -31 dBFS, or gains without "
                    "experimental.drc_gains");
            }
        }
        // The downmix's values go where the presentation's channel mode sends
        // them: set for one that does not, they are refused; the stream's
        // go to those that do.
        if (pc.downmix || (config.downmix && pres_ch_mode >= 3)) {
            p.downmix =
                detail::resolve_downmix(pc.downmix ? *pc.downmix : *config.downmix, pres_ch_mode);
            if (!p.downmix) {
                return invalid(
                    "downmix values for a presentation below 5.X, or a gain, an LFE gain or a "
                    "correction off its table's steps");
            }
            downmix_sent = downmix_sent || !pc.downmix;
        }
        // The mixing values (Part 2 clause 6.2.2.3): a gain for each group
        // clause 6.2.1.3's n_substream_groups counts (none for configuration
        // 1, the main and associated groups' for 4), and the associated
        // audio's.
        const int groups_sent = !pc.config || *pc.config == 1 ? 1
                                : *pc.config == 3             ? 3
                                : *pc.config == 5             ? static_cast<int>(count)
                                                              : 2;
        p.mix.n_substream_groups = groups_sent;
        if (!pc.gains_db.empty()) {
            if (pc.gains_db.size() != count) {
                return invalid(
                    "group gains that are not one for each of a presentation's substreams");
            }
            std::vector<int> codes;
            for (std::size_t m = 0; m < count; ++m) {
                const std::optional<int> code = group_gain_code(pc.gains_db[m]);
                if (!code) {
                    return invalid(
                        "a group gain off sg_gain's steps: 0 to -15.5 dB in steps of 0.25, or "
                        "-infinity");
                }
                const bool sent = groups_sent > 1 && roles[m] != Role::kEnhancement;
                if (!sent && *code != 0) {
                    return invalid(
                        "a group gain where the syntax sends none: configuration 1's, and "
                        "configuration 4's dialogue enhancement's");
                }
                if (sent) {
                    codes.push_back(*code);
                }
            }
            if (std::ranges::any_of(codes, [](int c) { return c != 0; })) {
                p.mix.sg_gain = codes;
            }
        }
        if (pc.associated) {
            if (!associated) {
                return invalid(
                    "associated audio's mixing values for a presentation without associated audio");
            }
            detail::AssociatedMixCodes a;
            const auto scaled = [](const std::optional<double>& db, std::optional<int>& to) {
                if (db) {
                    to = scale_code(*db);
                    return to.has_value();
                }
                return true;
            };
            if (!scaled(pc.associated->main_db, a.scale_main) ||
                !scaled(pc.associated->main_centre_db, a.scale_main_centre) ||
                !scaled(pc.associated->main_front_db, a.scale_main_front)) {
                return invalid(
                    "a main audio scaling off its steps: 0 to -76.2 dB in steps of 0.3, or "
                    "-infinity");
            }
            if (pc.associated->pan_degrees) {
                // pan_associated is sent for a mono associated substream.
                const auto described = std::ranges::find(roles, Role::kAssociated);
                const std::size_t i =
                    p.members[static_cast<std::size_t>(described - roles.begin())];
                a.pan_associated = detail::pan_code(*pc.associated->pan_degrees);
                if (ch_modes[i] != 0 || !a.pan_associated) {
                    return invalid(
                        "a pan for associated audio that is not mono, or off its steps of 1.5 "
                        "degrees");
                }
            }
            p.mix.associated = a;
        }
        for (const EmdfPayload& payload : pc.emdf) {
            const auto codes = detail::resolve_emdf(payload);
            if (!codes) {
                return invalid(
                    "an EMDF payload with an id below 1 or a field outside Part 1 Table 79's "
                    "range");
            }
            p.emdf.push_back(*codes);
        }
        impl->presentations.push_back(std::move(p));
    }
    if (std::ranges::find(played, false) != played.end()) {
        return invalid("a substream no presentation plays");
    }
    if (config.downmix && !downmix_sent) {
        return invalid("the stream's downmix values, and no 5.X or 7.X presentation to send them");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (ch_modes[i] == 2 && !subs[i].enhances && !three_zero_dialogue[i]) {
            return invalid(
                "3.0 audio that is neither a dialogue enhancement signal nor the dialogue of a "
                "music and effects presentation");
        }
    }
    // CMAF (Part 2 Annex H.1.2.1): no two presentations with one
    // presentation_id.
    for (std::size_t a = 0; a < ids.size(); ++a) {
        for (std::size_t b = a + 1; b < ids.size(); ++b) {
            if (ids[a] && ids[a] == ids[b]) {
                return invalid(
                    "two presentations with one presentation_id (CMAF, Part 2 Annex H.1.2.1)");
            }
        }
    }

    // --- The coders -------------------------------------------------------------
    //
    // Each substream's share of the rate: its own where it sets one, and what
    // those leave shared by the others in proportion to their full-band
    // channels.
    double set_kbps = 0.0;
    double unset_channels = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const int full = static_cast<int>(
            std::popcount(kModeChannels[static_cast<std::size_t>(ch_modes[i])] & ~kLfe));
        if (subs[i].bitrate_kbps) {
            set_kbps += *subs[i].bitrate_kbps;
        } else {
            unset_channels += full;
        }
    }
    if (set_kbps > config.bitrate_kbps ||
        (unset_channels > 0.0 && set_kbps >= config.bitrate_kbps)) {
        return invalid("substream rates that leave nothing of the stream's rate for the others");
    }
    impl->input_channels = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const SubstreamConfig& s = subs[i];
        StreamSubstream stream_sub;
        const int full = static_cast<int>(
            std::popcount(kModeChannels[static_cast<std::size_t>(ch_modes[i])] & ~kLfe));
        stream_sub.weight = s.bitrate_kbps
                                ? static_cast<double>(*s.bitrate_kbps)
                                : (config.bitrate_kbps - set_kbps) * full / unset_channels;
        stream_sub.enhances =
            s.enhances ? std::optional<std::size_t>{static_cast<std::size_t>(*s.enhances)}
                       : std::nullopt;
        stream_sub.content_classifier =
            s.content ? std::optional<int>{static_cast<int>(*s.content)} : std::nullopt;
        stream_sub.language = s.language;
        if (!s.enhances) {
            stream_sub.first_input = impl->input_channels;
            stream_sub.inputs = s.channels;
            impl->input_channels += static_cast<std::size_t>(s.channels);
        }
        impl->substreams.push_back(std::move(stream_sub));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const SubstreamConfig& s = subs[i];
        // The substream alone, as a coder takes it.
        EncoderConfig one = config;
        one.channels = *channels_of(i);
        one.codec_mode = s.codec_mode;
        one.bitrate_kbps = std::max(1, static_cast<int>(std::lround(impl->substreams[i].weight)));
        one.dialogue = s.enhances ? std::nullopt : s.dialogue;
        one.loudness.reset();
        one.drc.reset();
        one.downmix.reset();
        one.substreams.clear();
        one.presentations.clear();
        one.trace = {};
        if (one.dialogue && one.dialogue->source == DialogueSource::kStem) {
            impl->stem = true;
        }
        std::vector<std::unique_ptr<SubstreamCoder>> candidates;
        std::optional<Refusal> refused;  // kAuto's first mode's reason, where none takes it
        for (const CodecMode mode : modes_for(one)) {
            auto coder = SubstreamCoder::make(one, mode, !s.enhances);
            if (coder) {
                candidates.push_back(std::move(*coder));
            } else if (s.codec_mode != CodecMode::kAuto) {
                return std::unexpected(coder.error());
            } else if (!refused) {
                refused = coder.error();
            }
        }
        if (candidates.empty()) {
            return invalid(refused.value_or("a substream no codec mode codes"));
        }
        // A dialogue substream's mixing values (b_dialog), for a substream
        // that is dialogue somewhere or is classified so; and its payloads.
        const bool is_dialogue = dialogue[i] || s.content == ContentClassifier::kDialogue;
        if (s.dialogue_mix && !is_dialogue) {
            return invalid("dialogue mixing values for a substream that is not dialogue");
        }
        std::optional<detail::DialogueMixCodes> mix;
        if (is_dialogue) {
            mix = detail::resolve_dialogue_mix(s.dialogue_mix.value_or(DialogueMix{}), ch_modes[i]);
            if (!mix) {
                return invalid(
                    "a dialogue gain cap other than 3, 6, 9 or 12 dB, or pans that are not one a "
                    "channel of mono or stereo dialogue in steps of 1.5 degrees");
            }
        }
        std::vector<detail::EmdfPayloadCodes> payloads;
        for (const EmdfPayload& payload : s.emdf) {
            const auto codes = detail::resolve_emdf(payload);
            if (!codes) {
                return invalid(
                    "an EMDF payload with an id below 1 or a field outside Part 1 Table 79's "
                    "range");
            }
            payloads.push_back(*codes);
        }
        for (std::unique_ptr<SubstreamCoder>& coder : candidates) {
            coder->dialogue_mix = mix;
            coder->emdf = payloads;
        }
        impl->substreams[i].coder = std::move(candidates.front());
        // The coders kAuto tries next, kept until the rate is known to hold
        // the first's least frame.
        candidates.erase(candidates.begin());
        impl->fallbacks.push_back(std::move(candidates));
    }
    // The slack: the substream with the most of the rate.
    for (std::size_t i = 1; i < n; ++i) {
        if (impl->substreams[i].weight > impl->substreams[impl->slack].weight) {
            impl->slack = i;
        }
    }

    // DRC gains, where a presentation's modes send them, from its main or
    // music and effects substream's input.
    for (StreamPresentation& p : impl->presentations) {
        if (!p.drc || !p.drc->gains) {
            continue;
        }
        SubstreamCoder& anchor = *impl->substreams[*p.anchor].coder;
        if (impl->substreams[*p.anchor].enhances) {
            return invalid(
                "DRC gains for a presentation whose main audio is a dialogue enhancement "
                "substream");
        }
        const std::vector<detail::DrcChannel> drc_channels =
            drc_channels_of(anchor.config.channels, config.experimental.seven_x);
        const bool small = anchor.config.channels <= 2;
        for (const detail::DrcModeCodes& drc_mode : p.drc->modes) {
            detail::DrcModeGains zero;
            if (drc_mode.gains_config) {
                const int gains = *drc_mode.gains_config;
                zero.groups = gains > 0 && !small ? 3 : 1;
                zero.subframes = gains > 0 ? detail::drc_subframes(timing->frame_length) : 1;
                zero.bands = gains == 2 ? 2 : (gains == 3 ? 4 : 1);
                zero.gain.assign(
                    static_cast<std::size_t>(zero.groups * zero.subframes * zero.bands), 0);
            }
            if (drc_mode.gains_config && !drc_mode.repeat_id) {
                p.drc_gain_encoders.emplace_back(std::in_place,
                                                 detail::drc_gain_curve(drc_mode.gains_curve),
                                                 *drc_mode.gains_config, drc_channels, small,
                                                 *timing, anchor.rate_hz, p.dialnorm_db);
            } else {
                p.drc_gain_encoders.emplace_back();
            }
            p.drc_gains.push_back(std::move(zero));
        }
        // A repeat of a mode that sends gains sends them too.
        for (std::size_t m = 0; m < p.drc->modes.size(); ++m) {
            const detail::DrcModeCodes& drc_mode = p.drc->modes[m];
            if (drc_mode.repeat_id) {
                for (std::size_t other = 0; other < p.drc->modes.size(); ++other) {
                    if (p.drc->modes[other].id == *drc_mode.repeat_id) {
                        p.drc_gains[m] = p.drc_gains[other];
                    }
                }
            }
        }
        p.least_drc_gains = p.drc_gains;
        anchor.keep_input();
    }

    // --- The table of contents --------------------------------------------------
    //
    // The presentation substreams first, in the presentations' order, then
    // the audio substreams, each in a group of its own, then the EMDF payload
    // substreams.
    std::size_t index = 0;
    for (StreamPresentation& p : impl->presentations) {
        if (p.toc.presentation_config != 6) {
            p.toc.presentation_substream = static_cast<int>(index++);
        }
    }
    impl->first_audio = index;
    impl->first_emdf = index + n;
    for (StreamPresentation& p : impl->presentations) {
        if (!p.emdf.empty()) {
            const int emdf_index =
                static_cast<int>(impl->first_emdf + impl->emdf_substreams.size());
            impl->emdf_substreams.push_back(p.emdf);
            if (p.toc.presentation_config == 6) {
                p.toc.add_emdf = {emdf_index};
            } else {
                p.toc.emdf_substream = emdf_index;
            }
        }
    }
    impl->layout.fs_index = impl->fs_index;
    impl->layout.frame_rate_index = timing->frame_rate_index;
    for (const StreamPresentation& p : impl->presentations) {
        impl->layout.presentations.push_back(p.toc);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const StreamSubstream& s = impl->substreams[i];
        detail::TocGroup group;
        group.substreams.push_back(
            detail::TocSubstream{.ch_mode = ch_modes[i],
                                 .add_ch_base = false,
                                 .iframe = true,
                                 .substream_index = static_cast<int>(impl->first_audio + i)});
        group.content_classifier = s.content_classifier;
        group.language = s.language;
        impl->layout.groups.push_back(std::move(group));
    }

    // --- The least frame ----------------------------------------------------------
    //
    // The rate must hold a frame with no bands and no A-SPX energy in every
    // substream, in the smaller of the sizes it gives frames, whatever the
    // frame's blocks, with the presentation substreams an I-frame sends and
    // DRC's gains a stream starts from: that is what a frame falls back to.
    // kAuto takes, for each substream, the first of its modes that fits. The
    // table of contents is read back from that frame: what every frame
    // carries but its counter, its rate fields and its sizes.
    std::vector<BitWriter> fixed;
    for (const StreamPresentation& p : impl->presentations) {
        if (p.toc.presentation_config != 6) {
            fixed.push_back(impl->presentation_substream(p, true, true));
        }
    }
    for (const std::vector<detail::EmdfPayloadCodes>& payloads : impl->emdf_substreams) {
        fixed.push_back(detail::write_emdf_payloads_substream(payloads));
    }
    const auto frame_bytes = static_cast<std::size_t>(impl->bytes_per_frame);
    const int wait = config.rate_mode == RateMode::kConstant
                         ? 0
                         : (config.rate_mode == RateMode::kVariable ? 7 : 1);
    const detail::TocLayout least_layout = impl->layout_for(0, true, wait);
    for (StreamSubstream& s : impl->substreams) {
        s.coder->pending.fields = s.coder->fields_for(true);
    }
    impl->least_sizes.assign(n, 0);
    const auto sized = impl->sizes_for(least_layout, fixed, frame_bytes, {}, impl->least_sizes);
    if (!sized) {
        return invalid("a rate that cannot hold the presentation and EMDF payload substreams");
    }
    const std::vector<std::size_t>& sizes = sized->first;
    for (std::size_t i = 0; i < n; ++i) {
        StreamSubstream& s = impl->substreams[i];
        const std::size_t size = sizes[impl->first_audio + i];
        std::size_t next = 0;
        while (!s.coder->least_fits(size)) {
            if (next == impl->fallbacks[i].size()) {
                return invalid("a rate that cannot hold a substream's least frame");
            }
            s.coder = std::move(impl->fallbacks[i][next++]);
            s.coder->pending.fields = s.coder->fields_for(true);
        }
        // What an average rate may not take a substream below.
        impl->least_sizes[i] = s.coder->least_bytes();
    }
    impl->fallbacks.clear();
    std::vector<BitWriter> written(fixed.size() + n);
    for (std::size_t k = 0; k < fixed.size(); ++k) {
        written[impl->fixed_index(k)] = fixed[k];
    }
    for (std::size_t i = 0; i < n; ++i) {
        BitWriter audio = BitWriter::buffered();
        audio.write(1, 0, "b_tmp");  // any content: only the table of contents is read back
        written[impl->first_audio + i] =
            *detail::write_audio_substream(impl->substreams[i].coder->pending.fields, audio, 0);
    }
    const std::optional<std::vector<std::byte>> raw =
        detail::assemble(least_layout, written, 0, {});
    if (!raw) {
        return invalid("a least frame the writer cannot assemble");
    }
    const auto parsed = parse_raw_frame(*raw);
    if (!parsed) {
        return invalid("a least frame whose table of contents does not read back");
    }
    impl->toc = parsed->toc;
    // What the table of contents does not carry, for build_dac4(): whether
    // a presentation sends dialogue enhancement data, that none has
    // immersive audio, and an alternative presentation's name and its one
    // target, every device category at its md_compat.
    for (std::size_t p = 0; p < impl->toc.presentations_v1.size(); ++p) {
        PresentationInfoV1& presentation = impl->toc.presentations_v1[p];
        const StreamPresentation& configured = impl->presentations[p];
        bool de = false;
        for (const std::size_t m : configured.members) {
            de = de || impl->substreams[m].coder->metadata.de.has_value();
        }
        presentation.de_indicator = de;
        presentation.immersive_audio_indicator = false;
        if (configured.alternative) {
            AlternativeInfo alternative;
            for (const std::uint8_t byte : configured.alternative->name) {
                alternative.name.push_back(static_cast<char>(byte));
            }
            alternative.targets.push_back(AlternativeTarget{
                .md_compat = configured.alternative->target_level, .device_category = 0b1111});
            presentation.alternative_info = std::move(alternative);
        }
    }
    return impl;
}

std::expected<Encoder, EncodeError> Encoder::create(const EncoderConfig& config) {
    std::expected<std::unique_ptr<Impl>, Refusal> impl = Impl::make(config);
    if (!impl) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    return Encoder(std::move(*impl));
}

std::string_view Encoder::refusal_reason(const EncoderConfig& config) {
    const std::expected<std::unique_ptr<Impl>, Refusal> impl = Impl::make(config);
    return impl ? std::string_view{} : impl.error();
}

Encoder::Encoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::Impl::push(
    std::span<const std::span<const float>> channels,
    std::span<const std::span<const float>> dialogue) {
    if (flushed || channels.size() != input_channels || channels.empty() ||
        (stem && dialogue.size() != channels.size())) {
        return std::unexpected(EncodeError::kInvalidInput);
    }
    const std::size_t count = channels.front().size();
    for (const auto& input : {channels, dialogue}) {
        for (const std::span<const float> channel : input) {
            if (channel.size() != count) {
                return std::unexpected(EncodeError::kInvalidInput);
            }
            for (const float x : channel) {
                if (!std::isfinite(x)) {
                    return std::unexpected(EncodeError::kInvalidInput);
                }
            }
        }
    }
    std::vector<std::vector<std::vector<double>>> programmes(substreams.size());
    std::vector<std::vector<std::vector<double>>> stems(substreams.size());
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        StreamSubstream& s = substreams[i];
        if (s.enhances) {
            continue;
        }
        const auto inputs = static_cast<std::size_t>(s.inputs);
        programmes[i] =
            SubstreamCoder::internal(channels.subspan(s.first_input, inputs), s.coder->converters);
        if (s.coder->stem()) {
            stems[i] = SubstreamCoder::internal(dialogue.subspan(s.first_input, inputs),
                                                s.coder->stem_converters);
        }
    }
    take(programmes, stems);
    return drain();
}

std::vector<std::vector<double>> SubstreamCoder::internal(
    std::span<const std::span<const float>> input,
    std::vector<detail::dsp::Resampler<double>>& through) {
    std::vector<std::vector<double>> out(input.size());
    std::vector<double> samples;
    for (std::size_t c = 0; c < input.size(); ++c) {
        samples.assign(input[c].begin(), input[c].end());
        if (through.empty()) {
            out[c] = samples;
        } else {
            through[c].process(samples, out[c]);
        }
    }
    return out;
}

void SubstreamCoder::take(const std::vector<std::vector<double>>& programme_input,
                          const std::vector<std::vector<double>>& stem_input) {
    const std::size_t count = programme_input.front().size();
    if (!plan.acpl) {
        for (std::size_t c = 0; c < programme_input.size(); ++c) {
            signal[c].insert(signal[c].end(), programme_input[c].begin(), programme_input[c].end());
        }
    } else {
        // The A-CPL modes: the downmixes, the LFE and ASPX_ACPL_1's residuals
        // are coded, and A-CPL's analysis reads the channels it rebuilds.
        std::vector<double> input(plan.source.size());
        for (std::size_t n = 0; n < count; ++n) {
            for (std::size_t k = 0; k < input.size(); ++k) {
                input[k] = programme_input[static_cast<std::size_t>(plan.source[k])][n];
                source[k].push_back(input[k]);
            }
            const std::vector<double> coded = detail::acpl_downmix(*plan.acpl, input);
            for (std::size_t c = 0; c < coded.size(); ++c) {
                signal[c].push_back(coded[c]);
            }
            if (plan.lfe >= 0) {
                signal[static_cast<std::size_t>(plan.lfe)].push_back(
                    programme_input[static_cast<std::size_t>(plan.input_lfe)][n]);
            }
            if (!plan.residuals.empty()) {
                const std::vector<double> residuals = detail::acpl_residuals(*plan.acpl, input);
                for (std::size_t i = 0; i < residuals.size(); ++i) {
                    signal[static_cast<std::size_t>(plan.residuals[i])].push_back(residuals[i]);
                }
            }
        }
    }
    for (std::size_t c = 0; c < kept_input.size(); ++c) {
        kept_input[c].insert(kept_input[c].end(), programme_input[c].begin(),
                             programme_input[c].end());
    }
    if (stem()) {
        for (std::size_t i = 0; i < de_channels.size(); ++i) {
            const std::vector<double>& programme_channel = programme_input[de_channels[i]];
            const std::vector<double>& dialogue_channel = stem_input[de_channels[i]];
            de_programme[i].insert(de_programme[i].end(), programme_channel.begin(),
                                   programme_channel.end());
            de_dialogue[i].insert(de_dialogue[i].end(), dialogue_channel.begin(),
                                  dialogue_channel.end());
        }
    }
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels) {
    if (impl_->stem) {
        return std::unexpected(EncodeError::kInvalidInput);  // the stem goes with the programme
    }
    return impl_->push(channels, {});
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels,
    std::span<const std::span<const float>> dialogue) {
    if (!impl_->stem) {
        return std::unexpected(EncodeError::kInvalidInput);
    }
    return impl_->push(channels, dialogue);
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::flush() {
    Impl& impl = *impl_;
    if (impl.flushed) {
        return std::vector<EncodedFrame>{};
    }
    if (impl.timing.resampled()) {
        // What the converters hold back, flushed out with silence.
        std::vector<std::vector<std::vector<double>>> programmes(impl.substreams.size());
        std::vector<std::vector<std::vector<double>>> stems(impl.substreams.size());
        for (std::size_t i = 0; i < impl.substreams.size(); ++i) {
            StreamSubstream& s = impl.substreams[i];
            if (s.enhances) {
                continue;
            }
            SubstreamCoder& coder = *s.coder;
            const std::vector<float> zeros(
                static_cast<std::size_t>(coder.converters.front().filter().taps()), 0.0F);
            const std::vector<std::span<const float>> views(coder.converters.size(), zeros);
            programmes[i] = SubstreamCoder::internal(views, coder.converters);
            if (coder.stem()) {
                stems[i] = SubstreamCoder::internal(views, coder.stem_converters);
            }
        }
        impl.take(programmes, stems);
    }
    impl.flushed = true;
    return impl.drain();
}

const Toc& Encoder::toc() const noexcept {
    return impl_->toc;
}

CodecMode Encoder::codec_mode() const noexcept {
    return impl_->substreams.front().coder->plan.mode;
}

int Encoder::delay_samples() const noexcept {
    // The silence ahead of the input at the external rate, and where the
    // frame rate needs one the converter's delay.
    const detail::FrameTiming& t = impl_->timing;
    double delay = static_cast<double>(impl_->delay) * t.decoder_up / t.decoder_down;
    if (t.resampled()) {
        delay += detail::dsp::ResamplerFilter(t.decoder_down, t.decoder_up).delay();
    }
    return static_cast<int>(std::lround(delay));
}

int Encoder::decoder_delay_samples() const noexcept {
    // The decoder's delay at the internal rate and its converter's, which is
    // in the converter's input samples, both converted by its ratio.
    const detail::FrameTiming& t = impl_->timing;
    double delay = t.decoder_delay();
    if (t.resampled()) {
        delay += detail::dsp::ResamplerFilter(t.decoder_up, t.decoder_down).delay();
    }
    return static_cast<int>(std::lround(delay * t.decoder_up / t.decoder_down));
}

}  // namespace ac4
