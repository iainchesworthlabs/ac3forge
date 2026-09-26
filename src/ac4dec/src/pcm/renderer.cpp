#include "pcm/renderer.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>

namespace ac4::detail {
namespace {

using S = Speaker;

// The generalized rendering matrix's channel indices (5.10.2.2).
constexpr std::size_t kL = 0;
constexpr std::size_t kR = 1;
constexpr std::size_t kC = 2;
constexpr std::size_t kLs = 3;
constexpr std::size_t kRs = 4;
constexpr std::size_t kLb = 5;
constexpr std::size_t kRb = 6;
constexpr std::size_t kTfl = 7;
constexpr std::size_t kTfr = 8;
constexpr std::size_t kTbl = 9;
constexpr std::size_t kTbr = 10;
constexpr std::size_t kLfe = 11;
constexpr std::size_t kTsl = 12;
constexpr std::size_t kTsr = 13;
constexpr std::size_t kIndices = 14;

constexpr std::size_t kNone = kIndices;

using Rows = std::array<std::array<double, kIndices>, kIndices>;  // [out][in]

// A coefficient in dB as the tables print it, linear (src/ac4dec/ERRATA.md,
// "The downmix gains").
[[nodiscard]] double from_db(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

// Table 129's gains, by code; code 7, -inf dB, is 0.
[[nodiscard]] double table_129(int code) noexcept {
    constexpr std::array<double, 7> kDb = {0.0, -1.5, -3.0, -4.5, -6.0, -9.0, -12.0};
    if (code < 0 || code >= 7) {
        return 0.0;
    }
    return from_db(kDb[static_cast<std::size_t>(code)]);
}

[[nodiscard]] std::size_t index_of(Speaker speaker) noexcept {
    switch (speaker) {
        case S::kLeft:
            return kL;
        case S::kRight:
            return kR;
        case S::kCentre:
            return kC;
        case S::kLeftSurround:
            return kLs;
        case S::kRightSurround:
            return kRs;
        case S::kLeftBack:
            return kLb;
        case S::kRightBack:
            return kRb;
        case S::kTopFrontLeft:
            return kTfl;
        case S::kTopFrontRight:
            return kTfr;
        case S::kTopBackLeft:
            return kTbl;
        case S::kTopBackRight:
            return kTbr;
        case S::kLfe:
            return kLfe;
        case S::kTopSideLeft:
            return kTsl;
        case S::kTopSideRight:
            return kTsr;
        default:
            return kNone;
    }
}

// Where a decoded channel of full decoding enters the matrix: its own index,
// the side pair's where it carries a .2 source's Tsl and Tsr (Table 59), and
// nowhere where the input configuration leaves it out.
[[nodiscard]] std::size_t full_input(const ImmersiveLayout& layout, Speaker speaker) noexcept {
    const std::size_t i = index_of(speaker);
    if ((i == kLb || i == kRb) && !layout.backs) {
        return kNone;
    }
    if (i >= kTfl && i <= kTbr) {
        switch (layout.tops) {
            case 3:
                return i;
            case 1:
                return i == kTfl ? kTsl : (i == kTfr ? kTsr : kNone);
            case 2:
                return i == kTbl ? kTsl : (i == kTbr ? kTsr : kNone);
            default:
                return kNone;
        }
    }
    return i;
}

void diagonal(Rows& r, std::initializer_list<std::size_t> indices) {
    for (const std::size_t i : indices) {
        r[i][i] = 1.0;
    }
}

// (out1, in1) and (out2, in2) at `value`: a pair of coefficients, left and
// right, as the tables list them.
void pair(Rows& r, std::size_t out1, std::size_t in1, std::size_t out2, std::size_t in2,
          double value) {
    r[out1][in1] = value;
    r[out2][in2] = value;
}

// Tables 38 to 43: full decoding, from `in` to `out`.
[[nodiscard]] Rows full_rows(const ChannelConfiguration& in, const ChannelConfiguration& out,
                             const RenderGains& g) {
    Rows r{};
    const double m3 = from_db(-3.0);
    const auto& t2 = g.gain_t2;
    const bool in7 = in.width == 7;
    diagonal(r, {kL, kR, kC});
    // The surround and back channels.
    if (out.width == 7) {
        diagonal(r, {kLs, kRs});
        if (in7) {
            diagonal(r, {kLb, kRb});
        }
    } else if (in7) {
        // Tables 41 to 43: the backs folded into the sides, at gain_b, or at
        // -3 dB from a 7.X.0 input, which takes no custom downmix data.
        const double b = in.tops == 0 ? m3 : g.gain_b;
        pair(r, kLs, kLs, kLs, kLb, b);
        pair(r, kRs, kRs, kRs, kRb, b);
    } else {
        diagonal(r, {kLs, kRs});
    }
    // The top channels.
    if (in.tops == 4) {
        switch (out.tops) {
            case 4:
                diagonal(r, {kTfl, kTfr, kTbl, kTbr});
                break;
            case 2: {
                // Table 39 takes -3 dB from 5.X.4, which has no 7.X.2 downmix
                // data; Tables 39 and 42 take gain_t1 otherwise.
                const double t1 = out.width == 7 && !in7 ? m3 : g.gain_t1;
                pair(r, kTsl, kTfl, kTsl, kTbl, t1);
                pair(r, kTsr, kTfr, kTsr, kTbr, t1);
                break;
            }
            default:
                if (out.width == 7 && !in7) {
                    // Table 40, 5.X.4: into the sides at -3 dB.
                    pair(r, kLs, kTfl, kRs, kTfr, m3);
                    pair(r, kLs, kTbl, kRs, kTbr, m3);
                    break;
                }
                pair(r, kL, kTfl, kR, kTfr, t2[0]);
                pair(r, kLs, kTfl, kRs, kTfr, t2[1]);
                pair(r, kL, kTbl, kR, kTbr, t2[3]);
                pair(r, kLs, kTbl, kRs, kTbr, t2[4]);
                if (out.width == 7) {
                    // Table 40, 7.X.4: into the backs as well.
                    pair(r, kLb, kTfl, kRb, kTfr, t2[2]);
                    pair(r, kLb, kTbl, kRb, kTbr, t2[5]);
                }
                break;
        }
    } else if (in.tops == 2) {
        switch (out.tops) {
            case 4:
                pair(r, kTfl, kTsl, kTfr, kTsr, m3);
                pair(r, kTbl, kTsl, kTbr, kTsr, m3);
                break;
            case 2:
                diagonal(r, {kTsl, kTsr});
                break;
            default:
                if (out.width == 7 && !in7) {
                    // Table 40, 5.X.2.
                    pair(r, kLs, kTsl, kRs, kTsr, m3);
                    break;
                }
                pair(r, kL, kTsl, kR, kTsr, t2[0]);
                pair(r, kLs, kTsl, kRs, kTsr, t2[1]);
                if (out.width == 7) {
                    pair(r, kLb, kTsl, kRb, kTsr, t2[2]);
                }
                break;
        }
    }
    return r;
}

// Tables 45 and 46: core decoding, to 5.X.2 or 5.X.0, by the source's
// top_channels_present and b_4_back_channels_present.
[[nodiscard]] Rows core_rows(const ImmersiveLayout& layout, const ChannelConfiguration& out,
                             const RenderGains& g) {
    Rows r{};
    const double p3 = from_db(3.0);
    diagonal(r, {kL, kR, kC});
    const double side = layout.backs ? g.gain_b * p3 : p3;
    r[kLs][kLs] = side;
    r[kRs][kRs] = side;
    if (layout.tops != 0) {
        if (out.tops == 2) {
            const double top = layout.tops == 3 ? g.gain_t1 * p3 : p3;
            r[kTsl][kTsl] = top;
            r[kTsr][kTsr] = top;
        } else {
            pair(r, kL, kTsl, kR, kTsr, g.gain_t2[0] * p3);
            pair(r, kLs, kTsl, kRs, kTsr, g.gain_t2[1] * p3);
        }
    }
    return r;
}

}  // namespace

ChannelConfiguration input_configuration(const ImmersiveLayout& layout) noexcept {
    const int tops = layout.tops == 3 ? 4 : (layout.tops == 0 ? 0 : 2);
    return {.width = layout.backs ? 7 : 5, .tops = tops};
}

RenderGains render_gains(const CustomDmxData* cdmx, int out_ch_config) noexcept {
    // Table 130.
    const double m3 = from_db(-3.0);
    RenderGains g;
    g.gain_b = m3;
    g.gain_t1 = m3;
    g.gain_t2 = {0.0, m3, 0.0, 0.0, m3, 0.0};
    if (cdmx == nullptr || !cdmx->b_cdmx_data_present) {
        return g;
    }
    const CdmxParameters* own = nullptr;
    const CdmxParameters* t4_to_t2 = nullptr;  // out_ch_config 1's, for the exception
    for (int dc = 0; dc < cdmx->n_cdmx_configs && dc < static_cast<int>(cdmx->cdmx.size()); ++dc) {
        const CdmxParameters& p = cdmx->cdmx[static_cast<std::size_t>(dc)];
        if (p.out_ch_config == out_ch_config) {
            own = &p;
        }
        if (p.out_ch_config == 1) {
            t4_to_t2 = &p;
        }
    }
    const auto take = [](const std::optional<int>& code, double& gain) {
        if (code) {
            gain = table_129(*code);
        }
    };
    if (own != nullptr) {
        take(own->gain_b_code, g.gain_b);
        take(own->gain_t1_code, g.gain_t1);
        take(own->gain_t2a_code, g.gain_t2[0]);
        take(own->gain_t2b_code, g.gain_t2[1]);
        take(own->gain_t2c_code, g.gain_t2[2]);
        take(own->gain_t2d_code, g.gain_t2[3]);
        take(own->gain_t2e_code, g.gain_t2[4]);
        take(own->gain_t2f_code, g.gain_t2[5]);
    }
    // 6.3.10.3.10's exception: 7.X.4 to 7.X.2 takes out_ch_config 1's
    // tool_t4_to_t2() where out_ch_config 4 sends none.
    if (cdmx->bs_ch_config == 1 && out_ch_config == 4 && (own == nullptr || !own->gain_t1_code) &&
        t4_to_t2 != nullptr) {
        take(t4_to_t2->gain_t1_code, g.gain_t1);
    }
    return g;
}

RenderPlan render_plan(const ImmersiveLayout& layout, DownmixTarget target) {
    RenderPlan plan;
    switch (target) {
        case DownmixTarget::kAsCoded:
            plan.output = input_configuration(layout);
            break;
        case DownmixTarget::k7X4:
            plan.output = {.width = 7, .tops = 4};
            break;
        case DownmixTarget::k7X2:
            plan.output = {.width = 7, .tops = 2};
            break;
        case DownmixTarget::k7X0:
            plan.output = {.width = 7, .tops = 0};
            break;
        case DownmixTarget::k5X4:
            plan.output = {.width = 5, .tops = 4};
            break;
        case DownmixTarget::k5X2:
            plan.output = {.width = 5, .tops = 2};
            break;
        case DownmixTarget::k5X:
            plan.output = {.width = 5, .tops = 0};
            break;
        case DownmixTarget::kStereo:
        case DownmixTarget::kLoRo:
        case DownmixTarget::kLtRt:
        case DownmixTarget::kMono:
            plan.output = {.width = 5, .tops = 0};
            plan.stereo = true;
            break;
    }
    if (layout.decoding == DecodingMode::kCore) {
        // Table 44: core decoding renders to 5.X.2 and 5.X.0 alone.
        plan.output = {.width = 5, .tops = plan.output.tops == 0 ? 0 : 2};
    }
    plan.speakers = {S::kLeft, S::kRight, S::kCentre};
    if (layout.lfe) {
        plan.speakers.push_back(S::kLfe);
    }
    plan.speakers.push_back(S::kLeftSurround);
    plan.speakers.push_back(S::kRightSurround);
    if (plan.output.width == 7) {
        plan.speakers.push_back(S::kLeftBack);
        plan.speakers.push_back(S::kRightBack);
    }
    if (plan.output.tops == 4) {
        plan.speakers.insert(plan.speakers.end(), {S::kTopFrontLeft, S::kTopFrontRight,
                                                   S::kTopBackLeft, S::kTopBackRight});
    } else if (plan.output.tops == 2) {
        plan.speakers.insert(plan.speakers.end(), {S::kTopSideLeft, S::kTopSideRight});
    }
    return plan;
}

std::vector<std::vector<double>> render_matrix(const ImmersiveLayout& layout,
                                               std::span<const Speaker> decoded,
                                               const RenderPlan& plan, const RenderGains& gains) {
    const bool core = layout.decoding == DecodingMode::kCore;
    Rows rows = core ? core_rows(layout, plan.output, gains)
                     : full_rows(input_configuration(layout), plan.output, gains);
    // Note 2 of 5.10.2.5 and 5.10.2.7: the LFE through where there is one.
    if (layout.lfe) {
        rows[kLfe][kLfe] = 1.0;
    }
    std::vector<std::vector<double>> m(plan.speakers.size(),
                                       std::vector<double>(decoded.size(), 0.0));
    for (std::size_t d = 0; d < decoded.size(); ++d) {
        const std::size_t in = core ? index_of(decoded[d]) : full_input(layout, decoded[d]);
        if (in == kNone) {
            continue;  // silenced: not in the input channel configuration
        }
        for (std::size_t o = 0; o < plan.speakers.size(); ++o) {
            const std::size_t out = index_of(plan.speakers[o]);
            if (out != kNone) {
                m[o][d] = rows[out][in];
            }
        }
    }
    return m;
}

std::optional<int> out_ch_config(const ChannelConfiguration& output) noexcept {
    if (output.width == 5) {
        return output.tops / 2;  // 5.X.0, 5.X.2 and 5.X.4: 0, 1 and 2
    }
    if (output.tops == 4) {
        return std::nullopt;
    }
    return output.tops == 0 ? 3 : 4;
}

LoudCorrOutput loud_corr_output(const ImmersiveLayout& layout,
                                const ChannelConfiguration& output) noexcept {
    const ChannelConfiguration input = input_configuration(layout);
    if (output.width >= input.width && output.tops >= input.tops) {
        return LoudCorrOutput::kNone;  // nothing downmixed
    }
    if (layout.decoding == DecodingMode::kCore) {
        return output.tops == 2 ? LoudCorrOutput::kCore5X2 : LoudCorrOutput::kCore5X;
    }
    if (output.width == 5) {
        return output.tops == 0 ? LoudCorrOutput::k5X
                                : (output.tops == 2 ? LoudCorrOutput::k5X2 : LoudCorrOutput::k5X4);
    }
    return output.tops == 0 ? LoudCorrOutput::k7X
                            : (output.tops == 2 ? LoudCorrOutput::k7X2 : LoudCorrOutput::k7X4);
}

}  // namespace ac4::detail
