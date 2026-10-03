#include "pcm/acpl.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numbers>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "syntax/acpl.hpp"
#include "syntax/reset.hpp"

namespace iclforge::ac4::detail {
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
    const auto g = static_cast<Real>(gain);
    for (QmfValue& v : values) {
        v *= g;
    }
}

// The subbands of a frame that share a parameter band and, in every one of `prevs`, an
// acpl_param_prev: Pseudocode 109 gives them the same value at every slot, to the bit, so a run's
// is formed once and its subbands take it. A run starts where the band or one of the prevs differs
// from the subband before's, which is what acpl::interpolate() starts one at.
struct Runs {
    std::array<std::uint8_t, kSubbands> first{};  // the first subband of each run, in order
    int count = 0;

    [[nodiscard]] int begin(int run) const noexcept { return first[at(run)]; }
    [[nodiscard]] int end(int run) const noexcept {
        return run + 1 < count ? first[at(run + 1)] : kSubbands;
    }
};

[[nodiscard]] Runs runs_of(int num_param_bands,
                           std::span<const acpl::ParamPrev* const> prevs) noexcept {
    Runs runs;
    int run_band = -1;
    for (int sb = 0; sb < kSubbands; ++sb) {
        const int pb = std::max(acpl::sb_to_pb(num_param_bands, sb), 0);
        bool starts = sb == 0 || pb != run_band;
        for (std::size_t k = 0; k < prevs.size() && !starts; ++k) {
            const acpl::ParamPrev& prev = *prevs[k];
            starts = std::bit_cast<std::uint64_t>(prev[at(sb)]) !=
                     std::bit_cast<std::uint64_t>(prev[at(sb - 1)]);
        }
        if (starts) {
            runs.first[at(runs.count)] = static_cast<std::uint8_t>(sb);
            ++runs.count;
            run_band = pb;
        }
    }
    return runs;
}

// One parameter's interpolation column for the run that starts at subband `sb`.
[[nodiscard]] acpl::Interpolator::Column column_of(int num_param_bands,
                                                   const acpl::ParamSets& values,
                                                   const acpl::ParamPrev& prev, int sb) noexcept {
    const auto pb = at(std::max(acpl::sb_to_pb(num_param_bands, sb), 0));
    return acpl::Interpolator::column(prev[at(sb)], values[0][pb], values[1][pb]);
}

}  // namespace

ParseResult acpl_values(const ChannelElement& element, AcplQuantHistory& history, AcplFrameValues& out) {
    reset_in_place(out);
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
    std::size_t expected = 2;
    if (element.kind == ElementKind::kPair) {
        expected = 1;
    } else if (element.kind == ElementKind::kImmersive) {
        expected = kMaxAcplModules;
    }
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

AcplStage::AcplStage() = default;

void AcplStage::reset() {
    for (auto& decorrelator : decorrelators_) {
        if (decorrelator != nullptr) {
            decorrelator->reset();
        }
    }
    for (auto& ducker : duckers_) {
        ducker.reset();
    }
    module_prev_ = {};
    coupling_prev_ = {};
}

// Pseudocode 111, then Pseudocode 114 with the gains of Pseudocodes 112 and
// 113 (src/ac4dec/ERRATA.md, "The transient ducker's energy").
void AcplStage::decorrelate(int decorrelator, std::span<const QmfValue> in, std::span<QmfValue> out, int num_ts) {
    // D0, D1 and D2, then the immersive element's second D0 and D1.
    static constexpr std::array<int, kDecorrelatorSlots> kIndex = {0, 1, 2, 0, 1};
    std::unique_ptr<acpl::Decorrelator<Real>>& slot = decorrelators_[at(decorrelator)];
    if (slot == nullptr) {
        slot = std::make_unique<acpl::Decorrelator<Real>>(kIndex[at(decorrelator)]);
    }
    slot->process(in, out, num_ts);
    duckers_[at(decorrelator)].process(out, num_ts);
}

// Pseudocodes 115 and 116 for one module: x0 and x1 are the channels as they
// came from A-SPX, before Pseudocode 115's doubling; x1 is empty where the
// mode passes 0 for it.
void AcplStage::module(const AcplModuleValues& values, int index, int decorrelator,
                       std::span<const QmfValue> x0, std::span<const QmfValue> x1,
                       std::span<QmfValue> z0, std::span<QmfValue> z1, int num_ts) {
    const std::size_t n = at(num_ts) * kSubbands;
    work_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        work_[i] = Real{2} * x0[i];
    }
    std::vector<QmfValue>& y = decorrelated_[at(decorrelator)];
    y.resize(n);
    decorrelate(decorrelator, work_, y, num_ts);

    std::array<acpl::ParamPrev, 2>& prev = module_prev_[at(index)];
    const std::array<const acpl::ParamPrev*, 2> prevs = {&prev[0], &prev[1]};
    const Runs runs = runs_of(values.num_bands, prevs);
    const acpl::Interpolator interpolator(values.framing, num_ts);
    columns_.resize(at(runs.count) * 2);
    for (int run = 0; run < runs.count; ++run) {
        columns_[at(run) * 2] = column_of(values.num_bands, values.alpha, prev[0], runs.begin(run));
        columns_[at(run) * 2 + 1] =
            column_of(values.num_bands, values.beta, prev[1], runs.begin(run));
    }
    for (int ts = 0; ts < num_ts; ++ts) {
        for (int run = 0; run < runs.count; ++run) {
            // alpha and beta are ac4core's own double-precision interpolation (acpl::Interpolator
            // is not retemplated on Real; see this class's declaration), narrowed once for the run.
            const auto a = static_cast<Real>(interpolator.at(columns_[at(run) * 2], ts));
            const auto b = static_cast<Real>(interpolator.at(columns_[at(run) * 2 + 1], ts));
            for (int sb = runs.begin(run); sb < runs.end(run); ++sb) {
                const std::size_t i = at(ts) * kSubbands + at(sb);
                const QmfValue x0in = work_[i];
                const QmfValue x1in = x1.empty() ? QmfValue{} : Real{2} * x1[i];
                if (sb < values.qmf_band) {
                    z0[i] = Real(0.5) * (x0in + x1in);
                    z1[i] = Real(0.5) * (x0in - x1in);
                } else {
                    z0[i] = Real(0.5) * (x0in * (Real{1} + a) + y[i] * b);
                    z1[i] = Real(0.5) * (x0in * (Real{1} - a) - y[i] * b);
                }
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
    const auto input_gain = static_cast<Real>(1.0 + 2.0 * std::sqrt(0.5));
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
    std::array<Param, 6>& g = coupling_g_;
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

    // The seventeen parameters, products and sums Pseudocodes 118 and 119
    // interpolate, in the order of the coefficients below.
    std::array<Param, 8>& derived = coupling_derived_;
    derived[0] = plus(plus(g[0], g[2]), g[4]);
    derived[1] = plus(plus(g[1], g[3]), g[5]);
    derived[2] = times(g[0], a1);
    derived[3] = times(g[1], a1);
    derived[4] = times(g[2], a2);
    derived[5] = times(g[3], a2);
    derived[6] = times(b3, a1);
    derived[7] = times(b3, a2);
    const std::array<const Param*, kCouplingInterpolations> params = {
        &g[0],       &g[1],       &g[2],       &g[3],       &g[4],       &g[5],
        &derived[0], &derived[1], &derived[2], &derived[3], &derived[4], &derived[5],
        &b1,         &b2,         &b3,         &derived[6], &derived[7]};
    enum Interpolated : std::size_t {
        kIg1,
        kIg2,
        kIg3,
        kIg4,
        kIg5,
        kIg6,
        kIg135,
        kIg246,
        kIg1a1,
        kIg2a1,
        kIg3a2,
        kIg4a2,
        kIb1,
        kIb2,
        kIb3,
        kIb3a1,
        kIb3a2
    };

    // Pseudocode 109 once for each run of subbands that shares a parameter band and every
    // parameter's acpl_param_prev, and at each slot: the coefficients the loops below multiply by.
    // ac4core's interpolation is in double (see this class's declaration); each sum below is formed
    // in that double precision, as the original single-scalar code computed it, and narrowed to
    // Real once, at the multiply into a QmfValue - the double build stays bit-for-bit since
    // narrowing a double to double is the identity.
    std::array<const acpl::ParamPrev*, kCouplingInterpolations> prevs{};
    for (std::size_t k = 0; k < params.size(); ++k) {
        prevs[k] = &params[k]->prev;
    }
    const Runs runs = runs_of(values.num_bands, prevs);
    const acpl::Interpolator interpolator(values.framing, num_ts);
    columns_.resize(at(runs.count) * kCouplingInterpolations);
    for (int run = 0; run < runs.count; ++run) {
        for (std::size_t k = 0; k < params.size(); ++k) {
            columns_[at(run) * kCouplingInterpolations + k] =
                column_of(values.num_bands, params[k]->values, params[k]->prev, runs.begin(run));
        }
    }
    coupling_coefficients_.resize(at(num_ts) * at(runs.count));
    for (int ts = 0; ts < num_ts; ++ts) {
        for (int run = 0; run < runs.count; ++run) {
            const acpl::Interpolator::Column* columns =
                &columns_[at(run) * kCouplingInterpolations];
            std::array<double, kCouplingInterpolations> ip{};
            for (std::size_t k = 0; k < ip.size(); ++k) {
                ip[k] = interpolator.at(columns[k], ts);
            }
            CouplingCoefficients& c = coupling_coefficients_[at(ts) * at(runs.count) + at(run)];
            c.ig1 = static_cast<Real>(ip[kIg1]);
            c.ig2 = static_cast<Real>(ip[kIg2]);
            c.ig3 = static_cast<Real>(ip[kIg3]);
            c.ig4 = static_cast<Real>(ip[kIg4]);
            c.ig135 = static_cast<Real>(ip[kIg135]);
            c.ig246 = static_cast<Real>(ip[kIg246]);
            c.z0_l = static_cast<Real>(ip[kIg1] + ip[kIg1a1]);
            c.z0_r = static_cast<Real>(ip[kIg2] + ip[kIg2a1]);
            c.z0_y = static_cast<Real>(ip[kIb1]);
            c.z1_l = static_cast<Real>(ip[kIg1] - ip[kIg1a1]);
            c.z1_r = static_cast<Real>(ip[kIg2] - ip[kIg2a1]);
            c.z2_l = static_cast<Real>(ip[kIg3] + ip[kIg3a2]);
            c.z2_r = static_cast<Real>(ip[kIg4] + ip[kIg4a2]);
            c.z2_y = static_cast<Real>(ip[kIb2]);
            c.z3_l = static_cast<Real>(ip[kIg3] - ip[kIg3a2]);
            c.z3_r = static_cast<Real>(ip[kIg4] - ip[kIg4a2]);
            c.z4_l = static_cast<Real>(ip[kIg5]);
            c.z4_r = static_cast<Real>(ip[kIg6]);
            c.y2_z0 = static_cast<Real>(ip[kIb3] + ip[kIb3a1]);
            c.y2_z1 = static_cast<Real>(ip[kIb3] - ip[kIb3a1]);
            c.y2_z2 = static_cast<Real>(ip[kIb3] + ip[kIb3a2]);
            c.y2_z3 = static_cast<Real>(ip[kIb3] - ip[kIb3a2]);
            c.y2_z4 = static_cast<Real>(ip[kIb3]);
        }
    }

    // Transform() into the three decorrelators' inputs, then their outputs.
    std::array<std::vector<QmfValue>, 3>& v = transformed_;
    for (auto& matrix : v) {
        matrix.resize(n);
    }
    for (int ts = 0; ts < num_ts; ++ts) {
        for (int run = 0; run < runs.count; ++run) {
            const CouplingCoefficients& c =
                coupling_coefficients_[at(ts) * at(runs.count) + at(run)];
            for (int sb = runs.begin(run); sb < runs.end(run); ++sb) {
                const std::size_t i = at(ts) * kSubbands + at(sb);
                v[0][i] = x0in[i] * c.ig1 + x1in[i] * c.ig2;
                v[1][i] = x0in[i] * c.ig3 + x1in[i] * c.ig4;
                v[2][i] = x0in[i] * c.ig135 + x1in[i] * c.ig246;
            }
        }
    }
    for (int d = 0; d < acpl::kDecorrelators; ++d) {
        decorrelated_[at(d)].resize(n);
        decorrelate(d, v[at(d)], decorrelated_[at(d)], num_ts);
    }
    const std::vector<QmfValue>& y0 = decorrelated_[0];
    const std::vector<QmfValue>& y1 = decorrelated_[1];
    const std::vector<QmfValue>& y2 = decorrelated_[2];

    const auto sqrt2 = static_cast<Real>(kSqrt2);
    for (int ts = 0; ts < num_ts; ++ts) {
        for (int run = 0; run < runs.count; ++run) {
            const CouplingCoefficients& c =
                coupling_coefficients_[at(ts) * at(runs.count) + at(run)];
            for (int sb = runs.begin(run); sb < runs.end(run); ++sb) {
                const std::size_t i = at(ts) * kSubbands + at(sb);
                const QmfValue l = x0in[i];
                const QmfValue r = x1in[i];
                // ACplModule2() for (z0, z1), (z2, z3) and (z4, z5), whose z5 is 0.
                QmfValue z0 = Real(0.5) * (l * c.z0_l + r * c.z0_r + y0[i] * c.z0_y);
                QmfValue z1 = Real(0.5) * (l * c.z1_l + r * c.z1_r - y0[i] * c.z0_y);
                QmfValue z2 = Real(0.5) * (l * c.z2_l + r * c.z2_r + y1[i] * c.z2_y);
                QmfValue z3 = Real(0.5) * (l * c.z3_l + r * c.z3_r - y1[i] * c.z2_y);
                QmfValue z4 = l * c.z4_l + r * c.z4_r;
                // ACplModule3() with beta3: (b3, a1), (b3, a2), and (-b3, 1), whose
                // interp_b3 and interp_b3_a are both -interp(b3).
                z0 += Real(0.25) * y2[i] * c.y2_z0;
                z1 += Real(0.25) * y2[i] * c.y2_z1;
                z2 += Real(0.25) * y2[i] * c.y2_z2;
                z3 += Real(0.25) * y2[i] * c.y2_z3;
                z4 -= Real(0.5) * y2[i] * c.y2_z4;
                z[0][i] = z0;
                z[1][i] = sqrt2 * z1;
                z[2][i] = z2;
                z[3][i] = sqrt2 * z3;
                z[4][i] = sqrt2 * z4;
            }
        }
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
    AC4_ZONE_SCOPED_N("ac4_acpl");
    const std::size_t n = at(num_ts) * kSubbands;
    const auto matrix_of = [&](Speaker speaker) -> QmfMatrix {
        for (std::size_t c = 0; c < channels.speakers.size(); ++c) {
            if (channels.speakers[c] == speaker && c < channels.matrices.size()) {
                return channels.matrices[c];
            }
        }
        return {};
    };
    // The inputs are copied first: every output overwrites a channel an
    // input came from.
    const auto input = [&](std::size_t slot, Speaker speaker) -> std::span<const QmfValue> {
        const QmfMatrix matrix = matrix_of(speaker);
        std::vector<QmfValue>& copy = in_[slot];
        copy.assign(n, QmfValue{});
        if (matrix.size() >= n) {
            std::copy_n(matrix.begin(), n, copy.begin());
        }
        return copy;
    };
    const auto output = [&](Speaker speaker) -> std::span<QmfValue> {
        const QmfMatrix matrix = matrix_of(speaker);
        if (matrix.size() < n) {
            return {};
        }
        return matrix.first(n);
    };
    const auto writable = [&](std::initializer_list<Speaker> speakers) {
        return std::ranges::all_of(speakers, [&](Speaker s) { return !output(s).empty(); });
    };

    using S = Speaker;
    if (kind == ElementKind::kImmersive) {
        // Part 2 Pseudocode 2 with b_5fronts 0: modules 1 to 4 on (Ls, Lb),
        // (Rs, Rb), (Tfl, Tbl) and (Tfr, Tbr) (Table 25's x5/x7, x6/x8, x9/x11
        // and x10/x12), whose decorrelators are D0, D0, D1 and D1, each its own
        // instance; x7, x8, x11 and x12 are the ASPX_ACPL_1 residuals and 0 in
        // ASPX_ACPL_2. Then z0, z2 and z4 are twice L, R and C, and every
        // module's outputs are scaled by the square root of 2.
        constexpr std::array<std::array<S, 2>, kMaxAcplModules> kPairs = {
            {{S::kLeftSurround, S::kLeftBack},
             {S::kRightSurround, S::kRightBack},
             {S::kTopFrontLeft, S::kTopBackLeft},
             {S::kTopFrontRight, S::kTopBackRight}}};
        constexpr std::array<int, kMaxAcplModules> kDecorrelator = {0, acpl::kDecorrelators, 1,
                                                                    acpl::kDecorrelators + 1};
        if (values.module_count != kMaxAcplModules ||
            !writable({S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kLeftBack,
                       S::kRightSurround, S::kRightBack, S::kTopFrontLeft, S::kTopBackLeft,
                       S::kTopFrontRight, S::kTopBackRight})) {
            return;
        }
        const bool residuals = codec_mode == immersive_mode::kAspxAcpl1;
        for (std::size_t m = 0; m < kMaxAcplModules; ++m) {
            const std::span<const QmfValue> x0 = input(0, kPairs[m][0]);
            const std::span<const QmfValue> x1 =
                residuals ? input(1, kPairs[m][1]) : std::span<const QmfValue>{};
            module(values.modules[m], static_cast<int>(m), kDecorrelator[m], x0, x1,
                   output(kPairs[m][0]), output(kPairs[m][1]), num_ts);
            scale(output(kPairs[m][0]), kSqrt2);
            scale(output(kPairs[m][1]), kSqrt2);
        }
        for (const Speaker front : {S::kLeft, S::kRight, S::kCentre}) {
            scale(output(front), 2.0);
        }
        return;
    }
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
        module(values.modules[0], 0, 0, x0, x1, output(S::kLeft), output(S::kRight), num_ts);
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
    module(values.modules[0], 0, 0, x0, x3, output(map.x0), output(map.x3), num_ts);
    module(values.modules[1], 1, 1, x1, x4, output(map.x1), output(map.x4), num_ts);
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

}  // namespace iclforge::ac4::detail
