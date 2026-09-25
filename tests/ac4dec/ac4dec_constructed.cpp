#include "ac4dec_constructed.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "ac4dec_printed_matrices.hpp"
#include "ac4enc/encoder.hpp"
#include "acpl/acpl_syntax.hpp"
#include "asf/analysis.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/stereo.hpp"
#include "aspx/aspx_encoder.hpp"
#include "aspx/aspx_syntax.hpp"
#include "bit_writer.hpp"
#include "frame/frame_writer.hpp"

namespace ac4dec_test {
namespace {

using ac4::Speaker;
using ac4::detail::AcplConfig1chFields;
using ac4::detail::AcplConfig2chFields;
using ac4::detail::AcplData1chFields;
using ac4::detail::AcplData2chFields;
using ac4::detail::AcplParamFields;
using ac4::detail::AspxChannelFields;
using ac4::detail::AspxSetup;
using ac4::detail::BitWriter;
using ac4::detail::CodedTrack;
using ac4::detail::FrameLayout;
using Lines = std::vector<double>;
using Matrix = std::vector<std::vector<double>>;

constexpr int kFrameLength = 2048;
constexpr int kRate = 48000;
constexpr double kAmplitude = 0.1;  // -20 dBFS
// Every tone is below the coded band's top, 1.5 kHz: lines of 11.7 Hz.
constexpr std::size_t kTopLine = 128;
// The LFE's max_sfb: n_msfbl_bits is 3 at 2 048 samples (Part 1 Table 106).
constexpr int kLfeMaxSfb = 7;
// A-SPX: 40 kbps a channel takes the high resolution table from 10.5 kHz.
constexpr double kAspxKbpsPerChannel = 40.0;
// A loud envelope's scale factors, in 1.5 dB steps, and its noise floor: at 0,
// Q = 2^6 and the noise carries the envelope's energy (Part 1 5.7.6.3.5). A
// silent one at 0 with no noise (29, the codebook's top).
constexpr int kLoudEnvelope = 40;
constexpr int kNoNoise = 29;
constexpr double kRoot2 = std::numbers::sqrt2;
// ASPX_ACPL_1's acpl_qmf_band: mid-side coded below 3 kHz, above every tone.
constexpr int kAcplQmfBand = 8;
// ASPX_ACPL_3's gamma1 and gamma4: 4 steps of 1 638 / 16 384, or 2 of 3 276
// (Table 208), 6 552 / 16 384 either way.
constexpr double kGamma = 6552.0 / 16384.0;

// Each channel's tone: gen_ac4_baseline.py's for L R C LFE Ls Rs, and the next
// two primes for the 7.X modes' last pair; none sits on another's harmonic.
[[nodiscard]] double tone_of(Speaker speaker) {
    switch (speaker) {
        case Speaker::kLeft:
            return 331.0;
        case Speaker::kRight:
            return 457.0;
        case Speaker::kCentre:
            return 613.0;
        case Speaker::kLfe:
            return 47.0;
        case Speaker::kLeftSurround:
            return 787.0;
        case Speaker::kRightSurround:
            return 953.0;
        case Speaker::kLeftBack:
        case Speaker::kLeftWide:
        case Speaker::kTopFrontLeft:
            return 1117.0;
        default:
            return 1289.0;
    }
}

[[nodiscard]] bool has_lfe(int ch_mode) {
    return ch_mode == 4 || ch_mode == 6 || ch_mode == 8 || ch_mode == 10;
}

// A 7.X mode's last pair (Table 88).
[[nodiscard]] std::pair<Speaker, Speaker> last_pair(int ch_mode) {
    if (ch_mode <= 6) {
        return {Speaker::kLeftBack, Speaker::kRightBack};
    }
    if (ch_mode <= 8) {
        return {Speaker::kLeftWide, Speaker::kRightWide};
    }
    return {Speaker::kTopFrontLeft, Speaker::kTopFrontRight};
}

// The decoder's channels for a mode, in its order: L R C, the LFE, Ls Rs, the
// last pair.
[[nodiscard]] std::vector<Speaker> speakers_for(int ch_mode) {
    if (ch_mode == 1) {
        return {Speaker::kLeft, Speaker::kRight};
    }
    std::vector<Speaker> out = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre};
    if (ch_mode == 2) {
        return out;
    }
    if (has_lfe(ch_mode)) {
        out.push_back(Speaker::kLfe);
    }
    out.push_back(Speaker::kLeftSurround);
    out.push_back(Speaker::kRightSurround);
    if (ch_mode >= 5) {
        const auto [left, right] = last_pair(ch_mode);
        out.push_back(left);
        out.push_back(right);
    }
    return out;
}

// Pseudocode 59's a, b, c and d for a sap_mode that sends nothing per band.
[[nodiscard]] Abcd parameters_of(int sap_mode) {
    return sap_mode == 2 ? Abcd{1.0, 1.0, 1.0, -1.0} : Abcd{1.0, 0.0, 0.0, 1.0};
}

// Gauss-Jordan with partial pivoting; the matrices here are cascades of
// invertible 2 x 2 steps.
[[nodiscard]] Matrix inverse(Matrix m) {
    const std::size_t n = m.size();
    Matrix out(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        out[i][i] = 1.0;
    }
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::abs(m[row][col]) > std::abs(m[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(m[pivot][col]) < 1e-12) {
            throw std::runtime_error("a matrix with no inverse");
        }
        std::swap(m[col], m[pivot]);
        std::swap(out[col], out[pivot]);
        const double scale = m[col][col];
        for (std::size_t k = 0; k < n; ++k) {
            m[col][k] /= scale;
            out[col][k] /= scale;
        }
        for (std::size_t row = 0; row < n; ++row) {
            if (row == col || m[row][col] == 0.0) {
                continue;
            }
            const double factor = m[row][col];
            for (std::size_t k = 0; k < n; ++k) {
                m[row][k] -= factor * m[col][k];
                out[row][k] -= factor * out[col][k];
            }
        }
    }
    return out;
}

// Tracks I = M^-1 O for outputs O.
[[nodiscard]] std::vector<Lines> tracks_for(const Matrix& m, const std::vector<const Lines*>& outputs) {
    const Matrix inv = inverse(m);
    std::vector<Lines> tracks(outputs.size(), Lines(outputs[0]->size(), 0.0));
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        for (std::size_t o = 0; o < outputs.size(); ++o) {
            if (inv[t][o] == 0.0) {
                continue;
            }
            for (std::size_t k = 0; k < tracks[t].size(); ++k) {
                tracks[t][k] += inv[t][o] * (*outputs[o])[k];
            }
        }
    }
    return tracks;
}

// One track coded finely: each band's step a 2^-12 of its peak.
[[nodiscard]] CodedTrack code(const Lines& lines, const FrameLayout& layout, int max_sfb) {
    const ac4::detail::Grouped grouped = ac4::detail::regroup(lines, layout, {max_sfb, max_sfb});
    std::vector<std::vector<int>> sf(grouped.offset.size());
    for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
        for (std::size_t b = 0; b + 1 < grouped.offset[g].size(); ++b) {
            double peak = 0.0;
            for (std::size_t k = grouped.offset[g][b]; k < grouped.offset[g][b + 1]; ++k) {
                peak = std::max(peak, std::abs(grouped.lines[k]));
            }
            // g = 2^((sf - 100) / 4) (Part 1 Pseudocode 21).
            const double step = peak > 0.0 ? peak / 4096.0 : 1.0;
            sf[g].push_back(static_cast<int>(std::lround(100.0 + 4.0 * std::log2(step))));
        }
    }
    return ac4::detail::code_track(grouped, sf, 0, layout);
}

class ElementWriter {
   public:
    ElementWriter(BitWriter& w, const std::map<Speaker, Lines>& lines, const ElementCase& c)
        : w_(w), lines_(lines), c_(c), layout_(ac4::detail::long_layout(kFrameLength)) {
        const auto offsets = ac4::detail::band_offsets(kFrameLength);
        while (max_sfb_ + 1 < static_cast<int>(offsets.size()) &&
               offsets[static_cast<std::size_t>(max_sfb_)] < kTopLine) {
            ++max_sfb_;
        }
    }

    // mono_data(1): sf_info_lfe() is max_sfb alone.
    void lfe() {
        w_.write(3, kLfeMaxSfb, "max_sfb");
        ac4::detail::write_sf_data(w_, code(lines_.at(Speaker::kLfe), layout_, kLfeMaxSfb), layout_);
    }

    // mono_data(0).
    void mono(Speaker speaker) {
        w_.write(1, 0, "spec_frontend");
        sf_info();
        sf_data(lines_.at(speaker));
    }

    // stereo_data(): the 3.0 element's pair, spec_frontend sent per track
    // when the tracks have their own sf_info().
    void stereo_data(Speaker a, Speaker b) {
        w_.write(1, c_.stereo_proc ? 1U : 0U, "b_enable_mdct_stereo_proc");
        if (c_.stereo_proc) {
            sf_info();
            chparam(c_.sap_mode);
        } else {
            w_.write(1, 0, "spec_frontend_l");
            sf_info();
            w_.write(1, 0, "spec_frontend_r");
            sf_info();
        }
        pair_tracks(a, b);
    }

    void two_channel_data(Speaker a, Speaker b) {
        w_.write(1, c_.stereo_proc ? 1U : 0U, "b_enable_mdct_stereo_proc");
        sf_info();
        if (c_.stereo_proc) {
            chparam(c_.sap_mode);
        } else {
            sf_info();
        }
        pair_tracks(a, b);
    }

    void three_channel_data(Speaker a, Speaker b, Speaker c) {
        sf_info();
        w_.write(4, static_cast<std::uint64_t>(c_.chel_matsel), "chel_matsel");
        const std::array<Abcd, 2> p = {parameters_of(c_.sap_mode), parameters_of(c_.sap_mode)};
        chparam(c_.sap_mode);
        chparam(c_.sap_mode);
        mixed(printed_matrix(kTable178[static_cast<std::size_t>(c_.chel_matsel)], p), {a, b, c});
    }

    void four_channel_data(Speaker a, Speaker b, Speaker c, Speaker d) {
        sf_info();
        std::array<Abcd, 4> p{};
        for (Abcd& set : p) {
            set = parameters_of(c_.sap_mode);
            chparam(c_.sap_mode);
        }
        mixed(printed_matrix(kFourChannel, p), {a, b, c, d});
    }

    void five_channel_data(Speaker a, Speaker b, Speaker c, Speaker d, Speaker e) {
        sf_info();
        w_.write(4, static_cast<std::uint64_t>(c_.chel_matsel), "chel_matsel");
        std::array<Abcd, 5> p{};
        for (Abcd& set : p) {
            set = parameters_of(c_.sap_mode);
            chparam(c_.sap_mode);
        }
        mixed(printed_matrix(kTable179[static_cast<std::size_t>(c_.chel_matsel)], p), {a, b, c, d, e});
    }

    void chparam(int sap_mode) {
        ac4::detail::StereoChoice choice;
        choice.sap_mode = sap_mode;
        ac4::detail::write_chparam_info(w_, choice);
    }

    // The channel pair's ASPX_ACPL_1 data (Table 21): with stereo processing
    // one sf_info() with b_dual_maxsfb and its chparam_info(); without, the
    // mid track's sf_info() and the side track's, b_side_limited. One long
    // block, n_msfb_bits 6 and n_side_bits 5 (Table 106).
    void acpl_1_pair(Speaker a, Speaker b) {
        const auto max_sfb = static_cast<std::uint64_t>(max_sfb_);
        w_.write(1, c_.stereo_proc ? 1U : 0U, "b_enable_mdct_stereo_proc");
        if (c_.stereo_proc) {
            w_.write(1, 1, "b_long_frame");
            w_.write(6, max_sfb, "max_sfb");
            w_.write(6, max_sfb, "max_sfb_side");
            chparam(c_.sap_mode);
        } else {
            w_.write(1, 0, "spec_frontend_m");
            sf_info();
            w_.write(1, 0, "spec_frontend_s");
            w_.write(1, 1, "b_long_frame");
            w_.write(5, max_sfb, "max_sfb_side");
        }
        pair_tracks(a, b);
    }

    // ASPX_ACPL_1's residuals (Tables 25 and 33): max_sfb_master at the long
    // block's n_side_bits, two chparam_info() and two sf_data().
    void residuals(Speaker s0, Speaker s1, int sap_mode) {
        w_.write(5, static_cast<std::uint64_t>(max_sfb_), "max_sfb_master");
        chparam(sap_mode);
        chparam(sap_mode);
        sf_data(lines_.at(s0));
        sf_data(lines_.at(s1));
    }

   private:
    void sf_info() { ac4::detail::write_sf_info(w_, layout_, {max_sfb_, max_sfb_}); }

    void sf_data(const Lines& lines) { ac4::detail::write_sf_data(w_, code(lines, layout_, max_sfb_), layout_); }

    void pair_tracks(Speaker a, Speaker b) {
        if (c_.stereo_proc) {
            const std::array<Abcd, 1> p = {parameters_of(c_.sap_mode)};
            mixed(printed_matrix("a0 b0 | c0 d0", p), {a, b});
        } else {
            sf_data(lines_.at(a));
            sf_data(lines_.at(b));
        }
    }

    void mixed(const Matrix& m, const std::vector<Speaker>& outputs) {
        std::vector<const Lines*> o;
        for (const Speaker speaker : outputs) {
            o.push_back(&lines_.at(speaker));
        }
        for (const Lines& track : tracks_for(m, o)) {
            sf_data(track);
        }
    }

    BitWriter& w_;
    const std::map<Speaker, Lines>& lines_;
    const ElementCase& c_;
    FrameLayout layout_;
    int max_sfb_ = 0;
};

// One FIXFIX envelope over the frame: loud, with noise carrying its energy,
// or silent.
[[nodiscard]] AspxChannelFields aspx_channel(const AspxSetup& setup, bool loud) {
    AspxChannelFields c;
    c.framing.int_class = ac4::detail::AspxIntervalClass::kFixFix;
    c.framing.tmp_num_env = 0;
    if (setup.config.freq_res_mode == 0) {
        c.framing.freq_res = {1};
    }
    c.qmode_env = 0;  // one FIXFIX envelope is sent in 1.5 dB steps
    const std::vector<int> borders = ac4::detail::interval_borders(c.framing, 0);
    bool high = true;
    switch (setup.config.freq_res_mode) {
        case 0:
            high = c.framing.freq_res[0] != 0;
            break;
        case 1:
            high = false;
            break;
        case 2:
            high = ac4::detail::envelope_high_res(borders, 0, c.framing.tsg_ptr);
            break;
        default:
            break;
    }
    c.envelope_freq_res = {high ? 1 : 0};
    const int bands = high ? setup.counts.num_sbg_sig_highres : setup.counts.num_sbg_sig_lowres;
    // The first value along frequency, then no change: every group at it.
    const auto flat = [](int count, int first) {
        std::vector<int> values(static_cast<std::size_t>(count), 0);
        if (!values.empty()) {
            values.front() = first;
        }
        return ac4::detail::AspxEnvelopeFields{.delta_dir = 0, .values = values};
    };
    c.sig = {flat(bands, loud ? kLoudEnvelope : 0)};
    c.noise = {flat(setup.counts.num_sbg_noise, loud ? 0 : kNoNoise)};
    c.tna_mode.assign(static_cast<std::size_t>(setup.counts.num_sbg_noise), 0);
    return c;
}

void write_aspx_data(BitWriter& w, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    const auto elements = aspx_elements(c.ch_mode, c.acpl != 0 ? c.acpl : 1);
    for (std::size_t e = 0; e < elements.size(); ++e) {
        const bool loud = static_cast<int>(e) == c.loud_unit;
        if (elements[e].size() == 1) {
            ac4::detail::write_aspx_data_1ch(w, iframe, setup.xover_subband_offset, setup.config, setup.counts,
                                             aspx_channel(setup, loud));
        } else {
            ac4::detail::write_aspx_data_2ch(w, iframe, setup.xover_subband_offset, setup.config, setup.counts,
                                             false, {aspx_channel(setup, loud), aspx_channel(setup, loud)});
        }
    }
}

// companding_control(num_chan), b_compand_on for the case's channel only.
void write_companding(BitWriter& w, int num_chan, const ElementCase& c) {
    ac4::detail::CompandingFields fields;
    fields.num_chan = num_chan;
    for (int ch = 0; ch < num_chan; ++ch) {
        fields.compand_on[static_cast<std::size_t>(ch)] = ch == c.companded;
    }
    ac4::detail::write_companding_control(w, fields);
}

void write_3_0(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    w.write(1, c.aspx ? 1U : 0U, "3_0_codec_mode");
    if (iframe && c.aspx) {
        ac4::detail::write_aspx_config(w, setup.config);
    }
    if (c.aspx) {
        write_companding(w, 3, c);
    }
    w.write(1, static_cast<std::uint64_t>(c.coding_config), "3_0_coding_config");
    if (c.coding_config == 0) {
        e.stereo_data(S::kLeft, S::kRight);
        e.mono(S::kCentre);
    } else {
        e.three_channel_data(S::kLeft, S::kRight, S::kCentre);
    }
    if (c.aspx) {
        write_aspx_data(w, iframe, setup, c);
    }
}

// Table 180: where coding_config 0 to 3 put the channels.
void write_5_x(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    w.write(3, c.aspx ? 1U : 0U, "5_X_codec_mode");
    if (iframe && c.aspx) {
        ac4::detail::write_aspx_config(w, setup.config);
    }
    if (has_lfe(c.ch_mode)) {
        e.lfe();
    }
    if (c.aspx) {
        write_companding(w, 5, c);
    }
    w.write(2, static_cast<std::uint64_t>(c.coding_config), "coding_config");
    switch (c.coding_config) {
        case 0:
            w.write(1, c.two_ch_mode ? 1U : 0U, "2ch_mode");
            if (c.two_ch_mode) {
                e.two_channel_data(S::kLeft, S::kLeftSurround);
                e.two_channel_data(S::kRight, S::kRightSurround);
            } else {
                e.two_channel_data(S::kLeft, S::kRight);
                e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            }
            e.mono(S::kCentre);
            break;
        case 1:
            e.three_channel_data(S::kLeft, S::kRight, S::kCentre);
            e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            break;
        case 2:
            e.four_channel_data(S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround);
            e.mono(S::kCentre);
            break;
        default:
            e.five_channel_data(S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround);
            break;
    }
    if (c.aspx) {
        write_aspx_data(w, iframe, setup, c);
    }
}

// Table 182's A to G, with Table 183's steps undone first where
// b_use_sap_add_ch sends them: `lines` then holds A to G under the channels
// they are at identity (L, R, C, Ls, Rs and the last pair).
void write_7_x(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    const auto [f, g] = last_pair(c.ch_mode);
    w.write(2, c.aspx ? 1U : 0U, "7_X_codec_mode");
    if (iframe && c.aspx) {
        ac4::detail::write_aspx_config(w, setup.config);
    }
    if (has_lfe(c.ch_mode)) {
        e.lfe();
    }
    w.write(2, static_cast<std::uint64_t>(c.coding_config), "coding_config");
    switch (c.coding_config) {
        case 0:
            w.write(1, c.two_ch_mode ? 1U : 0U, "2ch_mode");
            if (c.two_ch_mode) {
                e.two_channel_data(S::kLeft, S::kLeftSurround);
                e.two_channel_data(S::kRight, S::kRightSurround);
            } else {
                e.two_channel_data(S::kLeft, S::kRight);
                e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            }
            break;
        case 1:
            e.three_channel_data(S::kLeft, S::kRight, S::kCentre);
            e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            break;
        case 2:
            e.four_channel_data(S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround);
            break;
        default:
            e.five_channel_data(S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround);
            break;
    }
    w.write(1, c.use_sap_add_ch ? 1U : 0U, "b_use_sap_add_ch");
    if (c.use_sap_add_ch) {
        e.chparam(c.sap_add_mode);
        e.chparam(c.sap_add_mode);
    }
    e.two_channel_data(f, g);
    if (c.coding_config == 0 || c.coding_config == 2) {
        e.mono(S::kCentre);
    }
    if (c.aspx) {
        write_aspx_data(w, iframe, setup, c);
    }
}

// alpha 1 or -1 (Tables 203 and 205), and alpha 0.
[[nodiscard]] int alpha_q(bool second, int quant) {
    if (quant == 0) {
        return second ? 8 : 24;
    }
    return second ? 4 : 12;
}

[[nodiscard]] int alpha_zero_q(int quant) {
    return quant == 0 ? 16 : 8;
}

// Every band of one parameter at one quantised value: in I-frames along
// frequency, the first band's value and no change after it; otherwise along
// time, no change from the frame before.
[[nodiscard]] AcplParamFields constant_param(int value, int first, int bands, bool iframe) {
    ac4::detail::AcplSetFields set;
    set.diff_type = iframe ? 0 : 1;
    set.values.assign(static_cast<std::size_t>(bands - first), 0);
    if (iframe) {
        set.values.front() = value;
    }
    return {set};
}

[[nodiscard]] AcplConfig1chFields acpl_config_1ch_of(const ElementCase& c) {
    return {.partial = c.acpl == 2,
            .num_param_bands_id = c.acpl_bands_id,
            .quant_mode = c.acpl_quant,
            .qmf_band = kAcplQmfBand};
}

[[nodiscard]] AcplConfig2chFields acpl_config_2ch_of(const ElementCase& c) {
    return {.num_param_bands_id = c.acpl_bands_id, .quant_mode_0 = c.acpl_quant, .quant_mode_1 = c.acpl_quant};
}

[[nodiscard]] AcplData1chFields acpl_data_1ch_of(const ElementCase& c, bool iframe) {
    const AcplConfig1chFields config = acpl_config_1ch_of(c);
    const int bands = ac4::detail::acpl_num_param_bands(config.num_param_bands_id);
    const int first = ac4::detail::acpl_param_band(config);
    const bool decorrelated = c.acpl_beta_q > 0;
    AcplData1chFields d;
    d.alpha1 = constant_param(decorrelated ? alpha_zero_q(c.acpl_quant) : alpha_q(c.acpl_second, c.acpl_quant),
                              first, bands, iframe);
    d.beta1 = constant_param(c.acpl_beta_q, first, bands, iframe);
    return d;
}

// ASPX_ACPL_3: gamma1 and gamma4 route L's and R's downmixes, alpha1 and
// alpha2 to L and R or to Ls and Rs; every beta 0.
[[nodiscard]] AcplData2chFields acpl_data_2ch_of(const ElementCase& c, bool iframe) {
    const int bands = ac4::detail::acpl_num_param_bands(c.acpl_bands_id);
    const int gamma_q = c.acpl_quant == 0 ? 4 : 2;
    AcplData2chFields d;
    for (auto& alpha : d.alpha) {
        alpha = constant_param(alpha_q(c.acpl_second, c.acpl_quant), 0, bands, iframe);
    }
    for (auto& beta : d.beta) {
        beta = constant_param(0, 0, bands, iframe);
    }
    d.beta3 = constant_param(0, 0, bands, iframe);
    for (std::size_t k = 0; k < d.gamma.size(); ++k) {
        d.gamma[k] = constant_param(k == 0 || k == 3 ? gamma_q : 0, 0, bands, iframe);
    }
    return d;
}

void write_acpl_1ch_pair(BitWriter& w, const ElementCase& c, bool iframe, int count) {
    const AcplConfig1chFields config = acpl_config_1ch_of(c);
    const AcplData1chFields data = acpl_data_1ch_of(c, iframe);
    for (int k = 0; k < count; ++k) {
        ac4::detail::write_acpl_data_1ch(w, config, data);
    }
}

// channel_pair_element() in ASPX_ACPL_1 and 2 (Table 21).
void write_pair_acpl(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    w.write(2, static_cast<std::uint64_t>(c.acpl), "stereo_codec_mode");
    if (iframe) {
        ac4::detail::write_aspx_config(w, setup.config);
        ac4::detail::write_acpl_config_1ch(w, acpl_config_1ch_of(c));
    }
    write_companding(w, 1, c);
    if (c.acpl == 2) {
        e.acpl_1_pair(S::kLeft, S::kRight);
    } else {
        e.mono(S::kLeft);  // spec_frontend, sf_info() and sf_data(), as mono_data(0) sends them
    }
    write_aspx_data(w, iframe, setup, c);
    write_acpl_1ch_pair(w, c, iframe, 1);
}

// 5_X_channel_element() in the A-CPL modes (Table 25): Table 181's channel
// data, ASPX_ACPL_1's residuals, or ASPX_ACPL_3's stereo_data().
void write_5_x_acpl(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    w.write(3, static_cast<std::uint64_t>(c.acpl), "5_X_codec_mode");
    if (iframe) {
        ac4::detail::write_aspx_config(w, setup.config);
        if (c.acpl == 4) {
            ac4::detail::write_acpl_config_2ch(w, acpl_config_2ch_of(c));
        } else {
            ac4::detail::write_acpl_config_1ch(w, acpl_config_1ch_of(c));
        }
    }
    if (has_lfe(c.ch_mode)) {
        e.lfe();
    }
    if (c.acpl == 4) {
        write_companding(w, 2, c);
        e.stereo_data(S::kLeft, S::kRight);
        write_aspx_data(w, iframe, setup, c);
        ac4::detail::write_acpl_data_2ch(w, acpl_config_2ch_of(c), acpl_data_2ch_of(c, iframe));
        return;
    }
    write_companding(w, 3, c);
    w.write(1, c.coding_config != 0 ? 1U : 0U, "coding_config");
    if (c.coding_config != 0) {
        e.three_channel_data(S::kLeft, S::kRight, S::kCentre);
    } else {
        e.two_channel_data(S::kLeft, S::kRight);
    }
    if (c.acpl == 2) {
        e.residuals(S::kLeftSurround, S::kRightSurround, c.sap_add_mode);
    }
    if (c.coding_config == 0) {
        e.mono(S::kCentre);
    }
    write_aspx_data(w, iframe, setup, c);
    write_acpl_1ch_pair(w, c, iframe, 2);
}

// 7_X_channel_element() in ASPX_ACPL_1 and 2 (Table 33): Table 184's channel
// data, and ASPX_ACPL_1's residuals for the last pair.
void write_7_x_acpl(BitWriter& w, ElementWriter& e, bool iframe, const AspxSetup& setup, const ElementCase& c) {
    using S = Speaker;
    const auto [f, g] = last_pair(c.ch_mode);
    w.write(2, static_cast<std::uint64_t>(c.acpl), "7_X_codec_mode");
    if (iframe) {
        ac4::detail::write_aspx_config(w, setup.config);
        ac4::detail::write_acpl_config_1ch(w, acpl_config_1ch_of(c));
    }
    if (has_lfe(c.ch_mode)) {
        e.lfe();
    }
    write_companding(w, 5, c);
    w.write(2, static_cast<std::uint64_t>(c.coding_config), "coding_config");
    switch (c.coding_config) {
        case 0:
            w.write(1, c.two_ch_mode ? 1U : 0U, "2ch_mode");
            if (c.two_ch_mode) {
                e.two_channel_data(S::kLeft, S::kLeftSurround);
                e.two_channel_data(S::kRight, S::kRightSurround);
            } else {
                e.two_channel_data(S::kLeft, S::kRight);
                e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            }
            break;
        case 1:
            e.three_channel_data(S::kLeft, S::kRight, S::kCentre);
            e.two_channel_data(S::kLeftSurround, S::kRightSurround);
            break;
        case 2:
            e.four_channel_data(S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround);
            break;
        default:
            e.five_channel_data(S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround);
            break;
    }
    if (c.acpl == 2) {
        e.residuals(f, g, c.sap_add_mode);
    }
    if (c.coding_config == 0 || c.coding_config == 2) {
        e.mono(S::kCentre);
    }
    write_aspx_data(w, iframe, setup, c);
    write_acpl_1ch_pair(w, c, iframe, 2);
}

[[nodiscard]] Lines mix(double a, const Lines& x, double b, const Lines& y) {
    Lines out(x.size());
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = a * x[k] + b * y[k];
    }
    return out;
}

// ASPX_ACPL_1's pair of a base channel and the residual coded against it:
// below acpl_qmf_band, A-CPL makes base = k (x0 + x3) and partner = sqrt 2
// (x0 - x3) of what the base and residual carry after the residual's
// chparam_info() step, (x0, x3) = P (A, res); so (A, res) = P^-1 (x0, x3).
void residual_pair(std::map<Speaker, Lines>& lines, const std::map<Speaker, Lines>& tone, Speaker base,
                   Speaker partner, double k, int sap_mode) {
    const Lines x0 = mix(0.5 / k, tone.at(base), 0.5 / kRoot2, tone.at(partner));
    const Lines x3 = mix(0.5 / k, tone.at(base), -0.5 / kRoot2, tone.at(partner));
    const std::array<Abcd, 1> p = {parameters_of(sap_mode)};
    const std::vector<Lines> tracks = tracks_for(printed_matrix("a0 b0 | c0 d0", p), {&x0, &x3});
    lines[base] = tracks[0];
    lines[partner] = tracks[1];
}

// The A-CPL modes' outputs worked back through Pseudocodes 115 to 120: on
// entry `lines` holds each channel's tone, on return what the channel data
// carries for each channel it codes at identity, and the residuals' lines
// under the channels they are coded for. Returns the channels left silent.
std::vector<Speaker> acpl_lines(std::map<Speaker, Lines>& lines, const ElementCase& c) {
    using S = Speaker;
    const std::map<Speaker, Lines> tone = lines;
    const Lines& none = tone.begin()->second;
    const bool second = c.acpl_second;
    const auto scaled = [&](double gain, Speaker speaker) { return mix(gain, tone.at(speaker), 0.0, none); };
    if (c.ch_mode == 1) {
        if (c.acpl == 2) {
            // Below acpl_qmf_band L = x0 + x1 and R = x0 - x1.
            lines[S::kLeft] = mix(0.5, tone.at(S::kLeft), 0.5, tone.at(S::kRight));
            lines[S::kRight] = mix(0.5, tone.at(S::kLeft), -0.5, tone.at(S::kRight));
            return {};
        }
        if (c.acpl_beta_q > 0) {
            lines[S::kLeft] = tone.at(S::kLeft);  // L + R = 2 x0
            return {};
        }
        lines[S::kLeft] = scaled(0.5, second ? S::kRight : S::kLeft);
        return {second ? S::kLeft : S::kRight};
    }
    if (c.ch_mode <= 4) {
        if (c.acpl == 4) {
            // z0 = (1 + sqrt 2) gamma x0; z1 = sqrt 2 times that with alpha -1.
            const double k = (1.0 + kRoot2) * kGamma * (second ? kRoot2 : 1.0);
            lines[S::kLeft] = scaled(1.0 / k, second ? S::kLeftSurround : S::kLeft);
            lines[S::kRight] = scaled(1.0 / k, second ? S::kRightSurround : S::kRight);
            if (second) {
                return {S::kLeft, S::kRight, S::kCentre};
            }
            return {S::kCentre, S::kLeftSurround, S::kRightSurround};
        }
        if (c.acpl == 2) {
            residual_pair(lines, tone, S::kLeft, S::kLeftSurround, 1.0, c.sap_add_mode);
            residual_pair(lines, tone, S::kRight, S::kRightSurround, 1.0, c.sap_add_mode);
            return {};
        }
        // z0 = 2 x0 with alpha 1, z1 = sqrt 2 (2 x0) with alpha -1.
        lines[S::kLeft] = second ? scaled(0.5 / kRoot2, S::kLeftSurround) : scaled(0.5, S::kLeft);
        lines[S::kRight] = second ? scaled(0.5 / kRoot2, S::kRightSurround) : scaled(0.5, S::kRight);
        if (second) {
            return {S::kLeft, S::kRight};
        }
        return {S::kLeftSurround, S::kRightSurround};
    }
    // Table 202: the base pair the modules couple with the last pair, and the
    // pair that passes; Pseudocode 120 scales z0 and z2 by sqrt 2 when the base
    // is the surrounds (3/4/0, or add_ch_base), z6 and z7 when they pass.
    const bool surround_base = c.ch_mode <= 6 || c.add_ch_base;
    const auto [f, g] = last_pair(c.ch_mode);
    const std::array<S, 2> base = surround_base ? std::array{S::kLeftSurround, S::kRightSurround}
                                                : std::array{S::kLeft, S::kRight};
    const std::array<S, 2> passing = surround_base ? std::array{S::kLeft, S::kRight}
                                                   : std::array{S::kLeftSurround, S::kRightSurround};
    const std::array<S, 2> last = {f, g};
    const double k = surround_base ? kRoot2 : 1.0;
    const double k_passing = surround_base ? 1.0 : kRoot2;
    for (std::size_t i = 0; i < 2; ++i) {
        lines[passing[i]] = scaled(1.0 / k_passing, passing[i]);
        if (c.acpl == 2) {
            residual_pair(lines, tone, base[i], last[i], k, c.sap_add_mode);
        } else {
            lines[base[i]] = second ? scaled(0.5 / kRoot2, last[i]) : scaled(0.5 / k, base[i]);
        }
    }
    if (c.acpl == 2) {
        return {};
    }
    if (second) {
        return {base[0], base[1]};
    }
    return {f, g};
}

// Each channel's lines for frame `frame`: its tone over the 2N samples the
// frame's one long block transforms (asf/analysis.hpp).
[[nodiscard]] std::map<Speaker, Lines> channel_lines(ac4::detail::Analysis& analysis, const FrameLayout& layout,
                                                     const std::vector<Speaker>& speakers, int frame) {
    std::map<Speaker, Lines> out;
    std::vector<double> samples(2 * kFrameLength);
    for (const Speaker speaker : speakers) {
        const double w = 2.0 * std::numbers::pi * tone_of(speaker) / kRate;
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const auto t = static_cast<double>(static_cast<std::size_t>(frame) * kFrameLength + n);
            samples[n] = kAmplitude * std::sin(w * t);
        }
        Lines& lines = out[speaker];
        analysis.transform(samples, layout, kFrameLength, kFrameLength, lines);
    }
    return out;
}

// Table 183 undone: (first, second) = P (first', second'), so (first',
// second') = P^-1 (first, second), for each of the two steps.
void undo_additional_steps(std::map<Speaker, Lines>& lines, const ElementCase& c) {
    using S = Speaker;
    const auto [f, g] = last_pair(c.ch_mode);
    const bool back = c.ch_mode <= 6;
    const std::array<std::pair<S, S>, 2> steps = {std::pair{back ? S::kLeftSurround : S::kLeft, f},
                                                  std::pair{back ? S::kRightSurround : S::kRight, g}};
    const std::array<Abcd, 1> p = {parameters_of(c.sap_add_mode)};
    const Matrix m = printed_matrix("a0 b0 | c0 d0", p);
    for (const auto& [first, second] : steps) {
        const std::vector<Lines> tracks = tracks_for(m, {&lines.at(first), &lines.at(second)});
        lines[first] = tracks[0];
        lines[second] = tracks[1];
    }
}

}  // namespace

std::vector<std::vector<Speaker>> aspx_elements(int ch_mode, int codec_mode) {
    using S = Speaker;
    if (codec_mode >= 2) {
        if (ch_mode == 1) {
            return {{S::kLeft}};
        }
        if (ch_mode <= 4) {
            if (codec_mode == 4) {
                return {{S::kLeft, S::kRight}};
            }
            return {{S::kLeft, S::kRight}, {S::kCentre}};
        }
        return {{S::kLeft, S::kRight}, {S::kLeftSurround, S::kRightSurround}, {S::kCentre}};
    }
    if (ch_mode == 2) {
        return {{S::kLeft, S::kRight}, {S::kCentre}};
    }
    if (ch_mode <= 4) {
        return {{S::kLeft, S::kRight}, {S::kLeftSurround, S::kRightSurround}, {S::kCentre}};
    }
    const auto [left, right] = last_pair(ch_mode);
    const std::vector<S> surround = {S::kLeftSurround, S::kRightSurround};
    const std::vector<S> last = {left, right};
    const bool wide = ch_mode == 7 || ch_mode == 8;
    return {{S::kLeft, S::kRight}, wide ? last : surround, {S::kCentre}, wide ? surround : last};
}

BuiltStream build_stream(const ElementCase& c, int frames) {
    BuiltStream out;
    out.speakers = speakers_for(c.ch_mode);
    for (const Speaker speaker : out.speakers) {
        out.tone_hz.push_back(tone_of(speaker));
    }
    const std::optional<AspxSetup> setup = ac4::detail::aspx_setup_for(kAspxKbpsPerChannel, kRate);
    if (!setup) {
        throw std::runtime_error("no A-SPX configuration at 48 kHz");
    }
    ac4::detail::Analysis analysis(kFrameLength, 1);
    const FrameLayout layout = ac4::detail::long_layout(kFrameLength);
    for (int frame = 0; frame < frames; ++frame) {
        const bool iframe = frame % 4 == 0;
        std::map<Speaker, Lines> lines = channel_lines(analysis, layout, out.speakers, frame);
        if (c.acpl != 0) {
            const std::vector<Speaker> silent = acpl_lines(lines, c);
            for (std::size_t s = 0; s < out.speakers.size(); ++s) {
                if (std::ranges::find(silent, out.speakers[s]) != silent.end()) {
                    out.tone_hz[s] = 0.0;
                }
            }
            if (c.acpl_beta_q > 0) {
                out.tone_hz[1] = out.tone_hz[0];  // R carries L's tone, decorrelated
            }
        } else if (c.ch_mode >= 5 && c.use_sap_add_ch) {
            undo_additional_steps(lines, c);
        }
        BitWriter audio = BitWriter::buffered();
        ElementWriter element(audio, lines, c);
        if (c.ch_mode == 1) {
            write_pair_acpl(audio, element, iframe, *setup, c);
        } else if (c.ch_mode == 2) {
            write_3_0(audio, element, iframe, *setup, c);
        } else if (c.ch_mode <= 4) {
            if (c.acpl != 0) {
                write_5_x_acpl(audio, element, iframe, *setup, c);
            } else {
                write_5_x(audio, element, iframe, *setup, c);
            }
        } else if (c.acpl != 0) {
            write_7_x_acpl(audio, element, iframe, *setup, c);
        } else {
            write_7_x(audio, element, iframe, *setup, c);
        }
        ac4::detail::FrameFields fields;
        fields.sequence_counter = frame;
        fields.iframe = iframe;
        fields.ch_mode = c.ch_mode;
        fields.add_ch_base = c.add_ch_base;
        std::vector<ac4::SyntaxRecord>& trace = out.traces.emplace_back();
        const auto keep = [&trace](const ac4::SyntaxRecord& record) { trace.push_back(record); };
        auto raw = ac4::detail::write_frame(fields, audio, 0, keep);
        if (!raw) {
            throw std::runtime_error("the frame writer refused a frame");
        }
        out.frames.push_back(std::move(*raw));
    }
    return out;
}

std::vector<std::byte> sync_framed(const BuiltStream& stream) {
    std::vector<std::byte> out;
    for (const auto& frame : stream.frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

std::vector<ElementCase> committed_cases() {
    // One of each element and channel mode, with every coding_config, both
    // 2ch_modes, stereo processing on and off, and b_use_sap_add_ch, among
    // them; the ASPX ones with their second aspx_data element loud.
    return {
        {.name = "3_0-simple-config0", .ch_mode = 2, .coding_config = 0, .sap_mode = 2},
        {.name = "3_0-aspx-config1-matsel5", .ch_mode = 2, .aspx = true, .coding_config = 1, .chel_matsel = 5,
         .sap_mode = 2, .loud_unit = 1, .companded = 2},
        {.name = "5_0-simple-config2", .ch_mode = 3, .coding_config = 2, .sap_mode = 2},
        {.name = "5_1-simple-config1-matsel9", .ch_mode = 4, .coding_config = 1, .chel_matsel = 9, .sap_mode = 2},
        {.name = "5_1-simple-config0-2ch1", .ch_mode = 4, .coding_config = 0, .two_ch_mode = true,
         .stereo_proc = false},
        {.name = "5_1-aspx-config3-matsel3", .ch_mode = 4, .aspx = true, .coding_config = 3, .chel_matsel = 3,
         .sap_mode = 2, .loud_unit = 1, .companded = 3},
        {.name = "7_0-340-aspx-config1-matsel6", .ch_mode = 5, .aspx = true, .coding_config = 1, .chel_matsel = 6,
         .sap_mode = 2, .loud_unit = 3},
        {.name = "7_1-340-simple-config0-2ch1-sap", .ch_mode = 6, .coding_config = 0, .two_ch_mode = true,
         .sap_mode = 2, .use_sap_add_ch = true},
        {.name = "7_0-520-aspx-config3-matsel11-sap", .ch_mode = 7, .aspx = true, .coding_config = 3,
         .chel_matsel = 11, .sap_mode = 2, .use_sap_add_ch = true, .loud_unit = 1},
        {.name = "7_1-520-simple-config1-matsel0", .ch_mode = 8, .coding_config = 1, .sap_mode = 2},
        {.name = "7_0-322-aspx-config0", .ch_mode = 9, .aspx = true, .coding_config = 0, .sap_mode = 2,
         .loud_unit = 3},
        {.name = "7_1-322-simple-config2-sap", .ch_mode = 10, .coding_config = 2, .sap_mode = 2,
         .use_sap_add_ch = true},
        // The A-CPL modes: each element's, both routings, residuals against
        // both of Table 202's bases, and the band counts and quantisations.
        {.name = "2_0-acpl1-stereoproc", .ch_mode = 1, .sap_mode = 2, .acpl = 2},
        {.name = "2_0-acpl2-second", .ch_mode = 1, .acpl = 3, .acpl_second = true, .acpl_bands_id = 3},
        {.name = "5_1-acpl1-config1-matsel7", .ch_mode = 4, .coding_config = 1, .chel_matsel = 7, .sap_mode = 2,
         .sap_add_mode = 0, .acpl = 2, .acpl_bands_id = 1},
        {.name = "5_0-acpl2-config0", .ch_mode = 3, .coding_config = 0, .sap_mode = 2, .acpl = 3},
        {.name = "5_1-acpl3-second-coarse", .ch_mode = 4, .sap_mode = 2, .acpl = 4, .acpl_second = true,
         .acpl_quant = 1},
        {.name = "7_1-340-acpl2-config2", .ch_mode = 6, .coding_config = 2, .sap_mode = 2, .acpl = 3,
         .acpl_bands_id = 2},
        {.name = "7_0-520-acpl1-config3-base1", .ch_mode = 7, .coding_config = 3, .chel_matsel = 4, .sap_mode = 2,
         .acpl = 2, .add_ch_base = true},
        {.name = "7_1-322-acpl2-config0-second", .ch_mode = 10, .coding_config = 0, .sap_mode = 2, .acpl = 3,
         .acpl_second = true, .acpl_quant = 1},
    };
}

}  // namespace ac4dec_test
