#include "pcm/acpl.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <numbers>

#include "syntax/acpl.hpp"

namespace ac4::detail {
namespace {

using acpl::kSubbands;

constexpr double kSqrt2 = std::numbers::sqrt2;

// The parameters, products and sums Pseudocodes 118 and 119 interpolate:
// gamma1 to gamma6, two sums of gammas, four gammas times an alpha, beta1,
// beta2, beta3 and beta3 times each alpha.
constexpr std::size_t kCouplingInterpolations = 17;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] acpl::Quant quant_of(int quant_mode) noexcept {
    return quant_mode == 0 ? acpl::Quant::kFine : acpl::Quant::kCoarse;
}

[[nodiscard]] acpl::Framing framing_of(const AcplFraming& framing) noexcept {
    return acpl::Framing{.steep = framing.interpolation_type == 1,
                         .num_param_sets = framing.num_param_sets,
                         .param_timeslot = {framing.param_timeslot[0], framing.param_timeslot[1]}};
}

using QuantSets = std::array<std::array<int, acpl::kMaxParamBands>, acpl::kMaxParamSets>;

// Pseudocode 121 for each parameter set of one parameter: its codebook
// indices less cb_off, each set differenced against the one before, the first
// against `history`, which ends at the last set.
[[nodiscard]] ParseResult decode_sets(const AcplParams& params, acpl::Kind kind, int num_sets, int start_band,
                                      int num_bands, std::array<int, acpl::kMaxParamBands>& history,
                                      QuantSets& out) {
    const acpl::Quant quant = quant_of(params.quant_mode);
    for (int ps = 0; ps < num_sets; ++ps) {
        const AcplParamSet& set = params.sets[at(ps)];
        const bool diff_time = set.diff_type == 1;
        const int f0_off = acpl_codebook(params.data_type, params.quant_mode, AcplHcbType::kF0).cb_off;
        const int rest_off =
            acpl_codebook(params.data_type, params.quant_mode, diff_time ? AcplHcbType::kDt : AcplHcbType::kDf)
                .cb_off;
        std::array<int, acpl::kMaxParamBands> coded{};
        for (int i = start_band; i < num_bands; ++i) {
            const int off = !diff_time && i == start_band ? f0_off : rest_off;
            coded[at(i)] = static_cast<int>(set.huff_index[at(i)]) - off;
        }
        if (!acpl::differential_decode(kind, quant, diff_time, start_band, num_bands, coded, history,
                                       out[at(ps)])) {
            return fail(DecodeError::kInvalidStream, "an A-CPL parameter outside its quantisation table");
        }
        history = out[at(ps)];
    }
    return {};
}

// Tables 203 to 206 for an alpha and the beta that goes with it. Bands below
// start_band carry no values and are 0 (src/ac4dec/ERRATA.md, "Partial
// coupling starts at acpl_param_band").
void dequantise_alpha_beta(const QuantSets& alpha_q, const QuantSets& beta_q, acpl::Quant quant, int num_sets,
                           int start_band, int num_bands, acpl::ParamSets& alpha, acpl::ParamSets& beta) {
    alpha = {};
    beta = {};
    for (std::size_t ps = 0; ps < at(num_sets); ++ps) {
        for (int i = start_band; i < num_bands; ++i) {
            const acpl::AlphaValue a = acpl::dequantise_alpha(alpha_q[ps][at(i)], quant);
            alpha[ps][at(i)] = a.alpha;
            beta[ps][at(i)] = acpl::dequantise_beta(beta_q[ps][at(i)], a.ibeta, quant);
        }
    }
}

// Tables 207 and 208: a beta3 or gamma value times its step.
void dequantise_step(const QuantSets& q, double step, int num_sets, int num_bands, acpl::ParamSets& out) {
    out = {};
    for (std::size_t ps = 0; ps < at(num_sets); ++ps) {
        for (std::size_t i = 0; i < at(num_bands); ++i) {
            out[ps][i] = q[ps][i] * step;
        }
    }
}

// Table 202, and the 5.X element's fixed assignment: the channels of A-CPL's
// x0, x1, x2, x3, x4, x6 and x7 (the outputs z0, z2, z4, z1, z3, z6, z7).
struct AcplMapping {
    Speaker x0 = Speaker::kLeft;
    Speaker x1 = Speaker::kRight;
    Speaker x3 = Speaker::kLeftSurround;
    Speaker x4 = Speaker::kRightSurround;
    std::optional<std::array<Speaker, 2>> x6_x7;
    // Pseudocode 120's scalings: z0 and z2 when they are the surrounds, z6
    // and z7 when they are.
    bool scale_z0_z2 = false;
    bool scale_z6_z7 = false;
};

[[nodiscard]] AcplMapping mapping_of(int ch_mode, bool add_ch_base, ElementKind kind) {
    using S = Speaker;
    AcplMapping out;
    if (kind != ElementKind::k7X) {
        return out;
    }
    // 3/4/0 sends no add_ch_base; Table 202 codes its surrounds against the
    // back pair, as add_ch_base 1 codes the 5/2/0 and 3/2/2 surrounds against
    // their last pair, and Pseudocode 120's scalings are read alike
    // (src/ac4dec/ERRATA.md, "add_ch_base in 3/4/0").
    const bool back = ch_mode == ch_mode::k7_0_340 || ch_mode == ch_mode::k7_1_340;
    const bool surround_base = back || add_ch_base;
    std::array<S, 2> last{};
    if (back) {
        last = {S::kLeftBack, S::kRightBack};
    } else if (ch_mode == ch_mode::k7_0_520 || ch_mode == ch_mode::k7_1_520) {
        last = {S::kLeftWide, S::kRightWide};
    } else {
        last = {S::kTopFrontLeft, S::kTopFrontRight};
    }
    out.x0 = surround_base ? S::kLeftSurround : S::kLeft;
    out.x1 = surround_base ? S::kRightSurround : S::kRight;
    out.x3 = last[0];
    out.x4 = last[1];
    out.x6_x7 = surround_base ? std::array<S, 2>{S::kLeft, S::kRight}
                              : std::array<S, 2>{S::kLeftSurround, S::kRightSurround};
    out.scale_z0_z2 = surround_base;
    out.scale_z6_z7 = !surround_base;
    return out;
}

void scale(std::span<QmfValue> values, double gain) {
    for (QmfValue& v : values) {
        v *= gain;
    }
}

}  // namespace

ParseResult acpl_values(const ChannelElement& element, AcplQuantHistory& history, AcplFrameValues& out) {
    out = AcplFrameValues{};
    if (element.codec_mode == codec_mode::kAspxAcpl3) {
        if (!element.acpl_2ch) {
            return fail(DecodeError::kInvalidStream, "an ASPX_ACPL_3 element without its acpl_data_2ch()");
        }
        const AcplData2ch& data = *element.acpl_2ch;
        AcplCouplingValues values;
        values.framing = framing_of(data.framing);
        values.num_bands = data.num_bands;
        const int sets = values.framing.num_param_sets;
        const int bands = data.num_bands;
        // Table 62's order, which AcplQuantHistory::coupling keeps.
        const std::array<const AcplParams*, 11> params = {
            &data.alpha[0], &data.alpha[1], &data.beta[0],  &data.beta[1],  &data.beta3,    &data.gamma[0],
            &data.gamma[1], &data.gamma[2], &data.gamma[3], &data.gamma[4], &data.gamma[5]};
        const std::array<acpl::Kind, 11> kinds = {acpl::Kind::kAlpha, acpl::Kind::kAlpha, acpl::Kind::kBeta,
                                                  acpl::Kind::kBeta,  acpl::Kind::kBeta3, acpl::Kind::kGamma,
                                                  acpl::Kind::kGamma, acpl::Kind::kGamma, acpl::Kind::kGamma,
                                                  acpl::Kind::kGamma, acpl::Kind::kGamma};
        std::array<QuantSets, 11> q{};
        for (std::size_t p = 0; p < params.size(); ++p) {
            if (auto ok = decode_sets(*params[p], kinds[p], sets, 0, bands, history.coupling[p], q[p]); !ok) {
                return ok;
            }
        }
        const acpl::Quant quant_0 = quant_of(data.alpha[0].quant_mode);
        const acpl::Quant quant_1 = quant_of(data.gamma[0].quant_mode);
        for (std::size_t k = 0; k < 2; ++k) {
            dequantise_alpha_beta(q[k], q[2 + k], quant_0, sets, 0, bands, values.alpha[k], values.beta[k]);
        }
        dequantise_step(q[4], acpl::beta3_step(quant_0), sets, bands, values.beta3);
        for (std::size_t k = 0; k < 6; ++k) {
            dequantise_step(q[5 + k], acpl::gamma_step(quant_1), sets, bands, values.gamma[k]);
        }
        out.coupling = values;
        return {};
    }
    const std::size_t expected = element.kind == ElementKind::kPair ? 1 : 2;
    if (element.acpl_1ch.size() != expected) {
        return fail(DecodeError::kInvalidStream, "an A-CPL element without its acpl_data_1ch()");
    }
    for (std::size_t m = 0; m < expected; ++m) {
        const AcplData1ch& data = element.acpl_1ch[m];
        AcplModuleValues& values = out.modules[m];
        values.framing = framing_of(data.framing);
        values.num_bands = data.num_bands;
        values.qmf_band = data.qmf_band;
        const int sets = values.framing.num_param_sets;
        QuantSets alpha_q{};
        QuantSets beta_q{};
        if (auto ok = decode_sets(data.alpha1, acpl::Kind::kAlpha, sets, data.start_band, data.num_bands,
                                  history.modules[m][0], alpha_q);
            !ok) {
            return ok;
        }
        if (auto ok = decode_sets(data.beta1, acpl::Kind::kBeta, sets, data.start_band, data.num_bands,
                                  history.modules[m][1], beta_q);
            !ok) {
            return ok;
        }
        dequantise_alpha_beta(alpha_q, beta_q, quant_of(data.alpha1.quant_mode), sets, data.start_band,
                              data.num_bands, values.alpha, values.beta);
    }
    out.module_count = expected;
    return {};
}

AcplStage::AcplStage() : decorrelators_{acpl::Decorrelator<double>(0), acpl::Decorrelator<double>(1),
                                        acpl::Decorrelator<double>(2)} {}

void AcplStage::reset() {
    for (auto& decorrelator : decorrelators_) {
        decorrelator.reset();
    }
    for (auto& ducker : duckers_) {
        ducker.reset();
    }
    module_prev_ = {};
    coupling_prev_ = {};
}

void AcplStage::interpolate(const acpl::Framing& framing, int num_bands, const Param& param, int num_ts,
                            std::vector<double>& out) const {
    out.resize(at(num_ts) * kSubbands);
    acpl::interpolate(framing, num_bands, param.values, param.prev, num_ts, out);
}

// Pseudocode 111, then Pseudocode 114 with the gains of Pseudocodes 112 and
// 113 (src/ac4dec/ERRATA.md, "The transient ducker's energy").
void AcplStage::decorrelate(int decorrelator, std::span<const QmfValue> in, std::span<QmfValue> out, int num_ts) {
    decorrelators_[at(decorrelator)].process(in, out, num_ts);
    duckers_[at(decorrelator)].process(out, num_ts);
}

// Pseudocodes 115 and 116 for one module: x0 and x1 are the channels as they
// came from A-SPX, before Pseudocode 115's doubling; x1 is empty where the
// mode passes 0 for it.
void AcplStage::module(const AcplModuleValues& values, int index, std::span<const QmfValue> x0,
                       std::span<const QmfValue> x1, std::span<QmfValue> z0, std::span<QmfValue> z1, int num_ts) {
    const std::size_t n = at(num_ts) * kSubbands;
    work_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        work_[i] = 2.0 * x0[i];
    }
    std::vector<QmfValue>& y = decorrelated_[at(index)];
    y.resize(n);
    decorrelate(index, work_, y, num_ts);

    std::array<acpl::ParamPrev, 2>& prev = module_prev_[at(index)];
    const Param alpha{values.alpha, prev[0]};
    const Param beta{values.beta, prev[1]};
    interpolate(values.framing, values.num_bands, alpha, num_ts, interp_[0]);
    interpolate(values.framing, values.num_bands, beta, num_ts, interp_[1]);
    for (std::size_t ts = 0; ts < at(num_ts); ++ts) {
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            const std::size_t i = ts * kSubbands + sb;
            const QmfValue x0in = work_[i];
            const QmfValue x1in = x1.empty() ? QmfValue{} : 2.0 * x1[i];
            if (static_cast<int>(sb) < values.qmf_band) {
                z0[i] = 0.5 * (x0in + x1in);
                z1[i] = 0.5 * (x0in - x1in);
            } else {
                const double a = interp_[0][i];
                const double b = interp_[1][i];
                z0[i] = 0.5 * (x0in * (1.0 + a) + y[i] * b);
                z1[i] = 0.5 * (x0in * (1.0 - a) - y[i] * b);
            }
        }
    }
    acpl::end_frame(values.framing, values.num_bands, values.alpha, prev[0]);
    acpl::end_frame(values.framing, values.num_bands, values.beta, prev[1]);
}

// Pseudocodes 118 and 119: z = {z0, z1, z2, z3, z4}, the outputs L, Ls, R,
// Rs and C, from x0 and x1, L and R as they came from A-SPX.
void AcplStage::coupling(const AcplCouplingValues& values, std::span<const QmfValue> x0,
                         std::span<const QmfValue> x1, std::span<std::span<QmfValue>, 5> z, int num_ts) {
    const std::size_t n = at(num_ts) * kSubbands;
    const double input_gain = 1.0 + 2.0 * std::sqrt(0.5);
    std::vector<QmfValue>& x0in = in_[0];
    std::vector<QmfValue>& x1in = in_[1];
    x0in.resize(n);
    x1in.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        x0in[i] = input_gain * x0[i];
        x1in[i] = input_gain * x1[i];
    }

    // The parameters, with acpl_param_prev, in AcplQuantHistory::coupling's
    // order; the products and sums the pseudocode interpolates take their
    // acpl_param_prev from the same products and sums of the parameters'.
    const auto base = [&](std::size_t k, const acpl::ParamSets& sets) { return Param{sets, coupling_prev_[k]}; };
    const Param a1 = base(0, values.alpha[0]);
    const Param a2 = base(1, values.alpha[1]);
    const Param b1 = base(2, values.beta[0]);
    const Param b2 = base(3, values.beta[1]);
    const Param b3 = base(4, values.beta3);
    std::array<Param, 6> g{};
    for (std::size_t k = 0; k < 6; ++k) {
        g[k] = base(5 + k, values.gamma[k]);
    }
    const auto combine = [](const Param& p, const Param& q, auto op) {
        Param out;
        for (std::size_t ps = 0; ps < acpl::kMaxParamSets; ++ps) {
            for (std::size_t pb = 0; pb < acpl::kMaxParamBands; ++pb) {
                out.values[ps][pb] = op(p.values[ps][pb], q.values[ps][pb]);
            }
        }
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            out.prev[sb] = op(p.prev[sb], q.prev[sb]);
        }
        return out;
    };
    const auto times = [&](const Param& p, const Param& q) { return combine(p, q, std::multiplies<>{}); };
    const auto plus = [&](const Param& p, const Param& q) { return combine(p, q, std::plus<>{}); };

    // One interpolated matrix per parameter, product or sum used, in scratch
    // sized once so that the references below stay valid.
    std::size_t slots_used = 0;
    interp_scratch_.resize(kCouplingInterpolations);
    const auto interp = [&](const Param& p) -> const std::vector<double>& {
        std::vector<double>& out = interp_scratch_[slots_used++];
        interpolate(values.framing, values.num_bands, p, num_ts, out);
        return out;
    };
    const std::vector<double>& ig1 = interp(g[0]);
    const std::vector<double>& ig2 = interp(g[1]);
    const std::vector<double>& ig3 = interp(g[2]);
    const std::vector<double>& ig4 = interp(g[3]);
    const std::vector<double>& ig5 = interp(g[4]);
    const std::vector<double>& ig6 = interp(g[5]);
    const std::vector<double>& ig135 = interp(plus(plus(g[0], g[2]), g[4]));
    const std::vector<double>& ig246 = interp(plus(plus(g[1], g[3]), g[5]));
    const std::vector<double>& ig1a1 = interp(times(g[0], a1));
    const std::vector<double>& ig2a1 = interp(times(g[1], a1));
    const std::vector<double>& ig3a2 = interp(times(g[2], a2));
    const std::vector<double>& ig4a2 = interp(times(g[3], a2));
    const std::vector<double>& ib1 = interp(b1);
    const std::vector<double>& ib2 = interp(b2);
    const std::vector<double>& ib3 = interp(b3);
    const std::vector<double>& ib3a1 = interp(times(b3, a1));
    const std::vector<double>& ib3a2 = interp(times(b3, a2));

    // Transform() into the three decorrelators' inputs, then their outputs.
    std::array<std::vector<QmfValue>, 3>& v = transformed_;
    for (auto& matrix : v) {
        matrix.resize(n);
    }
    for (std::size_t i = 0; i < n; ++i) {
        v[0][i] = x0in[i] * ig1[i] + x1in[i] * ig2[i];
        v[1][i] = x0in[i] * ig3[i] + x1in[i] * ig4[i];
        v[2][i] = x0in[i] * ig135[i] + x1in[i] * ig246[i];
    }
    for (int d = 0; d < acpl::kDecorrelators; ++d) {
        decorrelated_[at(d)].resize(n);
        decorrelate(d, v[at(d)], decorrelated_[at(d)], num_ts);
    }
    const std::vector<QmfValue>& y0 = decorrelated_[0];
    const std::vector<QmfValue>& y1 = decorrelated_[1];
    const std::vector<QmfValue>& y2 = decorrelated_[2];

    for (std::size_t i = 0; i < n; ++i) {
        const QmfValue l = x0in[i];
        const QmfValue r = x1in[i];
        // ACplModule2() for (z0, z1), (z2, z3) and (z4, z5), whose z5 is 0.
        QmfValue z0 = 0.5 * (l * (ig1[i] + ig1a1[i]) + r * (ig2[i] + ig2a1[i]) + y0[i] * ib1[i]);
        QmfValue z1 = 0.5 * (l * (ig1[i] - ig1a1[i]) + r * (ig2[i] - ig2a1[i]) - y0[i] * ib1[i]);
        QmfValue z2 = 0.5 * (l * (ig3[i] + ig3a2[i]) + r * (ig4[i] + ig4a2[i]) + y1[i] * ib2[i]);
        QmfValue z3 = 0.5 * (l * (ig3[i] - ig3a2[i]) + r * (ig4[i] - ig4a2[i]) - y1[i] * ib2[i]);
        QmfValue z4 = l * ig5[i] + r * ig6[i];
        // ACplModule3() with beta3: (b3, a1), (b3, a2), and (-b3, 1), whose
        // interp_b3 and interp_b3_a are both -interp(b3).
        z0 += 0.25 * y2[i] * (ib3[i] + ib3a1[i]);
        z1 += 0.25 * y2[i] * (ib3[i] - ib3a1[i]);
        z2 += 0.25 * y2[i] * (ib3[i] + ib3a2[i]);
        z3 += 0.25 * y2[i] * (ib3[i] - ib3a2[i]);
        z4 -= 0.5 * y2[i] * ib3[i];
        z[0][i] = z0;
        z[1][i] = kSqrt2 * z1;
        z[2][i] = z2;
        z[3][i] = kSqrt2 * z3;
        z[4][i] = kSqrt2 * z4;
    }

    const std::array<const acpl::ParamSets*, 11> sets = {
        &values.alpha[0], &values.alpha[1], &values.beta[0],  &values.beta[1],  &values.beta3,    &values.gamma[0],
        &values.gamma[1], &values.gamma[2], &values.gamma[3], &values.gamma[4], &values.gamma[5]};
    for (std::size_t k = 0; k < sets.size(); ++k) {
        acpl::end_frame(values.framing, values.num_bands, *sets[k], coupling_prev_[k]);
    }
}

void AcplStage::apply(int ch_mode, bool add_ch_base, ElementKind kind, int codec_mode, const AcplFrameValues& values,
                      int num_ts, const AcplChannels& channels) {
    const std::size_t n = at(num_ts) * kSubbands;
    const auto matrix_of = [&](Speaker speaker) -> std::vector<QmfValue>* {
        for (std::size_t c = 0; c < channels.speakers.size(); ++c) {
            if (channels.speakers[c] == speaker && c < channels.matrices.size()) {
                return channels.matrices[c];
            }
        }
        return nullptr;
    };
    // The inputs are copied first: every output overwrites a channel an
    // input came from.
    const auto input = [&](std::size_t slot, Speaker speaker) -> std::span<const QmfValue> {
        const std::vector<QmfValue>* matrix = matrix_of(speaker);
        std::vector<QmfValue>& copy = in_[slot];
        copy.assign(n, QmfValue{});
        if (matrix != nullptr && matrix->size() >= n) {
            std::copy_n(matrix->begin(), n, copy.begin());
        }
        return copy;
    };
    const auto output = [&](Speaker speaker) -> std::span<QmfValue> {
        std::vector<QmfValue>* matrix = matrix_of(speaker);
        if (matrix == nullptr || matrix->size() < n) {
            return {};
        }
        return std::span<QmfValue>(*matrix).first(n);
    };
    const auto writable = [&](std::initializer_list<Speaker> speakers) {
        return std::ranges::all_of(speakers, [&](Speaker s) { return !output(s).empty(); });
    };

    using S = Speaker;
    if (codec_mode == codec_mode::kAspxAcpl3) {
        if (!values.coupling || !writable({S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround})) {
            return;
        }
        // coupling() copies its inputs before it writes.
        std::array<std::span<QmfValue>, 5> z = {output(S::kLeft), output(S::kLeftSurround), output(S::kRight),
                                               output(S::kRightSurround), output(S::kCentre)};
        const std::span<const QmfValue> x0 = input(2, S::kLeft);
        const std::span<const QmfValue> x1 = input(3, S::kRight);
        coupling(*values.coupling, x0, x1, z, num_ts);
        return;
    }
    const bool residuals = codec_mode == codec_mode::kAspxAcpl1;
    if (kind == ElementKind::kPair) {
        if (values.module_count != 1 || !writable({S::kLeft, S::kRight})) {
            return;
        }
        const std::span<const QmfValue> x0 = input(0, S::kLeft);
        const std::span<const QmfValue> x1 = residuals ? input(1, S::kRight) : std::span<const QmfValue>{};
        module(values.modules[0], 0, x0, x1, output(S::kLeft), output(S::kRight), num_ts);
        return;
    }
    const AcplMapping map = mapping_of(ch_mode, add_ch_base, kind);
    if (values.module_count != 2 || !writable({map.x0, map.x1, map.x3, map.x4})) {
        return;
    }
    const std::span<const QmfValue> x0 = input(0, map.x0);
    const std::span<const QmfValue> x1 = input(1, map.x1);
    const std::span<const QmfValue> x3 = residuals ? input(3, map.x3) : std::span<const QmfValue>{};
    const std::span<const QmfValue> x4 = residuals ? input(4, map.x4) : std::span<const QmfValue>{};
    module(values.modules[0], 0, x0, x3, output(map.x0), output(map.x3), num_ts);
    module(values.modules[1], 1, x1, x4, output(map.x1), output(map.x4), num_ts);
    scale(output(map.x3), kSqrt2);
    scale(output(map.x4), kSqrt2);
    if (map.scale_z0_z2) {
        scale(output(map.x0), kSqrt2);
        scale(output(map.x1), kSqrt2);
    }
    if (map.x6_x7 && map.scale_z6_z7) {
        scale(output((*map.x6_x7)[0]), kSqrt2);
        scale(output((*map.x6_x7)[1]), kSqrt2);
    }
}

}  // namespace ac4::detail
