#include "pcm/ajcc.hpp"

#include <algorithm>
#include <initializer_list>
#include <numbers>
#include <span>

namespace ac4::detail {
namespace {

using S = Speaker;

constexpr double kSqrt2 = std::numbers::sqrt2;

// Pseudocodes 8 and 12: every input times 2 + 1/sqrt(2).
constexpr double kInputGain = 2.0 + 1.0 / std::numbers::sqrt2;

// AjccFrameValues::q's parameters, in syntax order.
enum Param : std::uint8_t {
    kAlpha1,
    kAlpha2,
    kBeta1,
    kBeta2,
    kDry1,
    kDry2,
    kDry3,
    kDry4,
    kWet1,
    kWet2,
    kWet3,
    kWet4,
    kWet5,
    kWet6,
};

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] acpl::Quant quant_of(int quant_mode) noexcept {
    return quant_mode == 0 ? acpl::Quant::kFine : acpl::Quant::kCoarse;
}

[[nodiscard]] acpl::Framing framing_of(const AjccFraming& framing) noexcept {
    return acpl::Framing{.steep = framing.interpolation_type == 1,
                         .num_param_sets = framing.num_param_sets,
                         .param_timeslot = {framing.param_timeslot[0], framing.param_timeslot[1]}};
}

// One module's dequantised parameters in parameter set `ps` and band `pb`
// (Pseudocodes 4 and 5, and Part 1 Tables 203 to 206 for alpha and beta).
[[nodiscard]] ajcc::ModuleParams module_params(const AjccFrameValues& v, std::size_t side,
                                               std::size_t ps, std::size_t pb) noexcept {
    const auto q = [&](Param left, Param right) {
        return static_cast<int>(v.q[side == 0 ? left : right][ps][pb]);
    };
    const acpl::AlphaValue alpha = acpl::dequantise_alpha(q(kAlpha1, kAlpha2), v.quant_ab);
    using ajcc::Kind;
    return {.alpha = alpha.alpha,
            .beta = acpl::dequantise_beta(q(kBeta1, kBeta2), alpha.ibeta, v.quant_ab),
            .dry1 = ajcc::dequantise(Kind::kDry, q(kDry1, kDry3), v.quant_dw),
            .dry2 = ajcc::dequantise(Kind::kDry, q(kDry2, kDry4), v.quant_dw),
            .wet1 = ajcc::dequantise(Kind::kWet, q(kWet1, kWet4), v.quant_dw),
            .wet2 = ajcc::dequantise(Kind::kWet, q(kWet2, kWet5), v.quant_dw),
            .wet3 = ajcc::dequantise(Kind::kWet, q(kWet3, kWet6), v.quant_dw)};
}

}  // namespace

ParseResult ajcc_values(const AjccData& data, AjccQuantHistory& history, AjccFrameValues& out) {
    out = AjccFrameValues{};
    out.core_mode = data.core_mode;
    out.num_bands = ajcc::num_param_bands(data.num_param_bands_id);
    out.quant_ab = quant_of(data.qm_ab);
    out.quant_dw = quant_of(data.qm_dw);
    for (std::size_t side = 0; side < 2; ++side) {
        out.framing[side] = framing_of(data.framing[side]);
    }
    const std::array<const AjccParams*, kAjccParams> params = {
        &data.alpha[0], &data.alpha[1], &data.beta[0], &data.beta[1], &data.dry[0],
        &data.dry[1],   &data.dry[2],   &data.dry[3],  &data.wet[0],  &data.wet[1],
        &data.wet[2],   &data.wet[3],   &data.wet[4],  &data.wet[5]};
    for (std::size_t p = 0; p < params.size(); ++p) {
        const AjccParams& param = *params[p];
        const acpl::Quant quant = quant_of(param.quant_mode);
        acpl::Range range{};
        switch (param.data_type) {
            case AjccDataType::kAlpha:
                range = acpl::quantised_range(acpl::Kind::kAlpha, quant);
                break;
            case AjccDataType::kBeta:
                range = acpl::quantised_range(acpl::Kind::kBeta, quant);
                break;
            case AjccDataType::kDry:
                range = ajcc::quantised_range(ajcc::Kind::kDry, quant);
                break;
            case AjccDataType::kWet:
                range = ajcc::quantised_range(ajcc::Kind::kWet, quant);
                break;
        }
        for (std::size_t ps = 0; ps < param.num_param_sets && ps < ajcc::kMaxParamSets; ++ps) {
            const AjccParamSet& set = param.sets[ps];
            const bool diff_time = set.diff_type == 1;
            const int f0_off =
                ajcc_codebook(param.data_type, param.quant_mode, AjccHcbType::kF0).cb_off;
            const int rest_off = ajcc_codebook(param.data_type, param.quant_mode,
                                               diff_time ? AjccHcbType::kDt : AjccHcbType::kDf)
                                     .cb_off;
            std::array<int, ajcc::kMaxParamBands> coded{};
            for (int i = 0; i < out.num_bands; ++i) {
                const int off = !diff_time && i == 0 ? f0_off : rest_off;
                coded[at(i)] = static_cast<int>(set.huff_index[at(i)]) - off;
            }
            std::array<int, ajcc::kMaxParamBands> decoded{};
            if (!ajcc::differential_decode(range, diff_time, out.num_bands, coded,
                                           history.params[p], decoded)) {
                return fail(DecodeError::kInvalidStream,
                            "an A-JCC parameter outside its quantisation range");
            }
            history.params[p] = decoded;
            for (int i = 0; i < out.num_bands; ++i) {
                out.q[p][ps][at(i)] = static_cast<std::int8_t>(decoded[at(i)]);
            }
        }
    }
    return {};
}

AjccStage::AjccStage()
    : decorrelators_{acpl::Decorrelator<double>(0), acpl::Decorrelator<double>(2),
                     acpl::Decorrelator<double>(1), acpl::Decorrelator<double>(0),
                     acpl::Decorrelator<double>(2), acpl::Decorrelator<double>(1)} {}

void AjccStage::reset() {
    for (auto& decorrelator : decorrelators_) {
        decorrelator.reset();
    }
    for (auto& ducker : duckers_) {
        ducker.reset();
    }
    pre_.reset();
    prev_ = {};
}

// Pseudocode 111 on `in`, then Pseudocode 114's ducking, as A-CPL's
// inputSignalModification() and applyTransientDucker() (clause 5.6.3.4).
void AjccStage::decorrelate(std::size_t slot, const std::vector<QmfValue>& in,
                            std::vector<QmfValue>& out, int num_ts) {
    out.resize(in.size());
    decorrelators_[slot].process(in, out, num_ts);
    duckers_[slot].process(out, num_ts);
}

// Pseudocode 11 (full decoding) or 14 (core decoding) for one side: each
// coefficient per parameter set and band, interpolated with its own
// ajcc_param_prev and weighting the input it goes with into its output.
void AjccStage::module(DecodingMode decoding, std::size_t side, const AjccFrameValues& values,
                       int num_ts, std::array<const std::vector<QmfValue>*, 2> x,
                       std::array<const std::vector<QmfValue>*, 3> y,
                       std::array<std::vector<QmfValue>*, ajcc::kModule2Outputs> z) {
    const bool full = decoding == DecodingMode::kFull;
    const std::size_t outputs = full ? ajcc::kModule2Outputs : ajcc::kModule4Outputs;
    const std::size_t count = full ? ajcc::kModule2Coefficients : ajcc::kModule4Coefficients;
    const acpl::Framing& framing = values.framing[side];
    const std::size_t sets = at(std::clamp(framing.num_param_sets, 1, ajcc::kMaxParamSets));
    auto& coefficients = coefficients_[side];
    auto& prev = prev_[side];
    for (auto& coefficient : coefficients) {
        coefficient = {};
    }
    for (std::size_t ps = 0; ps < sets; ++ps) {
        for (std::size_t pb = 0; pb < at(values.num_bands); ++pb) {
            const ajcc::ModuleParams p = module_params(values, side, ps, pb);
            if (full) {
                const auto c = ajcc::module_2(values.core_mode, p);
                for (std::size_t k = 0; k < count; ++k) {
                    coefficients[k][ps][pb] = c[k];
                }
            } else {
                const auto c = ajcc::module_4(values.core_mode, p);
                for (std::size_t k = 0; k < count; ++k) {
                    coefficients[k][ps][pb] = c[k];
                }
            }
        }
    }
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    for (std::size_t o = 0; o < outputs; ++o) {
        z[o]->assign(n, QmfValue{});
    }
    interp_.resize(n);
    for (std::size_t k = 0; k < count; ++k) {
        // A coefficient that is 0 now and was 0 before adds nothing.
        const bool silent = std::ranges::all_of(prev[k], [](double v) { return v == 0.0; }) &&
                            std::ranges::all_of(coefficients[k], [](const auto& set) {
                                return std::ranges::all_of(set, [](double v) { return v == 0.0; });
                            });
        if (!silent) {
            acpl::interpolate(framing, values.num_bands, coefficients[k], prev[k], num_ts, interp_);
            const ajcc::Term term = ajcc::module_term(k, outputs);
            const std::vector<QmfValue>& in = term.decorrelated ? *y[term.input] : *x[term.input];
            ajcc::accumulate<double>(interp_, in, *z[term.output], num_ts);
        }
        acpl::end_frame(framing, values.num_bands, coefficients[k], prev[k]);
    }
}

void AjccStage::apply(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
                      const AcplChannels& channels) {
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    const auto matrix_of = [&](Speaker speaker) -> std::vector<QmfValue>* {
        for (std::size_t c = 0; c < channels.speakers.size() && c < channels.matrices.size(); ++c) {
            if (channels.speakers[c] == speaker && channels.matrices[c]->size() >= n) {
                return channels.matrices[c];
            }
        }
        return nullptr;
    };
    const bool full = decoding == DecodingMode::kFull;
    const std::array<std::array<S, ajcc::kModule2Outputs>, 2> kFullOutputs = {
        {{S::kLeft, S::kLeftSurround, S::kLeftBack, S::kTopFrontLeft, S::kTopBackLeft},
         {S::kRight, S::kRightSurround, S::kRightBack, S::kTopFrontRight, S::kTopBackRight}}};
    const std::array<std::array<S, ajcc::kModule4Outputs>, 2> kCoreOutputs = {
        {{S::kLeft, S::kLeftSurround, S::kTopSideLeft},
         {S::kRight, S::kRightSurround, S::kTopSideRight}}};
    std::array<std::array<std::vector<QmfValue>*, ajcc::kModule2Outputs>, 2> z{};
    for (std::size_t side = 0; side < 2; ++side) {
        const std::size_t outputs = full ? ajcc::kModule2Outputs : ajcc::kModule4Outputs;
        for (std::size_t o = 0; o < outputs; ++o) {
            z[side][o] = matrix_of(full ? kFullOutputs[side][o] : kCoreOutputs[side][o]);
            if (z[side][o] == nullptr) {
                return;
            }
        }
    }
    std::vector<QmfValue>* centre = matrix_of(S::kCentre);
    if (centre == nullptr) {
        return;
    }
    // x0, x1, x3 and x4 (L, R, Ls and Rs holding A'', B'', D'' and E''), times
    // the input gain, before any output overwrites them.
    const std::array<S, 4> kInputs = {S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround};
    for (std::size_t i = 0; i < kInputs.size(); ++i) {
        const std::vector<QmfValue>& source = *matrix_of(kInputs[i]);
        x_in_[i].resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            x_in_[i][k] = kInputGain * source[k];
        }
    }
    const std::vector<QmfValue>& x0 = x_in_[0];
    const std::vector<QmfValue>& x1 = x_in_[1];
    const std::vector<QmfValue>& x3 = x_in_[2];
    const std::vector<QmfValue>& x4 = x_in_[3];
    if (full) {
        // Pseudocode 8: (w1in, w2in) of Pseudocode 9, then D0, D2 and D1 on
        // x0in, w1in and x3in for the left module and on x1in, w2in and x4in
        // for the right.
        for (auto& w : w_in_) {
            w.resize(n);
        }
        pre_.process(values.core_mode, num_ts, x0, x3, x1, x4, w_in_[0], w_in_[1]);
        decorrelate(0, x0, y_[0], num_ts);
        decorrelate(1, w_in_[0], y_[1], num_ts);
        decorrelate(2, x3, y_[2], num_ts);
        module(decoding, 0, values, num_ts, {&x0, &x3}, {&y_[0], &y_[1], &y_[2]}, z[0]);
        decorrelate(3, x1, y_[0], num_ts);
        decorrelate(4, w_in_[1], y_[1], num_ts);
        decorrelate(5, x4, y_[2], num_ts);
        module(decoding, 1, values, num_ts, {&x1, &x4}, {&y_[0], &y_[1], &y_[2]}, z[1]);
        // z5 to z12, every output but L, R and C, times the square root of 2.
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t o = 1; o < ajcc::kModule2Outputs; ++o) {
                for (QmfValue& v : *z[side][o]) {
                    v *= kSqrt2;
                }
            }
        }
    } else {
        // Pseudocode 12: D0 and D2 on x0in and x3in for the left module, and
        // on x1in and x4in for the right.
        decorrelate(0, x0, y_[0], num_ts);
        decorrelate(1, x3, y_[1], num_ts);
        module(decoding, 0, values, num_ts, {&x0, &x3}, {&y_[0], &y_[1], nullptr}, z[0]);
        decorrelate(3, x1, y_[0], num_ts);
        decorrelate(4, x4, y_[1], num_ts);
        module(decoding, 1, values, num_ts, {&x1, &x4}, {&y_[0], &y_[1], nullptr}, z[1]);
    }
    // z2 = x2in.
    for (std::size_t k = 0; k < n; ++k) {
        (*centre)[k] *= kInputGain;
    }
}

}  // namespace ac4::detail
