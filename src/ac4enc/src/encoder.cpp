#include "ac4enc/encoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
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
#include "frame/dialogue.hpp"
#include "frame/frame_writer.hpp"
#include "frame/metadata.hpp"
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

constexpr int kFrameLength = 2048;  // frame_rate_index 13, at 44.1 and 48 kHz
// The silence ahead of the input: the frame's long window starts at its first
// output sample, so a frame codes input from half a frame before it to half a
// frame after it, and this delay puts the next frame's transients, which set
// the frame's last window, inside the input read before the frame is coded.
// A frame and a half, which is also what DEE's encoder gives.
constexpr int kDelay = kFrameLength * 3 / 2;
// The decoder's delay at index 13, which flush() codes enough frames to
// cover: d_pcm (Part 1 Table 188), the QMF banks' 577 samples and the six
// QMF slots the synthesis works behind (5.7.1).
constexpr int kDecoderDelay = 352 + 577 + 6 * 64;
constexpr int kQmfSlot = 64;        // samples per QMF slot
constexpr int kSubBlocks = 16;      // transient detection, a sixteenth of a frame each
constexpr int kSubBlock = kFrameLength / kSubBlocks;
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
// 5.1 streams send.
constexpr double kLfeCutoffHz = 120.0;
// sf_info_lfe()'s max_sfb: Part 1 Table 106's n_msfbl_bits at a transform of
// 2 048 samples.
constexpr int kLfeMaxSfbBits = 3;

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

[[nodiscard]] std::optional<Plan> plan_for(const EncoderConfig& config, CodecMode mode) {
    Plan p;
    p.mode = mode;
    p.coded = config.channels;
    const AdditionalPair pair = config.experimental.seven_x;
    const bool seven = config.channels == 7 || config.channels == 8;
    if (seven != (pair != AdditionalPair::kNone)) {
        return std::nullopt;
    }
    if (is_acpl(mode)) {
        // ASPX_ACPL_2 and 3 in the 5.X element; with experimental.acpl,
        // ASPX_ACPL_1 there, and ASPX_ACPL_1 and 2 in the channel pair.
        const bool five = config.channels == 5 || config.channels == 6;
        const bool options = config.experimental.acpl;
        const bool fits = five ? mode != CodecMode::kAspxAcpl1 || options
                               : config.channels == 2 && mode != CodecMode::kAspxAcpl3 && options;
        if (!fits || config.experimental.coding_configs) {
            return std::nullopt;
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
        case 5:
        case 6:
        case 7:
        case 8:
            break;
        default:
            return std::nullopt;
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

struct Encoder::Impl {
    // An encoder in `mode`, or kInvalidConfig where the configuration is not
    // one this version writes in it, or the rate cannot hold its least frame.
    [[nodiscard]] static std::expected<std::unique_ptr<Impl>, EncodeError> make(const EncoderConfig& config,
                                                                              CodecMode mode);

    EncoderConfig config{};
    Plan plan{};
    detail::Analysis analysis{kFrameLength, 1};
    detail::Psychoacoustics psycho{48000, kFrameLength};
    Toc toc{};
    int fs_index = 1;
    int dialnorm_bits = 124;
    double cutoff = 20000.0;
    double bytes_per_frame = 0.0;
    double byte_carry = 0.0;

    // The input, from sample index `base` of the delayed signal on; the first
    // kDelay samples of that signal are the silence ahead of the input.
    std::vector<std::vector<double>> signal;
    std::int64_t base = 0;
    std::int64_t input_samples = 0;
    bool flushed = false;

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

    [[nodiscard]] bool stem() const noexcept {
        return config.dialogue && config.dialogue->source == DialogueSource::kStem;
    }

    std::int64_t frames_out = 0;

    // Each layout group's transform layouts, decided for frames_out and the
    // frame after, and the length of the last window of the frame before.
    struct Group {
        std::vector<int> channels;
        bool lfe = false;  // sf_info_lfe(): always one long block
        std::deque<FrameLayout> layouts;
        int previous_last = kFrameLength;
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
        const double top = kAcplResidualQmfBand * static_cast<double>(config.sample_rate_hz) / 128.0;
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        if (*plan.acpl == detail::AcplLayout::kPair) {
            f.residual_max_sfb = {bands_below(first, top, config.sample_rate_hz),
                                  bands_below(last, top, config.sample_rate_hz)};
            return;
        }
        const int largest = std::max(first, last);
        f.residual_master = bands_below(largest, top, config.sample_rate_hz, (1 << detail::side_bits(largest)) - 1);
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
        const double crossover = aspx ? aspx->groups.sbx * static_cast<double>(config.sample_rate_hz) / 128.0 : 0.0;
        for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
            const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
            out[g].assign(bands, false);
            if (waveform_hz.empty()) {
                continue;
            }
            const int length = layout.group_length[g];
            const std::span<const std::uint16_t> offsets = detail::band_offsets(length);
            const double line_hz = static_cast<double>(config.sample_rate_hz) / (2.0 * length);
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
        while (acpl->slots() < detail::AcplEncoder::slots_needed(frame)) {
            const std::int64_t from = kQmfSlot * acpl->slots() - detail::kAnalysisLead;
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

    // Analyses the QMF slots frame f needs: its interval's and the six after
    // it that a variable border can reach, and with companding those whose
    // synthesis reaches the end of its transform window.
    void analyse_qmf(std::int64_t frame) {
        std::int64_t end = detail::kQmfSlotsPerFrame * (frame + 2);
        if (aspx->companding) {
            const std::int64_t window_last = (frame + 2) * kFrameLength - 1;
            end = std::max(end, (window_last + detail::kCompandedLag) / kQmfSlot + 1);
        }
        std::array<double, kQmfSlot> chunk{};
        for (std::size_t q = 0; q < qmf.size(); ++q) {
            const auto c = static_cast<std::size_t>(qmf_channel[q]);
            while (qmf[q].slots() < end) {
                const std::int64_t from = kQmfSlot * qmf[q].slots() - detail::kAnalysisLead;
                for (std::size_t i = 0; i < chunk.size(); ++i) {
                    chunk[i] = sample(c, from + static_cast<std::int64_t>(i));
                }
                qmf[q].push_slot(chunk);
            }
        }
    }

    // A frame's A-SPX data before the rate loop: each channel's proposal,
    // with companding as the stream has it; or with `fallback`, what costs
    // least (AspxChannelEncoder::fallback()).
    [[nodiscard]] AspxFrame aspx_frame(std::int64_t frame, bool iframe, std::optional<bool> fallback) {
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
                element.channels.push_back(fallback ? qmf[q].fallback(iframe, *fallback) : qmf[q].propose(frame, iframe));
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
            return detail::long_layout(kFrameLength);
        }
        const std::int64_t centre = frame * kFrameLength + kFrameLength / 2;
        std::array<double, kSubBlocks + 4> energy{};
        for (int k = -4; k < kSubBlocks; ++k) {
            double e = 0.0;
            for (const int channel : group.channels) {
                const auto c = static_cast<std::size_t>(channel);
                const std::int64_t start = centre + static_cast<std::int64_t>(k) * kSubBlock;
                for (std::int64_t s = start; s < start + kSubBlock; ++s) {
                    const double d = sample(c, s) - sample(c, s - 1);
                    e += d * d;
                }
            }
            energy[static_cast<std::size_t>(k + 4)] = e;
        }
        std::array<int, 2> attack{-1, -1};
        const double quietest = kAttackFloor * kSubBlock * static_cast<double>(group.channels.size());
        for (int k = 0; k < kSubBlocks; ++k) {
            const auto i = static_cast<std::size_t>(k + 4);
            const double before = (energy[i - 1] + energy[i - 2] + energy[i - 3] + energy[i - 4]) / 4.0;
            if (energy[i] > quietest && energy[i] > kAttackRatio * std::max(before, quietest)) {
                const auto half = static_cast<std::size_t>(k / (kSubBlocks / 2));
                if (attack[half] < 0) {
                    attack[half] = k % (kSubBlocks / 2);
                }
            }
        }
        if (attack[0] < 0 && attack[1] < 0) {
            return detail::long_layout(kFrameLength);
        }
        // An attack's half splits into eight blocks; the other half stays one
        // block of half the frame.
        const std::array<int, 2> transf_length{attack[0] >= 0 ? 0 : 3, attack[1] >= 0 ? 0 : 3};
        return detail::split_layout(kFrameLength, transf_length, attack);
    }

    [[nodiscard]] std::array<int, 2> max_sfb_for(const FrameLayout& layout, const Group& group) const {
        if (group.lfe) {
            const int bands = bands_below(kFrameLength, kLfeCutoffHz, config.sample_rate_hz, (1 << kLfeMaxSfbBits) - 1);
            return {bands, bands};
        }
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        return {bands_below(first, cutoff, config.sample_rate_hz), bands_below(last, cutoff, config.sample_rate_hz)};
    }

    [[nodiscard]] detail::FrameFields fields_for(std::int64_t frame) const {
        detail::FrameFields fields;
        fields.sequence_counter = sequence_counter(frame);
        fields.iframe = frame % config.iframe_interval == 0;
        fields.fs_index = fs_index;
        fields.frame_rate_index = 13;
        fields.ch_mode = plan.ch_mode;
        fields.dialnorm_bits = dialnorm_bits;
        fields.metadata = &metadata;
        fields.de = &de_current;
        fields.de_previous = de_sent ? &de_previous : nullptr;
        return fields;
    }

    // Dialogue enhancement's parameters for frame f from the stem: the
    // long-block spectra of each channel and of its dialogue over the frame's
    // transform window, which is centred where the decoder's interpolation
    // reaches the frame's parameters.
    void estimate_dialogue(std::int64_t frame) {
        const std::int64_t start = frame * kFrameLength;
        const FrameLayout layout = detail::long_layout(kFrameLength);
        std::vector<double> window(2 * kFrameLength);
        std::vector<double> programme;
        std::vector<double> dialogue;
        for (std::size_t i = 0; i < de_channels.size(); ++i) {
            for (const auto& [from, to] :
                 {std::pair{&de_programme[i], &programme}, std::pair{&de_dialogue[i], &dialogue}}) {
                for (std::size_t n = 0; n < window.size(); ++n) {
                    const std::int64_t s = start + static_cast<std::int64_t>(n);
                    window[n] = s >= base && s < signal_end()
                                    ? (*from)[static_cast<std::size_t>(s - base)]
                                    : 0.0;
                }
                analysis.transform(window, layout, kFrameLength, kFrameLength, *to);
            }
            de_current[i] = detail::de_parameters(programme, dialogue, kFrameLength);
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
                w.write(kLfeMaxSfbBits, static_cast<std::uint64_t>(max_sfb[0]), "max_sfb");
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

    [[nodiscard]] EncodedFrame encode_frame(std::int64_t frame) {
        const std::size_t channels = signal.size();
        const std::int64_t start = frame * kFrameLength;
        if (stem()) {
            estimate_dialogue(frame);
        }
        const detail::FrameFields fields = fields_for(frame);
        Coding f;
        f.iframe = fields.iframe;
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
            f.aspx = aspx_frame(frame, fields.iframe, std::nullopt);
        }
        if (acpl) {
            analyse_acpl(frame);
            f.acpl = acpl->propose(frame, fields.iframe);
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
            const double subband_hz = static_cast<double>(config.sample_rate_hz) / 128.0;
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
                f.max_sfb[g] = {std::max(f.max_sfb[g][0], bands_below(first_length, top, config.sample_rate_hz)),
                                std::max(f.max_sfb[g][1], bands_below(last_length, top, config.sample_rate_hz))};
            }
        }

        limit_residuals(f);
        std::vector<detail::Channel> spectra(channels);
        std::vector<std::vector<std::vector<bool>>> silenced(channels);
        std::vector<double> window(2 * kFrameLength);
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
        std::vector<std::size_t> order;
        for (const Unit& unit : f.structure.units) {
            for (const int c : unit.outputs) {
                order.push_back(static_cast<std::size_t>(c));
            }
        }

        // The frame's size, and the bits the channel element's sf_data() may
        // take.
        const double exact = byte_carry + bytes_per_frame;
        const auto frame_bytes = static_cast<std::size_t>(exact);
        const std::size_t overhead = detail::frame_overhead_bits(fields, frame_bytes);
        f.tracks.resize(channels);
        BitWriter side = BitWriter::buffered();
        write_element(side, f, false);
        const std::size_t side_bits = side.bit_position();
        const std::size_t budget =
            8 * frame_bytes > overhead + side_bits ? 8 * frame_bytes - overhead - side_bits : 0;

        // The rate loop: a level of noise per line, top_level * 10^(kStepDb p
        // / 10) at step p, and two laws that bring the bands' allowances to
        // it. Where the budget holds every band at its masking threshold (p =
        // 0), the bits left lower the level: each band whose allowance per
        // line is over it is brought kLevelWeight of the way to it, in dB, so
        // that the bits go first where the noise is loudest. Where it does
        // not, every band is pulled kLevelWeight of the way to the level from
        // above or below: the loudest bands keep noise under their thresholds,
        // as a waveform coder at a low rate must, and the quietest give up
        // theirs first. No band's noise is let past its energy, which would
        // leave a hole, until the last steps (kCapSteps on) relax that too.
        double top_level = 0.0;  // the highest allowance per line
        std::vector<std::vector<std::vector<double>>> energy(channels);
        for (const std::size_t c : order) {
            const detail::Channel& track = spectra[c];
            energy[c].resize(track.allowed.size());
            for (std::size_t g = 0; g < track.allowed.size(); ++g) {
                energy[c][g].assign(track.allowed[g].size(), 0.0);
                for (std::size_t b = 0; b < track.allowed[g].size(); ++b) {
                    if (silenced[c][g][b]) {
                        continue;
                    }
                    const std::size_t begin = track.grouped.offset[g][b];
                    const std::size_t end = track.grouped.offset[g][b + 1];
                    top_level = std::max(top_level, track.allowed[g][b] / static_cast<double>(end - begin));
                    for (std::size_t k = begin; k < end; ++k) {
                        energy[c][g][b] += track.grouped.lines[k] * track.grouped.lines[k];
                    }
                }
            }
        }
        std::vector<std::vector<std::vector<int>>> sf(channels);
        std::vector<std::vector<double>> capped;
        double kappa = kCaps.front();
        const auto set_step = [&](bool pull, int step) {
            const double level = top_level * std::pow(10.0, kStepDb * step / 10.0);
            const double cap = kappa * (step > kCapSteps ? std::pow(10.0, kStepDb * (step - kCapSteps) / 10.0) : 1.0);
            for (const std::size_t c : order) {
                const detail::Channel& track = spectra[c];
                capped = track.allowed;
                for (std::size_t g = 0; g < capped.size(); ++g) {
                    for (std::size_t b = 0; b < capped[g].size(); ++b) {
                        if (silenced[c][g][b]) {
                            continue;
                        }
                        const auto lines = static_cast<double>(track.grouped.offset[g][b + 1] - track.grouped.offset[g][b]);
                        double& allowance = capped[g][b];
                        if (!pull) {
                            if (allowance > level * lines) {
                                allowance *= std::pow(level * lines / allowance, kLevelWeight);
                            }
                            continue;
                        }
                        allowance *= std::pow(level * lines / allowance, kLevelWeight);
                        if (energy[c][g][b] > 0.0) {
                            allowance = std::min(allowance, cap * energy[c][g][b]);
                        }
                    }
                }
                sf[c] = detail::scale_factors_for(track.grouped, capped);
            }
        };
        const auto layout_of = [&](std::size_t c) -> const FrameLayout& { return f.layout[group_of[c]]; };
        const auto bits_at = [&](bool pull, int step) {
            set_step(pull, step);
            std::size_t total = 0;
            for (const std::size_t c : order) {
                total += detail::code_track(spectra[c].grouped, sf[c], 0, layout_of(c)).bits();
            }
            return total;
        };
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
            return detail::write_frame(fields, audio, frame_bytes, config.trace);
        };
        const auto write_at = [&](bool pull, int step) {
            set_step(pull, step);
            for (const std::size_t c : order) {
                f.tracks[c] = detail::code_track(spectra[c].grouped, sf[c], 0, layout_of(c));
            }
            return write();
        };
        std::optional<std::vector<std::byte>> raw;
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
            kappa = k;
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
                kappa = kCaps.back();
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
            // that cost least.
            // With A-CPL, the parameters as proposed, then those the decoder
            // holds already, which cost least.
            const std::optional<AspxFrame> proposed = std::move(f.aspx);
            std::optional<detail::AcplFrameFields> parameters = std::move(f.acpl);
            f = silent(fields.iframe, f.layout);
            for (const bool held : {false, true}) {
                if (held) {
                    if (!parameters) {
                        break;
                    }
                    parameters = acpl->held(fields.iframe);
                }
                f.acpl = parameters;
                f.aspx = proposed;
                raw = write();
                for (const bool silence : {false, true}) {
                    if (raw || !f.aspx) {
                        break;
                    }
                    f.aspx = aspx_frame(frame, fields.iframe, silence);
                    raw = write();
                }
                if (raw) {
                    break;
                }
            }
        }
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
        byte_carry = exact - static_cast<double>(frame_bytes);
        for (std::size_t g = 0; g < groups.size(); ++g) {
            groups[g].previous_last = f.layout[g].window_length.back();
        }
        if (metadata.de) {
            de_previous = de_current;
            de_sent = true;
        }
        EncodedFrame out;
        out.raw_ac4_frame = std::move(raw).value();
        out.samples = kFrameLength;
        out.iframe = fields.iframe;
        return out;
    }

    // Takes the input, and with a stem the dialogue in it, and returns the
    // frames it completes.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> push(
        std::span<const std::span<const float>> channels,
        std::span<const std::span<const float>> dialogue);

    // The frames the input read so far lets through. A frame needs the input
    // to half a frame past its window, where the next frame's transients are.
    [[nodiscard]] std::vector<EncodedFrame> drain() {
        std::vector<EncodedFrame> frames;
        for (;;) {
            const std::int64_t frame = frames_out;
            if (flushed && frame * kFrameLength >= input_samples + kDelay + kDecoderDelay) {
                break;
            }
            const std::int64_t needed = (frame + 2) * kFrameLength + kFrameLength / 2;
            if (!flushed && signal_end() < needed) {
                break;
            }
            for (Group& group : groups) {
                while (static_cast<std::int64_t>(group.layouts.size()) < 2) {
                    group.layouts.push_back(decide(frame + static_cast<std::int64_t>(group.layouts.size()), group));
                }
            }
            frames.push_back(encode_frame(frame));
            for (Group& group : groups) {
                group.layouts.pop_front();
            }
            ++frames_out;
            // Nothing before the next frame's window is read again.
            const std::int64_t keep_from = (frames_out * kFrameLength) - kSubBlock * 5;
            if (keep_from > base) {
                const auto drop = static_cast<std::size_t>(std::min(keep_from - base, signal_end() - base));
                for (std::vector<double>& channel : signal) {
                    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(drop));
                }
                for (std::vector<double>& channel : source) {
                    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(drop));
                }
                for (auto* buffers : {&de_programme, &de_dialogue}) {
                    for (std::vector<double>& channel : *buffers) {
                        channel.erase(channel.begin(),
                                      channel.begin() + static_cast<std::ptrdiff_t>(drop));
                    }
                }
                base += static_cast<std::int64_t>(drop);
            }
        }
        return frames;
    }
};

std::expected<std::unique_ptr<Encoder::Impl>, EncodeError> Encoder::Impl::make(const EncoderConfig& config,
                                                                                 CodecMode mode) {
    const std::optional<Plan> plan = plan_for(config, mode);
    if (!plan) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.sample_rate_hz != 48000 && config.sample_rate_hz != 44100) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.iframe_interval < 1 || config.bitrate_kbps < 8 || config.bitrate_kbps > 3000) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (!(config.dialnorm_db <= 0.0 && config.dialnorm_db >= -31.75)) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->plan = *plan;
    impl->psycho = detail::Psychoacoustics(config.sample_rate_hz, kFrameLength);
    impl->fs_index = config.sample_rate_hz == 48000 ? 1 : 0;
    impl->dialnorm_bits = static_cast<int>(std::lround(-config.dialnorm_db * 4.0));
    impl->bytes_per_frame = static_cast<double>(config.bitrate_kbps) * 1000.0 * kFrameLength /
                            (static_cast<double>(config.sample_rate_hz) * 8.0);
    const auto channels = static_cast<std::size_t>(plan->coded);
    const int full_channels = config.channels - (plan->lfe >= 0 ? 1 : 0);
    const double kbps_per_channel = static_cast<double>(config.bitrate_kbps) / full_channels;
    const bool multichannel = plan->ch_mode >= 3;
    impl->cutoff = cutoff_hz(kbps_per_channel);
    impl->group_of.assign(channels, 0);
    for (std::size_t g = 0; g < plan->groups.size(); ++g) {
        Impl::Group group;
        group.channels = plan->groups[g];
        group.lfe = group.channels.size() == 1 && group.channels.front() == plan->lfe;
        for (const int c : group.channels) {
            impl->group_of[static_cast<std::size_t>(c)] = g;
        }
        impl->groups.push_back(std::move(group));
    }
    if (plan->acpl) {
        if (*plan->acpl == detail::AcplLayout::kPair) {
            // As stereo is coded in the ASPX mode at the rate, without
            // companding, which DEE's A-CPL streams never turn on.
            impl->aspx = detail::aspx_setup_for(kbps_per_channel, config.sample_rate_hz, false);
            if (impl->aspx) {
                impl->aspx->companding = false;
            }
        } else {
            impl->aspx = detail::aspx_setup_for_acpl(*plan->acpl == detail::AcplLayout::kCoupling,
                                                     config.sample_rate_hz);
        }
        impl->acpl.emplace(*plan->acpl, kAcplBandsId, kAcplQuantMode,
                           plan->residuals.empty() ? 0 : kAcplResidualQmfBand);
        impl->source.assign(plan->source.size(), std::vector<double>(static_cast<std::size_t>(kDelay), 0.0));
    } else if (mode == CodecMode::kAspx) {
        impl->aspx = detail::aspx_setup_for(kbps_per_channel, config.sample_rate_hz, multichannel);
    }
    if (mode != CodecMode::kSimple) {
        if (!impl->aspx) {
            return std::unexpected(EncodeError::kInvalidConfig);
        }
        impl->aspx->varvar = config.experimental.aspx_varvar;
        impl->aspx->balance = config.experimental.aspx_balance;
        impl->aspx->interleave = config.experimental.aspx_interleave;
        impl->interleaved_prev.resize(channels);
        // The spectral frontend codes up to the crossover, subband sbx of 64
        // across half the sampling rate.
        impl->cutoff = static_cast<double>(impl->aspx->groups.sbx) * config.sample_rate_hz / 128.0;
        impl->qmf_of.assign(channels, -1);
        for (std::size_t c = 0; c < channels; ++c) {
            const bool coded = std::ranges::any_of(plan->aspx_elements, [&](const std::vector<int>& element) {
                return std::ranges::find(element, static_cast<int>(c)) != element.end();
            });
            if (!coded) {
                continue;
            }
            impl->qmf_of[c] = static_cast<int>(impl->qmf.size());
            impl->qmf_channel.push_back(static_cast<int>(c));
            impl->qmf.emplace_back(*impl->aspx);
        }
    }
    impl->signal.assign(channels, std::vector<double>(static_cast<std::size_t>(kDelay), 0.0));

    const std::optional<detail::StreamMetadata> metadata =
        detail::resolve_metadata(config, plan->ch_mode);
    if (!metadata) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    impl->metadata = *metadata;
    if (metadata->de) {
        // de_channel_config's L, R and C (Table 171) as input channels: C
        // alone in mono, L and R in stereo, and L, R and C the first three
        // otherwise.
        const int channel_config = metadata->de->channel_config;
        for (const auto& [bit, input] :
             {std::pair{4, 0}, std::pair{2, 1}, std::pair{1, config.channels == 1 ? 0 : 2}}) {
            if ((channel_config & bit) != 0) {
                impl->de_channels.push_back(static_cast<std::size_t>(input));
            }
        }
        if (impl->stem()) {
            impl->de_programme.assign(impl->de_channels.size(),
                                      std::vector<double>(static_cast<std::size_t>(kDelay), 0.0));
            impl->de_dialogue = impl->de_programme;
        } else {
            // Marked channels carry dialogue alone: 1 in every band.
            const int one = detail::de_parameter_index(1.0);
            for (std::size_t i = 0; i < impl->de_channels.size(); ++i) {
                impl->de_current[i].fill(one);
            }
        }
    }

    // The rate must hold a frame with no bands and no A-SPX energy, in the
    // smaller of the sizes it gives frames, whatever the frame's blocks: that
    // is what a frame falls back to. The table of contents is read back from
    // the first such frame: what every frame carries but its counter and
    // sizes.
    const detail::FrameFields fields = impl->fields_for(0);
    const auto frame_bytes = static_cast<std::size_t>(impl->bytes_per_frame);
    std::optional<Impl::AspxFrame> aspx_data;
    if (impl->aspx) {
        aspx_data = impl->aspx_frame(0, true, true);
    }
    std::optional<std::vector<std::byte>> raw;
    for (const FrameLayout& layout :
         {detail::long_layout(kFrameLength), detail::split_layout(kFrameLength, {0, 0}, {0, 0})}) {
        std::vector<FrameLayout> layouts;
        for (const Impl::Group& group : impl->groups) {
            layouts.push_back(group.lfe ? detail::long_layout(kFrameLength) : layout);
        }
        Impl::Coding silent = impl->silent(fields.iframe, layouts);
        silent.aspx = aspx_data;
        if (impl->acpl) {
            silent.acpl = impl->acpl->held(fields.iframe);
        }
        BitWriter audio = BitWriter::buffered();
        impl->write_element(audio, silent, true);
        auto written = detail::write_frame(fields, audio, frame_bytes, {});
        if (!written) {
            return std::unexpected(EncodeError::kInvalidConfig);  // the rate cannot hold a frame
        }
        if (!raw) {
            raw = std::move(written);
        }
    }
    const auto parsed = parse_raw_frame(*raw);
    if (!parsed) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    impl->toc = parsed->toc;
    // What the table of contents does not carry, for build_dac4(): whether
    // the stream sends dialogue enhancement data, and that it has no
    // immersive audio.
    for (PresentationInfoV1& presentation : impl->toc.presentations_v1) {
        presentation.de_indicator = impl->metadata.de.has_value();
        presentation.immersive_audio_indicator = false;
    }
    return impl;
}

std::expected<Encoder, EncodeError> Encoder::create(const EncoderConfig& config) {
    // kAuto codes in the mode the rate gives, or where the rate cannot hold
    // that mode's least frame, in the next of ASPX_ACPL_2 and ASPX that it
    // holds: ASPX_ACPL_3's least frame, with eleven parameters a band, needs
    // 25 kbps in 5.1 at 48 kHz, ASPX_ACPL_2's 15, and ASPX's 20.
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
    std::expected<std::unique_ptr<Impl>, EncodeError> impl = std::unexpected(EncodeError::kInvalidConfig);
    for (const CodecMode candidate : modes) {
        impl = Impl::make(config, candidate);
        if (impl) {
            break;
        }
    }
    if (!impl) {
        return std::unexpected(impl.error());
    }
    return Encoder(std::move(*impl));
}

Encoder::Encoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::Impl::push(
    std::span<const std::span<const float>> channels,
    std::span<const std::span<const float>> dialogue) {
    if (flushed || channels.size() != static_cast<std::size_t>(config.channels) ||
        (stem() && dialogue.size() != channels.size())) {
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
    if (!plan.acpl) {
        for (std::size_t c = 0; c < channels.size(); ++c) {
            std::vector<double>& buffer = signal[c];
            buffer.reserve(buffer.size() + count);
            for (const float x : channels[c]) {
                buffer.push_back(static_cast<double>(x));
            }
        }
    } else {
        // The A-CPL modes: the downmixes, the LFE and ASPX_ACPL_1's residuals
        // are coded, and A-CPL's analysis reads the channels it rebuilds.
        std::vector<double> input(plan.source.size());
        for (std::size_t n = 0; n < count; ++n) {
            for (std::size_t k = 0; k < input.size(); ++k) {
                input[k] = static_cast<double>(channels[static_cast<std::size_t>(plan.source[k])][n]);
                source[k].push_back(input[k]);
            }
            const std::vector<double> coded = detail::acpl_downmix(*plan.acpl, input);
            for (std::size_t c = 0; c < coded.size(); ++c) {
                signal[c].push_back(coded[c]);
            }
            if (plan.lfe >= 0) {
                signal[static_cast<std::size_t>(plan.lfe)].push_back(
                    static_cast<double>(channels[static_cast<std::size_t>(plan.input_lfe)][n]));
            }
            if (!plan.residuals.empty()) {
                const std::vector<double> residuals = detail::acpl_residuals(*plan.acpl, input);
                for (std::size_t i = 0; i < residuals.size(); ++i) {
                    signal[static_cast<std::size_t>(plan.residuals[i])].push_back(residuals[i]);
                }
            }
        }
    }
    if (stem()) {
        for (std::size_t i = 0; i < de_channels.size(); ++i) {
            for (const auto& [from, to] :
                 {std::pair{channels, &de_programme[i]}, std::pair{dialogue, &de_dialogue[i]}}) {
                for (const float x : from[de_channels[i]]) {
                    to->push_back(static_cast<double>(x));
                }
            }
        }
    }
    input_samples += static_cast<std::int64_t>(count);
    return drain();
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels) {
    if (impl_->stem()) {
        return std::unexpected(EncodeError::kInvalidInput);  // the stem goes with the programme
    }
    return impl_->push(channels, {});
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels,
    std::span<const std::span<const float>> dialogue) {
    if (!impl_->stem()) {
        return std::unexpected(EncodeError::kInvalidInput);
    }
    return impl_->push(channels, dialogue);
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::flush() {
    if (impl_->flushed) {
        return std::vector<EncodedFrame>{};
    }
    impl_->flushed = true;
    return impl_->drain();
}

const Toc& Encoder::toc() const noexcept {
    return impl_->toc;
}

CodecMode Encoder::codec_mode() const noexcept {
    return impl_->plan.mode;
}

int Encoder::delay_samples() const noexcept {
    return kDelay;
}

}  // namespace ac4
