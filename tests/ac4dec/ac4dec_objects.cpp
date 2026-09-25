#include "ac4dec_objects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "ac4enc/encoder.hpp"
#include "ajoc/ajoc_syntax.hpp"
#include "asf/analysis.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/stereo.hpp"
#include "aspx/aspx_encoder.hpp"
#include "aspx/aspx_syntax.hpp"
#include "bit_writer.hpp"
#include "frame/toc_writer.hpp"
#include "oamd/oamd_syntax.hpp"

namespace ac4dec_test {
namespace {

using ac4::detail::AjocFields;
using ac4::detail::AjocObjectFields;
using ac4::detail::AjocSetFields;
using ac4::detail::BitWriter;
using ac4::detail::FrameLayout;
using ac4::detail::OamdObject;
using ac4::detail::OamdObjectKind;
using ac4::detail::OamdTimingFields;
using ac4::detail::ObjectInfoBlockFields;
using ac4::detail::RenderStatus;
using ac4::detail::TocObjectAssignment;
using ac4::detail::TocObjectSubstream;
using Lines = std::vector<double>;

constexpr int kFrameLength = 2048;
constexpr int kRate = 48000;
constexpr double kAmplitude = 0.1;  // -20 dBFS
// The coded band: every line below 6 kHz, 512 lines of 11.7 Hz.
constexpr std::size_t kTopLine = 512;
// The LFE's max_sfb: n_msfbl_bits is 3 at 2 048 samples (Part 1 Table 106).
constexpr int kLfeMaxSfb = 7;
// A-SPX at 40 kbps a channel starts at 10.5 kHz, above every tone.
constexpr double kAspxKbpsPerChannel = 40.0;
// A silent A-SPX envelope: no signal and no noise (the noise codebook's top).
constexpr int kNoNoise = 29;
// Part 2 Tables 29 to 32: the dequantisation steps, coarse and fine.
constexpr double kCoarseStep = 0.2001953125;
constexpr double kFineStep = 0.10009765625;

[[nodiscard]] std::size_t at(int index) {
    return static_cast<std::size_t>(index);
}

// Part 2 Table 28, the builder's transcription: the parameter band of QMF
// subband `sb` for each ajoc_num_bands, by the table's rows of subbands.
[[nodiscard]] int band_of(int num_bands, int sb) {
    struct Row {
        int last_subband;
        std::array<int, 8> bands;  // for 23, 15, 12, 9, 7, 5, 3 and 1 bands
    };
    static constexpr std::array<Row, 23> kRows = {{
        {0, {0, 0, 0, 0, 0, 0, 0, 0}},     {1, {1, 1, 1, 1, 1, 1, 0, 0}},
        {2, {2, 2, 2, 2, 2, 1, 0, 0}},     {3, {3, 3, 3, 3, 2, 2, 1, 0}},
        {4, {4, 4, 4, 3, 3, 2, 1, 0}},     {5, {5, 5, 4, 4, 3, 2, 1, 0}},
        {6, {6, 6, 5, 4, 3, 2, 1, 0}},     {7, {7, 7, 5, 5, 3, 2, 1, 0}},
        {8, {8, 8, 6, 5, 4, 2, 1, 0}},     {9, {9, 9, 6, 6, 4, 3, 1, 0}},
        {10, {10, 9, 6, 6, 4, 3, 1, 0}},   {11, {11, 10, 7, 6, 4, 3, 1, 0}},
        {13, {12, 10, 7, 6, 4, 3, 1, 0}},  {15, {13, 11, 8, 7, 5, 3, 2, 0}},
        {17, {14, 11, 8, 7, 5, 3, 2, 0}},  {19, {15, 12, 9, 7, 5, 3, 2, 0}},
        {22, {16, 12, 9, 7, 5, 3, 2, 0}},  {25, {17, 13, 10, 8, 6, 4, 2, 0}},
        {29, {18, 13, 10, 8, 6, 4, 2, 0}}, {34, {19, 13, 10, 8, 6, 4, 2, 0}},
        {40, {20, 14, 11, 8, 6, 4, 2, 0}}, {47, {21, 14, 11, 8, 6, 4, 2, 0}},
        {63, {22, 14, 11, 8, 6, 4, 2, 0}},
    }};
    constexpr std::array<int, 8> kCounts = {23, 15, 12, 9, 7, 5, 3, 1};
    const auto column =
        static_cast<std::size_t>(std::ranges::find(kCounts, num_bands) - kCounts.begin());
    for (const Row& row : kRows) {
        if (sb <= row.last_subband) {
            return row.bands[column];
        }
    }
    return 0;
}

// The QMF subband of downmix signal or object `k`'s tone.
[[nodiscard]] int tone_subband(int k) {
    return 2 * k + 1;
}

// Pseudocode 14a, the builder's transcription: the element output (Q'in, the
// LFE first where there is one) that A-JOC input `i` takes, for m_fb fullband
// signals.
[[nodiscard]] int qin_source(int i, int m_fb, bool lfe) {
    const int m = m_fb + (lfe ? 1 : 0);
    const int skip = lfe ? 1 : 0;
    if (m_fb > 3) {
        const int n_offset = 2 + m_fb % 2;
        if (i < n_offset) {
            return m - n_offset + i;
        }
        return i - n_offset + skip;
    }
    return i + skip;
}

// The fullband track (0 up, in syntax order) whose tone A-JOC input `i` takes.
[[nodiscard]] int qin_track(const ObjectCase& c, int i) {
    if (c.kind == ObjectCase::Kind::kAjocStatic) {
        return i;  // L, R, C, Ls and Rs, in that order (src/ac4dec/ERRATA.md)
    }
    return qin_source(i, c.dmx, c.lfe) - (c.lfe ? 1 : 0);
}

[[nodiscard]] int num_dmx(const ObjectCase& c) {
    return c.kind == ObjectCase::Kind::kAjocStatic ? 5 : c.dmx;
}

// The object's coefficient on A-JOC input `ch` in parameter band `pb`, in
// dequantisation steps: object o takes input o mod m at +5, and an object
// past the inputs' count input (o + 1) mod m at -3 too; with more than one
// band, a step less, the same or a step more by band.
[[nodiscard]] int dry_steps(const ObjectCase& c, int o, int ch, int pb) {
    const int m = num_dmx(c);
    if (c.absent && o == c.umx - 1) {
        return 0;
    }
    int base = 0;
    if (ch == o % m) {
        base = 5;
    } else if (o >= m && ch == (o + 1) % m) {
        base = -3;
    }
    if (base == 0 || ac4::detail::ajoc_band_count(c.bands_code) == 1) {
        return base;
    }
    return base + pb % 3 - 1;
}

// The wet coefficient: object umx - 1 - d takes decorrelator d at +3 steps.
[[nodiscard]] int wet_steps(const ObjectCase& c, int o, int d) {
    if (c.absent && o == c.umx - 1) {
        return 0;
    }
    return o == c.umx - 1 - d ? 3 : 0;
}

// The dialogue object's downmix coefficient on input `ch`, Table 82's value
// times 15: all of the first input and a third of the second.
[[nodiscard]] int dialogue_coeff_code(int ch) {
    return ch == 0 ? 15 : (ch == 1 ? 5 : 0);
}

[[nodiscard]] double step_of(const ObjectCase& c) {
    return c.quant == 1 ? kCoarseStep : kFineStep;
}

// Pseudocode 16's nquant for dry and wet data: the quantised range's size.
[[nodiscard]] int nquant(bool wet, int quant) {
    if (wet) {
        return quant == 1 ? 21 : 41;
    }
    return quant == 1 ? 51 : 101;
}

// ajoc_huff_data()'s values for quantised values `q` (band by band): DIFF_FREQ
// sends the first as it is and each next as its difference from the band below
// modulo nquant; DIFF_TIME each band's difference from `previous`.
[[nodiscard]] AjocSetFields huff_values(const std::vector<int>& q, const std::vector<int>& previous,
                                        bool diff_time, int n) {
    AjocSetFields set;
    set.diff_type = diff_time ? 1 : 0;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (diff_time) {
            set.values.push_back(q[i] - previous[i]);
        } else if (i == 0) {
            set.values.push_back(q[0]);
        } else {
            set.values.push_back(((q[i] - q[i - 1]) % n + n) % n);
        }
    }
    return set;
}

// The quantised values of an object's coefficient on an input or decorrelator,
// band by band: the steps plus the range's centre.
[[nodiscard]] std::vector<int> quantised(const ObjectCase& c, int o, int index, bool wet) {
    const int bands = ac4::detail::ajoc_band_count(c.bands_code);
    const int centre = (nquant(wet, c.quant) - 1) / 2;
    std::vector<int> q;
    for (int pb = 0; pb < bands; ++pb) {
        q.push_back(centre + (wet ? wet_steps(c, o, index) : dry_steps(c, o, index, pb)));
    }
    return q;
}

// ajoc() for a frame: the case's constant parameters at every data point,
// DIFF_FREQ in an I-frame (ajoc_b_nodt) and DIFF_TIME in the others where the
// case asks, each data point after the first against the one before it.
[[nodiscard]] AjocFields ajoc_fields(const ObjectCase& c, bool iframe) {
    AjocFields f;
    f.decorr_enable.assign(at(c.decorr), 1);
    // An I-frame sends a data point at least, so that every stream starts
    // from its parameters.
    f.num_dpoints = iframe ? std::max(c.dpoints, 1) : c.dpoints;
    for (int dp = 0; dp < f.num_dpoints; ++dp) {
        // Data points at slots 0 and 16, each ramping over c.ramp slots.
        f.start_pos[at(dp)] = dp * 16;
        f.ramp_len[at(dp)] = c.ramp;
    }
    f.b_nodt = iframe || !c.diff_time;
    const int m = num_dmx(c);
    for (int o = 0; o < c.umx; ++o) {
        AjocObjectFields object;
        object.present = !(c.absent && o == c.umx - 1);
        object.num_bands_code = c.bands_code;
        object.quant_select = c.quant;
        object.sparse = c.sparse;
        if (c.sparse) {
            for (int ch = 0; ch < m; ++ch) {
                object.dry_present.push_back(dry_steps(c, o, ch, 0) != 0 ? 1 : 0);
            }
            for (int d = 0; d < c.decorr; ++d) {
                object.wet_present.push_back(wet_steps(c, o, d) != 0 ? 1 : 0);
            }
        }
        for (int dp = 0; dp < f.num_dpoints; ++dp) {
            // The values are the same at every data point and in every frame,
            // so DIFF_TIME sends zeros; the first data point of an I-frame is
            // frequency-differential, and so is every one without diff_time.
            const bool diff_time = c.diff_time && !(iframe && dp == 0);
            auto& dry = object.dry.emplace_back();
            for (int ch = 0; ch < m; ++ch) {
                const std::vector<int> q = quantised(c, o, ch, false);
                dry.push_back(huff_values(q, q, diff_time, nquant(false, c.quant)));
            }
            auto& wet = object.wet.emplace_back();
            for (int d = 0; d < c.decorr; ++d) {
                const std::vector<int> q = quantised(c, o, d, true);
                wet.push_back(huff_values(q, q, diff_time, nquant(true, c.quant)));
            }
        }
        f.objects.push_back(std::move(object));
    }
    return f;
}

// --- Object audio metadata ---------------------------------------------------

// Where the case's objects are, pos3D_X, _Y, _Z_sign and _Z: object 0 moves
// from the left wall to the right one over the stream, block by block; the
// others stay where they start.
[[nodiscard]] std::array<int, 4> position_of(int object, int frame, int block, int frames,
                                             int blocks) {
    if (object == 0) {
        const int t = frame * blocks + block;
        const int total = std::max(1, frames * blocks - 1);
        return {static_cast<int>(std::lround(62.0 * t / total)), 0, 1, 0};
    }
    return {(object * 17) % 63, (object * 23) % 63, object % 2, object % 16};
}

// The timing a frame sends: sample_offset by each of Table 92's types in turn,
// and the case's blocks, the first at the frame's start and the second half
// way through with a ramp from Table 95 or of its own.
[[nodiscard]] OamdTimingFields timing_of(const ObjectCase& c, int frame) {
    OamdTimingFields t;
    switch (frame % 3) {
        case 0:
            t.sample_offset_type = 0b0;
            break;
        case 1:
            t.sample_offset_type = 0b10;
            t.sample_offset_code = 0b10;
            break;
        default:
            t.sample_offset_type = 0b11;
            t.sample_offset = 5;
            break;
    }
    for (int b = 0; b < c.blocks; ++b) {
        OamdTimingFields::Block block;
        block.offset_factor = b * 32;
        if (b == 0) {
            block.ramp_code = 0b01;
        } else {
            block.ramp_code = 0b11;
            block.use_table = frame % 2 == 0;
            block.ramp_table = 8;
            block.ramp = 700;
        }
        t.blocks.push_back(block);
    }
    return t;
}

// The extras an object's I-frame block carries where the case asks for them:
// gain and priority, a zone constraint with snap, width, screen factor,
// distance and divergence, and add_per_object_md() with extended precision
// and headphone data.
void add_extras(ObjectInfoBlockFields& block, int object, bool dynamic) {
    block.basic.defaults = false;
    block.basic.info_md = 0b10;
    block.basic.gain_code = 0b0;
    block.basic.gain_value = 10 + object;
    block.basic.priority = 20;
    if (dynamic) {
        auto& r = block.render_fields;
        r.zone_defaults = false;
        r.zone_flags = 0b101;
        r.zone_mask = object % 7;
        r.other_defaults = false;
        r.other_mask = 0b1111;
        r.width_mode = object % 2;
        r.width = 12;
        r.width_xyz = {3, 6, 9};
        r.screen_factor = 3;
        r.depth_factor = 2;
        r.at_infinity = object % 3 == 0;
        r.distance = 5;
        r.div_mode = object % 2 == 0 ? 0b10 : 0b00;
        r.div_table = 1;
        r.div_code = 26;
    }
    ac4::detail::AddPerObjectFields add;
    add.trim_disable = true;
    if (dynamic) {
        add.ext_prec_pos = ac4::detail::ExtPrecPosFields{.presence = 0b110, .xyz = {1, 3, 0}};
    }
    add.headphone = std::pair{2, true};
    block.add_table = add;
}

// Each object's blocks of a frame. An I-frame's first block sends everything
// (b_no_delta); the other blocks reuse the basic information, and for a
// dynamic object reuse its render information where it has not moved and send
// the position alone where it has, as a difference where one fits.
[[nodiscard]] std::vector<ObjectInfoBlockFields> blocks_of(const ObjectCase& c,
                                                           std::span<const OamdObject> objects,
                                                           int frame, int frames, bool iframe) {
    std::vector<ObjectInfoBlockFields> out;
    int dynamic_index = 0;
    for (const OamdObject& object : objects) {
        const bool dynamic = object.kind == OamdObjectKind::kDynamic && !object.lfe;
        const int index = dynamic ? dynamic_index++ : -1;
        for (int b = 0; b < c.blocks; ++b) {
            ObjectInfoBlockFields block;
            const bool no_delta = iframe && b == 0;
            std::array<int, 4> pos{};
            std::array<int, 4> before{};
            if (dynamic) {
                pos = position_of(index, frame, b, frames, c.blocks);
                before = b > 0 ? position_of(index, frame, b - 1, frames, c.blocks)
                               : position_of(index, std::max(frame - 1, 0), c.blocks - 1, frames,
                                             c.blocks);
                block.render_fields.x = pos[0];
                block.render_fields.y = pos[1];
                block.render_fields.z_sign = pos[2];
                block.render_fields.z = pos[3];
            }
            if (no_delta) {
                if (c.extras && index != 0) {
                    add_extras(block, std::max(index, 0), dynamic);
                }
            } else {
                block.basic_reuse = true;
                if (dynamic && pos != before) {
                    block.render = RenderStatus::kPartReuse;
                    block.render_fields.otherprops = false;
                    block.render_fields.zone = false;
                    block.render_fields.position = true;
                    const int dx = pos[0] - before[0];
                    // Standard precision, the same sign of Z (6.3.9.8.4).
                    if (dx >= -4 && dx <= 3 && pos[1] == before[1] && pos[2] == before[2] &&
                        pos[3] == before[3]) {
                        block.render_fields.diff_pos = true;
                        block.render_fields.diff = {dx, 0, 0};
                    }
                } else {
                    block.render = RenderStatus::kReuse;
                }
            }
            out.push_back(block);
        }
    }
    return out;
}

// --- Audio -------------------------------------------------------------------

// A track's lines: `hz` at kAmplitude, continuous from frame to frame.
[[nodiscard]] Lines tone_lines(ac4::detail::Analysis& analysis, const FrameLayout& layout,
                               double hz, int frame) {
    std::vector<double> samples(2 * kFrameLength);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const auto t = static_cast<double>(static_cast<std::size_t>(frame) * kFrameLength + n);
        samples[n] = kAmplitude * std::sin(w * t);
    }
    Lines lines;
    analysis.transform(samples, layout, kFrameLength, kFrameLength, lines);
    return lines;
}

// One track coded finely: each band's step a 2^-12 of its peak.
[[nodiscard]] ac4::detail::CodedTrack code(const Lines& lines, const FrameLayout& layout,
                                           int max_sfb) {
    const ac4::detail::Grouped grouped = ac4::detail::regroup(lines, layout, {max_sfb, max_sfb});
    std::vector<std::vector<int>> sf(grouped.offset.size());
    for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
        for (std::size_t b = 0; b + 1 < grouped.offset[g].size(); ++b) {
            double peak = 0.0;
            for (std::size_t k = grouped.offset[g][b]; k < grouped.offset[g][b + 1]; ++k) {
                peak = std::max(peak, std::abs(grouped.lines[k]));
            }
            const double step = peak > 0.0 ? peak / 4096.0 : 1.0;
            sf[g].push_back(static_cast<int>(std::lround(100.0 + 4.0 * std::log2(step))));
        }
    }
    return ac4::detail::code_track(grouped, sf, 0, layout);
}

// The Part 1 data elements, their tracks each a tone, without stereo
// processing (b_enable_mdct_stereo_proc 0) or with an identity matrix
// (three_channel_data() of chel_matsel 0 and sap_mode 0).
class TrackWriter {
   public:
    TrackWriter(BitWriter& w, ac4::detail::Analysis& analysis, int frame)
        : w_(w),
          analysis_(analysis),
          frame_(frame),
          layout_(ac4::detail::long_layout(kFrameLength)) {
        const auto offsets = ac4::detail::band_offsets(kFrameLength);
        while (max_sfb_ + 1 < static_cast<int>(offsets.size()) &&
               offsets[at(max_sfb_)] < kTopLine) {
            ++max_sfb_;
        }
    }

    // mono_data(1): sf_info_lfe() is max_sfb alone.
    void lfe() {
        w_.write(3, kLfeMaxSfb, "max_sfb");
        data(kLfeToneHz, kLfeMaxSfb);
    }

    // mono_data(0).
    void mono(double hz) {
        w_.write(1, 0, "spec_frontend");
        info();
        data(hz, max_sfb_);
    }

    // stereo_data() of the channel pair and 3.0 elements.
    void stereo_data(double a, double b) {
        w_.write(1, 0, "b_enable_mdct_stereo_proc");
        w_.write(1, 0, "spec_frontend_l");
        info();
        w_.write(1, 0, "spec_frontend_r");
        info();
        data(a, max_sfb_);
        data(b, max_sfb_);
    }

    void two_channel_data(double a, double b) {
        w_.write(1, 0, "b_enable_mdct_stereo_proc");
        info();
        info();
        data(a, max_sfb_);
        data(b, max_sfb_);
    }

    // chel_matsel 0 with two chparam_info() of sap_mode 0: Table 178's
    // matrix is then the identity, so the tracks are the outputs.
    void three_channel_data(double a, double b, double c) {
        info();
        w_.write(4, 0, "chel_matsel");
        for (int k = 0; k < 2; ++k) {
            ac4::detail::StereoChoice choice;
            choice.sap_mode = 0;
            ac4::detail::write_chparam_info(w_, choice);
        }
        data(a, max_sfb_);
        data(b, max_sfb_);
        data(c, max_sfb_);
    }

   private:
    void info() { ac4::detail::write_sf_info(w_, layout_, {max_sfb_, max_sfb_}); }

    void data(double hz, int max_sfb) {
        ac4::detail::write_sf_data(
            w_, code(tone_lines(analysis_, layout_, hz, frame_), layout_, max_sfb), layout_);
    }

    BitWriter& w_;
    ac4::detail::Analysis& analysis_;
    int frame_ = 0;
    FrameLayout layout_;
    int max_sfb_ = 0;
};

// A silent A-SPX envelope: one FIXFIX envelope with no signal and no noise.
[[nodiscard]] ac4::detail::AspxChannelFields silent_aspx(const ac4::detail::AspxSetup& setup) {
    ac4::detail::AspxChannelFields ch;
    ch.framing.int_class = ac4::detail::AspxIntervalClass::kFixFix;
    ch.framing.tmp_num_env = 0;
    if (setup.config.freq_res_mode == 0) {
        ch.framing.freq_res = {1};
    }
    ch.qmode_env = 0;
    const std::vector<int> borders = ac4::detail::interval_borders(ch.framing, 0);
    bool high = true;
    switch (setup.config.freq_res_mode) {
        case 0:
            high = ch.framing.freq_res[0] != 0;
            break;
        case 1:
            high = false;
            break;
        case 2:
            high = ac4::detail::envelope_high_res(borders, 0, ch.framing.tsg_ptr);
            break;
        default:
            break;
    }
    ch.envelope_freq_res = {high ? 1 : 0};
    const int bands = high ? setup.counts.num_sbg_sig_highres : setup.counts.num_sbg_sig_lowres;
    const auto flat = [](int count, int first) {
        std::vector<int> values(static_cast<std::size_t>(count), 0);
        if (!values.empty()) {
            values.front() = first;
        }
        return ac4::detail::AspxEnvelopeFields{.delta_dir = 0, .values = values};
    };
    ch.sig = {flat(bands, 0)};
    ch.noise = {flat(setup.counts.num_sbg_noise, kNoNoise)};
    ch.tna_mode.assign(static_cast<std::size_t>(setup.counts.num_sbg_noise), 0);
    return ch;
}

// var_channel_element(b_iframe, n_dmx_signals, b_has_lfe), Part 2 clause
// 6.2.4.4: fullband track k (in syntax order) carries object_tone_hz(k).
void write_var_element(BitWriter& w, TrackWriter& t, const ObjectCase& c, bool iframe,
                       const ac4::detail::AspxSetup& setup) {
    const int n = c.dmx;
    w.write(1, c.aspx ? 1U : 0U, "var_codec_mode");
    if (c.aspx) {
        if (iframe) {
            ac4::detail::write_aspx_config(w, setup.config);
        }
        if (n <= 5) {
            ac4::detail::CompandingFields companding;
            companding.num_chan = n;
            ac4::detail::write_companding_control(w, companding);
        }
    }
    if (c.lfe) {
        t.lfe();
    }
    const int pairs = n / 2;
    if (n % 2 != 0) {
        if (n == 1) {
            t.mono(object_tone_hz(0));
        } else {
            for (int p = 0; p < pairs - 1; ++p) {
                t.two_channel_data(object_tone_hz(2 * p), object_tone_hz(2 * p + 1));
            }
            w.write(1, static_cast<std::uint64_t>(c.var_coding_config), "var_coding_config");
            const int k = 2 * (pairs - 1);
            if (c.var_coding_config == 0) {
                t.two_channel_data(object_tone_hz(k), object_tone_hz(k + 1));
                t.mono(object_tone_hz(k + 2));
            } else {
                t.three_channel_data(object_tone_hz(k), object_tone_hz(k + 1),
                                     object_tone_hz(k + 2));
            }
        }
    } else {
        for (int p = 0; p < pairs; ++p) {
            t.two_channel_data(object_tone_hz(2 * p), object_tone_hz(2 * p + 1));
        }
    }
    if (c.aspx) {
        for (int p = 0; p < pairs; ++p) {
            ac4::detail::write_aspx_data_2ch(w, iframe, setup.xover_subband_offset, setup.config,
                                             setup.counts, false,
                                             {silent_aspx(setup), silent_aspx(setup)});
        }
        if (n % 2 != 0) {
            ac4::detail::write_aspx_data_1ch(w, iframe, setup.xover_subband_offset, setup.config,
                                             setup.counts, silent_aspx(setup));
        }
    }
}

// The Part 1 element objs_to_channel_mode() names for `n` objects (Part 2
// clause 6.2.3.3), in SIMPLE: mono, a pair, 3.0 or 5.0, whose channels L, R,
// C, Ls and Rs carry tones first_tone and up in that order (mono's C the
// first). With `lfe_in_element`, the 5.1 element of audio_data_chan(), its
// LFE's mono_data(1) inside it.
void write_element(BitWriter& w, TrackWriter& t, int n, int first_tone,
                   bool lfe_in_element = false) {
    const auto tone = [first_tone](int k) { return object_tone_hz(first_tone + k); };
    switch (n) {
        case 1:
            w.write(1, 0, "mono_codec_mode");
            t.mono(tone(0));
            break;
        case 2:
            w.write(2, 0, "stereo_codec_mode");
            t.stereo_data(tone(0), tone(1));
            break;
        case 3:
            w.write(1, 0, "3_0_codec_mode");
            w.write(1, 0, "3_0_coding_config");
            t.stereo_data(tone(0), tone(1));
            t.mono(tone(2));
            break;
        default:
            w.write(3, 0, "5_X_codec_mode");
            if (lfe_in_element) {
                t.lfe();
            }
            w.write(2, 0, "coding_config");
            w.write(1, 0, "2ch_mode");
            t.two_channel_data(tone(0), tone(1));  // L and R
            t.two_channel_data(tone(3), tone(4));  // Ls and Rs
            t.mono(tone(2));                       // C
            break;
    }
}

// metadata() of an object substream at sus_ver 1 (Part 2 clause 6.2.7): its
// channel_mode is negative, so basic_metadata() and extended_metadata() send
// their flags, with the dialogue fields where `dialog`, and tools_metadata()
// dialog_enhancement() alone.
void write_metadata(BitWriter& w, bool dialog) {
    w.write(1, 0, "b_more_basic_metadata");
    w.write(1, dialog ? 1U : 0U, "b_dialog");
    if (dialog) {
        w.write(1, 1, "b_dialog_max_gain");
        w.write(2, 2, "dialog_max_gain");
        w.write(1, 0, "b_pan_dialog_present");
    }
    w.write(1, 0, "b_channels_classifier");
    w.write(1, 0, "b_event_probability");
    w.write(7, 1, "tools_metadata_size_value");
    w.write(1, 0, "b_more_bits");
    w.write(1, 0, "b_de_data_present");
    w.write(1, 0, "b_emdf_payloads_substream");
}

// ac4_substream(): audio_size, `audio` byte-aligned, then metadata().
[[nodiscard]] std::vector<std::byte> audio_substream(const BitWriter& audio, int index, bool dialog,
                                                     std::vector<ac4::SyntaxRecord>& trace) {
    const auto keep = [&trace](const ac4::SyntaxRecord& record) { trace.push_back(record); };
    BitWriter w(index, keep);
    const std::size_t bytes = (audio.bit_position() + 7) / 8;
    w.write(15, bytes & 0x7FFFU, "audio_size_value");
    w.write(1, bytes >= 0x8000U ? 1U : 0U, "b_more_bits");
    if (bytes >= 0x8000U) {
        w.write_variable_bits(7, bytes >> 15U, "audio_size_value");
    }
    w.append(audio);
    w.write_unrecorded(static_cast<unsigned>(bytes * 8 - audio.bit_position()), 0);  // fill_bits
    write_metadata(w, dialog);
    w.align();
    return w.bytes();
}

// ac4_presentation_substream() of an object presentation (Part 2 clause
// 6.2.2.3): no additional data, dialnorm, no DRC, and loud_corr()'s
// b_obj_loud_corr, pres_ch_mode being -1. With `static_core`, an A-JOC
// substream over a static 5.X downmix, pres_ch_mode_core is 3 or 4 (Table 71):
// custom_dmx_data() then sends b_stereo_dmx_coeff and loud_corr() the core's
// two b_loud_comp.
[[nodiscard]] std::vector<std::byte> presentation_substream(int index, bool static_core,
                                                            std::vector<ac4::SyntaxRecord>& trace) {
    const auto keep = [&trace](const ac4::SyntaxRecord& record) { trace.push_back(record); };
    BitWriter w(index, keep);
    w.write(1, 0, "b_additional_data");
    w.write(7, 124, "dialnorm_bits");
    w.write(1, 0, "b_further_loudness_info");
    w.write(5, 1, "drc_metadata_size_value");
    w.write(1, 0, "b_more_bits");
    w.write(1, 0, "b_drc_present");
    w.write(1, 0, "b_associated");
    if (static_core) {
        w.write(1, 0, "b_stereo_dmx_coeff");
    }
    w.write(1, 0, "b_obj_loud_corr");
    if (static_core) {
        w.write(1, 0, "b_loud_comp");  // loud_corr_core_5_X
        w.write(1, 0, "b_loud_comp");  // loud_corr_core_loro and _ltrt
    }
    w.align();
    return w.bytes();
}

// --- Cases -------------------------------------------------------------------

// The objects of an A-JOC substream's portion: the LFE first where there is
// one, then dynamic objects (b_dyn_objects_only).
[[nodiscard]] std::vector<OamdObject> portion(int n_fullband, bool lfe) {
    std::vector<OamdObject> out;
    if (lfe) {
        out.push_back({OamdObjectKind::kBed, true, false});
    }
    for (int k = 0; k < n_fullband; ++k) {
        out.push_back({OamdObjectKind::kDynamic, false, false});
    }
    return out;
}

// The common data the case's table of contents or OAMD substream sends:
// trim, bed render and headphone data with the extras, else the defaults.
[[nodiscard]] ac4::detail::OamdCommonFields common_of(const ObjectCase& c) {
    ac4::detail::OamdCommonFields f;
    f.screen_size_ratio_code = 20;
    f.bed_object_chan_distribute = true;
    if (c.extras) {
        ac4::detail::TrimFields trim;
        trim.warp_mode = 1;
        trim.global_trim_mode = 0b10;
        trim.configs[0] = {.default_trim = false,
                           .disable = false,
                           .presence = 0b11111,
                           .centre = 3,
                           .surround = 5,
                           .height = 7,
                           .tb_sign = 1,
                           .tb_amount = 4,
                           .lis_sign = 0,
                           .lis_amount = 2};
        trim.configs[1] = {.default_trim = false, .disable = true};
        f.trim = trim;
        ac4::detail::BedRenderFields bed;
        bed.stereo_dmx = ac4::detail::BedRenderFields::StereoDmx{.loro_centre = 2,
                                                                 .loro_surround = 3,
                                                                 .ltrt = std::array<int, 2>{4, 5},
                                                                 .lfe = 6,
                                                                 .preferred_dmx_method = 1};
        bed.cdmx = true;
        bed.gain_w_to_f = 2;
        bed.tm_ch_present = true;
        bed.t2_to_f_s_b =
            ac4::detail::GainToolFields{.to_front = false, .to_side = true, .gain = 3};
        bed.tf_ch_present = true;
        bed.tf_to_f_s = ac4::detail::GainToolFields{.to_front = true, .to_side = false, .gain = 1};
        bed.gain_tfb_to_tm = 4;
        f.bed_render = bed;
        f.headphone =
            ac4::detail::HeadphoneFields{.operation_mode = 0b010, .head_track_disable_all = true};
    }
    return f;
}

// A timing's update samples, block by block.
[[nodiscard]] std::vector<int> update_samples_of(const OamdTimingFields& t) {
    const std::array<int, 3> kCodeOffset = {16, 8, 24};  // Table 93 by 0b0, 0b10 and 0b11
    int offset = 0;
    if (t.sample_offset_type == 0b10) {
        offset =
            kCodeOffset[t.sample_offset_code == 0b0 ? 0 : (t.sample_offset_code == 0b10 ? 1 : 2)];
    } else if (t.sample_offset_type == 0b11) {
        offset = t.sample_offset;
    }
    std::vector<int> out;
    for (const OamdTimingFields::Block& b : t.blocks) {
        out.push_back(offset + 32 * b.offset_factor);
    }
    return out;
}

struct Frame {
    std::vector<std::vector<std::byte>> substreams;
    std::vector<std::vector<ac4::SyntaxRecord>> traces;  // per substream
    ac4::detail::TocGroup group;
    int oamd_index = -1;
    int presentation_index = 0;
    std::optional<OamdTimingFields>
        full_timing;  // the timing full decoding's metadata takes, where sent
};

void build_ajoc_frame(const ObjectCase& c, int frame, int frames, bool iframe,
                      ac4::detail::Analysis& analysis, const ac4::detail::AspxSetup& setup,
                      Frame& out) {
    const bool is_static = c.kind == ObjectCase::Kind::kAjocStatic;
    const int m = num_dmx(c);
    const std::vector<OamdObject> dmx_objects = portion(m, c.lfe);
    const std::vector<OamdObject> umx_objects = portion(c.umx, c.lfe);
    BitWriter audio = BitWriter::buffered();
    TrackWriter tracks(audio, analysis, frame);
    std::optional<OamdTimingFields> dmx_timing;
    if (is_static) {
        // audio_data_chan(5.1 or 5.0): the 5.X element, L R C Ls Rs.
        write_element(audio, tracks, 5, 0, c.lfe);
    } else {
        audio.write(1, 0, "b_some_signals_inactive");
        write_var_element(audio, tracks, c, iframe, setup);
        // The downmix's own timing, unless the group's OAMD substream sends it.
        const bool own = !c.oamd_substream;
        audio.write(1, own ? 1U : 0U, "b_dmx_timing");
        if (own) {
            dmx_timing = timing_of(c, frame);
            ac4::detail::write_oamd_timing_data(audio, *dmx_timing);
        }
        const std::vector<ObjectInfoBlockFields> blocks =
            blocks_of(c, dmx_objects, frame, frames, iframe);
        ac4::detail::write_oamd_dyndata(audio, dmx_objects, c.blocks, iframe, blocks, false,
                                        nullptr);
        audio.write(1, c.bed_info ? 1U : 0U, "b_oamd_extension_present");
        if (c.bed_info) {
            // skip_bits: one byte, ajoc_bed_info() and the rest skip_data.
            audio.write_variable_bits(3, 0, "skip_bits");
            BitWriter info = BitWriter::buffered();
            ac4::detail::write_ajoc_bed_info(info, 3);
            audio.append(info);
            audio.write_zero_run(8 - info.bit_position(), "skip_data");
        }
    }
    ac4::detail::write_ajoc(audio, m, ajoc_fields(c, iframe));
    // ajoc_dmx_de_data(): the configuration and coefficients in I-frames, and
    // the coefficients kept in the others.
    ac4::detail::AjocDmxDeFields de;
    de.cfg = iframe;
    de.keep_coeffs = !iframe;
    de.max_gain = 2;
    de.dialogue.assign(at(c.umx), 0);
    if (c.dialogue) {
        de.dialogue[0] = 1;
        for (int ch = 0; ch < m; ++ch) {
            de.coeff.push_back(static_cast<std::uint8_t>(dialogue_coeff_code(ch)));
        }
    }
    ac4::detail::write_ajoc_dmx_de_data(audio, m, de, c.dialogue ? 1 : 0);
    // The upmix's timing: its own in even frames, the downmix's in odd ones,
    // or the group's where an OAMD substream sends it.
    if (c.oamd_substream || is_static) {
        const bool own = is_static && !c.oamd_substream;
        audio.write(1, own ? 1U : 0U, "b_umx_timing");
        if (own) {
            out.full_timing = timing_of(c, frame);
            ac4::detail::write_oamd_timing_data(audio, *out.full_timing);
        } else {
            audio.write(1, 0, "b_derive_timing_from_dmx");
        }
    } else if (frame % 2 == 0) {
        audio.write(1, 1, "b_umx_timing");
        out.full_timing = timing_of(c, frame + 1);
        ac4::detail::write_oamd_timing_data(audio, *out.full_timing);
    } else {
        audio.write(1, 0, "b_umx_timing");
        audio.write(1, 1, "b_derive_timing_from_dmx");
        out.full_timing = dmx_timing;
    }
    const std::vector<ObjectInfoBlockFields> umx_blocks =
        blocks_of(c, umx_objects, frame, frames, iframe);
    ac4::detail::write_oamd_dyndata(audio, umx_objects, c.blocks, iframe, umx_blocks, false,
                                    nullptr);

    // Substream 0 the A-JOC substream, then the OAMD substream, then the
    // presentation substream.
    int index = 0;
    TocObjectSubstream info;
    info.ajoc = true;
    info.lfe = c.lfe;
    info.static_dmx = is_static;
    info.dmx_signals = c.dmx;
    info.umx_signals = c.umx;
    if (c.common && !c.oamd_substream) {
        info.oamd_common = common_of(c);
    }
    info.iframe = iframe;
    info.substream_index = index;
    out.traces.emplace_back();
    out.substreams.push_back(audio_substream(audio, index, false, out.traces.back()));
    out.group.objects.push_back(info);
    ++index;
    if (c.oamd_substream) {
        out.traces.emplace_back();
        const auto keep = [&out](const ac4::SyntaxRecord& record) {
            out.traces.back().push_back(record);
        };
        BitWriter w(index, keep);
        std::vector<OamdObject> group = umx_objects;
        for (OamdObject& o : group) {
            o.ajoc_coded = true;
        }
        std::optional<ac4::detail::OamdCommonFields> common;
        if (c.common && iframe) {
            common = common_of(c);
        }
        std::optional<OamdTimingFields> timing;
        if (iframe) {
            timing = timing_of(c, frame);
            out.full_timing = timing;
        }
        ac4::detail::write_oamd_substream(w, common, timing, group, c.blocks, iframe, false, {});
        out.substreams.push_back(w.bytes());
        out.group.oamd_substream = index;
        out.group.oamd_iframe = iframe;
        out.oamd_index = index;
        ++index;
    }
    out.presentation_index = index;
}

// The direct-coded cases' substreams: their shares of the group's objects.
struct DirectSubstream {
    TocObjectSubstream info;
    int n_objects = 0;  // in the element
    bool lfe = false;   // mono_data(1) first
    int first_tone = 0;
};

[[nodiscard]] std::vector<DirectSubstream> direct_substreams(const ObjectCase& c) {
    using Kind = ObjectCase::Kind;
    std::vector<DirectSubstream> out;
    switch (c.kind) {
        case Kind::kDynamic: {
            // Three objects and the LFE, then one: 3.0 and mono.
            DirectSubstream a;
            a.info.n_objects_code = 3;
            a.info.dynamic = true;
            a.info.lfe = true;
            a.n_objects = 3;
            a.lfe = true;
            a.first_tone = 0;
            DirectSubstream b;
            b.info.n_objects_code = 1;
            b.info.dynamic = true;
            b.n_objects = 1;
            b.first_tone = 3;
            out = {a, b};
            break;
        }
        case Kind::kBed: {
            // A 5.1 bed (bed_chan_assign_code 2) in one substream: its five
            // fullband objects in the 5.0 element and its LFE before it.
            DirectSubstream a;
            a.info.n_objects_code = 4;
            a.info.dynamic = false;
            a.info.static_kind = TocObjectSubstream::Static::kBed;
            a.info.start = true;
            a.info.start_assignment = {.kind = TocObjectAssignment::Kind::kBedCode, .code = 2};
            a.n_objects = 5;
            a.lfe = true;
            out = {a};
            break;
        }
        default: {
            // SR3.1.0.0 (isf_config 0, four objects): three, then one.
            DirectSubstream a;
            a.info.n_objects_code = 3;
            a.info.dynamic = false;
            a.info.static_kind = TocObjectSubstream::Static::kIsf;
            a.info.start = true;
            a.info.start_assignment = {.kind = TocObjectAssignment::Kind::kIsf, .code = 0};
            a.n_objects = 3;
            DirectSubstream b;
            b.info.n_objects_code = 1;
            b.info.dynamic = false;
            b.info.static_kind = TocObjectSubstream::Static::kIsf;
            b.n_objects = 1;
            b.first_tone = 3;
            out = {a, b};
            break;
        }
    }
    return out;
}

// The group's objects as oamd_dyndata_multi() lists them: each substream's in
// turn; a bed's as its table lists them (L R C LFE Ls Rs for code 2).
[[nodiscard]] std::vector<OamdObject> direct_objects(const ObjectCase& c) {
    std::vector<OamdObject> out;
    switch (c.kind) {
        case ObjectCase::Kind::kDynamic:
            out.push_back({OamdObjectKind::kBed, true, false});
            for (int k = 0; k < 4; ++k) {
                out.push_back({OamdObjectKind::kDynamic, false, false});
            }
            break;
        case ObjectCase::Kind::kBed:
            for (int k = 0; k < 6; ++k) {
                out.push_back({OamdObjectKind::kBed, k == 3, false});
            }
            break;
        default:
            for (int k = 0; k < 4; ++k) {
                out.push_back({OamdObjectKind::kIsf, false, false});
            }
            break;
    }
    return out;
}

void build_direct_frame(const ObjectCase& c, int frame, int frames, bool iframe,
                        ac4::detail::Analysis& analysis, Frame& out) {
    int index = 0;
    for (DirectSubstream& s : direct_substreams(c)) {
        BitWriter audio = BitWriter::buffered();
        TrackWriter tracks(audio, analysis, frame);
        // audio_data_objs(n_objects, b_lfe): the LFE's mono_data(1), then the
        // element.
        if (s.lfe) {
            tracks.lfe();
        }
        write_element(audio, tracks, s.n_objects, s.first_tone);
        s.info.iframe = iframe;
        s.info.substream_index = index;
        out.traces.emplace_back();
        out.substreams.push_back(audio_substream(audio, index, c.dialogue, out.traces.back()));
        out.group.objects.push_back(s.info);
        ++index;
    }
    out.traces.emplace_back();
    const auto keep = [&out](const ac4::SyntaxRecord& record) {
        out.traces.back().push_back(record);
    };
    BitWriter w(index, keep);
    const std::vector<OamdObject> objects = direct_objects(c);
    std::optional<ac4::detail::OamdCommonFields> common;
    if (c.common && iframe) {
        common = common_of(c);
    }
    // The dynamic case sends its timing in I-frames alone; the others in
    // every frame.
    std::optional<OamdTimingFields> timing;
    if (iframe || c.kind != ObjectCase::Kind::kDynamic) {
        timing = timing_of(c, frame);
        out.full_timing = timing;
    }
    const std::vector<ObjectInfoBlockFields> blocks = blocks_of(c, objects, frame, frames, iframe);
    ac4::detail::write_oamd_substream(w, common, timing, objects, c.blocks, iframe, false, blocks);
    out.substreams.push_back(w.bytes());
    out.group.oamd_substream = index;
    out.group.oamd_iframe = iframe;
    out.oamd_index = index;
    out.presentation_index = index + 1;
}

// The expected output of full and core decoding.
void expect(const ObjectCase& c, int frames, BuiltObjectStream& out) {
    const auto positions = [&](int index) {
        std::vector<std::array<int, 4>> p;
        for (int f = 0; f < frames; ++f) {
            p.push_back(position_of(index, f, c.blocks - 1, frames, c.blocks));
        }
        return p;
    };
    if (c.kind == ObjectCase::Kind::kAjoc || c.kind == ObjectCase::Kind::kAjocStatic) {
        const int m = num_dmx(c);
        const int bands = ac4::detail::ajoc_band_count(c.bands_code);
        const bool is_static = c.kind == ObjectCase::Kind::kAjocStatic;
        if (c.lfe) {
            out.full.push_back({.lfe = true, .tones = {{kLfeToneHz, kAmplitude}}});
            out.core.push_back({.lfe = true, .tones = {{kLfeToneHz, kAmplitude}}});
        }
        for (int o = 0; o < c.umx; ++o) {
            ExpectedObject object;
            for (int ch = 0; ch < m; ++ch) {
                const int track = qin_track(c, ch);
                const int pb = band_of(bands, tone_subband(track));
                const int steps = dry_steps(c, o, ch, pb);
                if (steps != 0) {
                    object.tones.push_back(
                        {object_tone_hz(track), std::abs(steps) * step_of(c) * kAmplitude});
                }
            }
            for (int d = 0; d < c.decorr; ++d) {
                object.decorrelated = object.decorrelated || wet_steps(c, o, d) != 0;
            }
            object.positions = positions(o);
            out.full.push_back(object);
        }
        for (int ch = 0; ch < m; ++ch) {
            ExpectedObject object;
            object.tones.push_back({object_tone_hz(qin_track(c, ch)), kAmplitude});
            if (!is_static) {
                object.positions = positions(ch);
            }
            out.core.push_back(object);
        }
        return;
    }
    // Direct-coded: the objects in each substream's order, the LFE first, then
    // the element's channels L R C Ls Rs.
    int dynamic_index = 0;
    for (const DirectSubstream& s : direct_substreams(c)) {
        if (s.lfe) {
            out.full.push_back({.lfe = true, .tones = {{kLfeToneHz, kAmplitude}}});
        }
        // The element's channels in L R C Ls Rs order carry tones first_tone
        // + 0, 1, 2, 3 and 4 (mono: C the first).
        for (int k = 0; k < s.n_objects; ++k) {
            ExpectedObject object;
            object.tones.push_back({object_tone_hz(s.first_tone + k), kAmplitude});
            if (c.kind == ObjectCase::Kind::kDynamic) {
                object.positions = positions(dynamic_index++);
            }
            out.full.push_back(object);
        }
    }
    out.core = out.full;
}

}  // namespace

double ajoc_dry_coefficient(const ObjectCase& c, int o, int ch) {
    const int pb =
        band_of(ac4::detail::ajoc_band_count(c.bands_code), tone_subband(qin_track(c, ch)));
    return dry_steps(c, o, ch, pb) * step_of(c);
}

double ajoc_input_tone_hz(const ObjectCase& c, int ch) {
    return object_tone_hz(qin_track(c, ch));
}

double ajoc_dialogue_dmx_coefficient(const ObjectCase& c, int ch) {
    return c.dialogue ? dialogue_coeff_code(ch) / 15.0 : 0.0;
}

double object_tone_hz(int k) {
    return (static_cast<double>(tone_subband(k)) + 0.5) * static_cast<double>(kRate) / 128.0;
}

BuiltObjectStream build_objects(const ObjectCase& c, int frames) {
    BuiltObjectStream out;
    const std::optional<ac4::detail::AspxSetup> setup =
        ac4::detail::aspx_setup_for(kAspxKbpsPerChannel, kRate);
    if (!setup) {
        throw std::runtime_error("no A-SPX configuration at 48 kHz");
    }
    ac4::detail::Analysis analysis(kFrameLength, 1);
    std::optional<OamdTimingFields> carried;
    for (int f = 0; f < frames; ++f) {
        const bool iframe = f % 4 == 0;
        Frame frame;
        if (c.kind == ObjectCase::Kind::kAjoc || c.kind == ObjectCase::Kind::kAjocStatic) {
            build_ajoc_frame(c, f, frames, iframe, analysis, *setup, frame);
        } else {
            build_direct_frame(c, f, frames, iframe, analysis, frame);
        }
        frame.traces.emplace_back();
        frame.substreams.push_back(presentation_substream(frame.presentation_index,
                                                          c.kind == ObjectCase::Kind::kAjocStatic,
                                                          frame.traces.back()));

        ac4::detail::TocLayout layout;
        layout.sequence_counter = f + 1;
        layout.iframe_global = iframe;
        ac4::detail::TocPresentation p;
        p.groups = {0};
        p.md_compat = 3;
        p.pres_ndot = iframe;
        p.presentation_substream = frame.presentation_index;
        layout.presentations.push_back(p);
        layout.groups.push_back(frame.group);
        if (frame.full_timing) {
            carried = frame.full_timing;
        }
        out.update_samples.push_back(carried ? update_samples_of(*carried) : std::vector<int>{});
        auto raw = ac4::detail::assemble_frame(layout, frame.substreams);
        if (!raw) {
            throw std::runtime_error("the table of contents writer refused a frame");
        }
        out.frames.push_back(std::move(*raw));
        // The decoder reads the OAMD substream first, then the rest in index
        // order.
        std::vector<ac4::SyntaxRecord>& trace = out.traces.emplace_back();
        if (frame.oamd_index >= 0) {
            const auto& oamd = frame.traces[at(frame.oamd_index)];
            trace.insert(trace.end(), oamd.begin(), oamd.end());
        }
        for (std::size_t s = 0; s < frame.traces.size(); ++s) {
            if (static_cast<int>(s) != frame.oamd_index) {
                trace.insert(trace.end(), frame.traces[s].begin(), frame.traces[s].end());
            }
        }
    }
    expect(c, frames, out);
    return out;
}

std::vector<std::byte> sync_framed(const BuiltObjectStream& stream) {
    std::vector<std::byte> out;
    for (const auto& frame : stream.frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

std::vector<ObjectCase> committed_object_cases() {
    using Kind = ObjectCase::Kind;
    return {
        // Two downmix signals to four objects in one band; the downmix's own
        // timing, the upmix's own and derived from it in turn.
        {.name = "ajoc-2to4-coarse", .kind = Kind::kAjoc, .dmx = 2, .umx = 4},
        // Five signals with an LFE in ASPX (Pseudocode 14a's rotation), three
        // decorrelators, 23 bands, fine, sparse, two data points, DIFF_TIME,
        // an absent object, dialogue, two blocks, the extras and
        // ajoc_bed_info().
        {.name = "ajoc-5lfe-aspx-decorr-sparse",
         .kind = Kind::kAjoc,
         .dmx = 5,
         .aspx = true,
         .lfe = true,
         .var_coding_config = 1,
         .umx = 7,
         .decorr = 3,
         .bands_code = 0,
         .quant = 0,
         .sparse = true,
         .dpoints = 2,
         .ramp = 8,
         .diff_time = true,
         .absent = true,
         .dialogue = true,
         .blocks = 2,
         .common = true,
         .extras = true,
         .bed_info = true},
        // Four signals over var_coding_config's even form, with the group's
        // OAMD substream sending the timing and common data.
        {.name = "ajoc-4-oamd-substream",
         .kind = Kind::kAjoc,
         .dmx = 4,
         .umx = 6,
         .bands_code = 4,
         .oamd_substream = true,
         .common = true},
        // Three signals, var_coding_config 0, and a static 5.1 downmix.
        {.name = "ajoc-3-config0",
         .kind = Kind::kAjoc,
         .dmx = 3,
         .umx = 5,
         .bands_code = 6,
         .dpoints = 0},
        {.name = "ajoc-static-5_1",
         .kind = Kind::kAjocStatic,
         .lfe = true,
         .umx = 8,
         .bands_code = 5},
        // Direct-coded dynamic objects, a bed and an ISF.
        {.name = "direct-dynamic",
         .kind = Kind::kDynamic,
         .blocks = 2,
         .common = true,
         .extras = true},
        {.name = "direct-bed-5_1", .kind = Kind::kBed},
        {.name = "direct-isf-sr3100", .kind = Kind::kIsf},
    };
}

}  // namespace ac4dec_test
