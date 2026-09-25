#include "acpl/acpl.hpp"

#include <algorithm>
#include <cstddef>

namespace ac4::detail::acpl {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Table 197, one row per QMF band group: the group's first subband and its
// parameter band for 15, 12, 9 and 7 parameter bands.
struct SbToPbRow {
    int first_subband = 0;
    std::array<std::uint8_t, 4> param_band{};
};

constexpr std::array<SbToPbRow, 15> kSbToPb = {{
    {0, {{0, 0, 0, 0}}},
    {1, {{1, 1, 1, 1}}},
    {2, {{2, 2, 2, 2}}},
    {3, {{3, 3, 3, 2}}},
    {4, {{4, 4, 3, 3}}},
    {5, {{5, 4, 4, 3}}},
    {6, {{6, 5, 4, 3}}},
    {7, {{7, 5, 5, 3}}},
    {8, {{8, 6, 5, 4}}},
    {9, {{9, 6, 6, 4}}},     // 9 - 10
    {11, {{10, 7, 6, 4}}},   // 11 - 13
    {14, {{11, 8, 7, 5}}},   // 14 - 17
    {18, {{12, 9, 7, 5}}},   // 18 - 22
    {23, {{13, 10, 8, 6}}},  // 23 - 34
    {35, {{14, 11, 8, 6}}},  // 35 - 63
}};

// Table 203: alpha_dq and ibeta by alpha_q, fine.
constexpr std::array<double, 33> kAlphaFine = {
    -2.000000, -1.809375, -1.637500, -1.484375, -1.350000, -1.234375, -1.137500, -1.059375,
    -1.000000, -0.940625, -0.862500, -0.765625, -0.650000, -0.515625, -0.362500, -0.190625,
    0.000000,  0.190625,  0.362500,  0.515625,  0.650000,  0.765625,  0.862500,  0.940625,
    1.000000,  1.059375,  1.137500,  1.234375,  1.350000,  1.484375,  1.637500,  1.809375,
    2.000000,
};
constexpr std::array<std::uint8_t, 33> kIbetaFine = {0, 1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                                     1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0};

// Table 205: alpha_dq and ibeta by alpha_q, coarse.
constexpr std::array<double, 17> kAlphaCoarse = {
    -2.000000, -1.637500, -1.350000, -1.137500, -1.000000, -0.862500, -0.650000, -0.362500, 0.000000,
    0.362500,  0.650000,  0.862500,  1.000000,  1.137500,  1.350000,  1.637500,  2.000000,
};
constexpr std::array<std::uint8_t, 17> kIbetaCoarse = {0, 1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3, 4, 3, 2, 1, 0};

// Table 204: beta_dq[beta_q][ibeta], fine.
constexpr std::array<std::array<double, 9>, 9> kBetaFine = {{
    {{0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000}},
    {{0.2375000, 0.2035449, 0.1729297, 0.1456543, 0.1217188, 0.1011230, 0.0838672, 0.0699512, 0.0593750}},
    {{0.5500000, 0.4713672, 0.4004688, 0.3373047, 0.2818750, 0.2341797, 0.1942188, 0.1619922, 0.1375000}},
    {{0.9375000, 0.8034668, 0.6826172, 0.5749512, 0.4804688, 0.3991699, 0.3310547, 0.2761230, 0.2343750}},
    {{1.4000000, 1.1998440, 1.0193750, 0.8585938, 0.7175000, 0.5960938, 0.4943750, 0.4123438, 0.3500000}},
    {{1.9375000, 1.6604980, 1.4107420, 1.1882319, 0.9929688, 0.8249512, 0.6841797, 0.5706543, 0.4843750}},
    {{2.5500000, 2.1854300, 1.8567190, 1.5638670, 1.3068750, 1.0857420, 0.9004688, 0.7510547, 0.6375000}},
    {{3.2375000, 2.7746389, 2.3573050, 1.9854980, 1.6592190, 1.3784670, 1.1432420, 0.9535449, 0.8093750}},
    {{4.0000000, 3.4281249, 2.9124999, 2.4531250, 2.0500000, 1.7031250, 1.4125000, 1.1781250, 1.0000000}},
}};

// Table 206: beta_dq[beta_q][ibeta], coarse.
constexpr std::array<std::array<double, 5>, 5> kBetaCoarse = {{
    {{0.0000000, 0.0000000, 0.0000000, 0.0000000, 0.0000000}},
    {{0.5500000, 0.4004688, 0.2818750, 0.1942188, 0.1375000}},
    {{1.4000000, 1.0193750, 0.7175000, 0.4943750, 0.3500000}},
    {{2.5500000, 1.8567190, 1.3068750, 0.9004688, 0.6375000}},
    {{4.0000000, 2.9124999, 2.0500000, 1.4125000, 1.0000000}},
}};

// Tables 199 to 201: a[i] for i = 0 to the region's filter length, by
// decorrelator.
constexpr std::array<std::array<double, 8>, 3> kK0 = {{
    {{1.0000, 0.5306, -0.4533, -0.6248, 0.0424, 0.4237, 0.4311, 0.1688}},
    {{1.0000, -0.4178, 0.1082, -0.2368, -0.1014, -0.1052, -0.3528, 0.4665}},
    {{1.0000, 0.4007, 0.4747, 0.2611, -0.1211, -0.4248, -0.2989, -0.1932}},
}};
constexpr std::array<std::array<double, 5>, 3> kK1 = {{
    {{1.0000, 0.5561, -0.3039, -0.5024, -0.1850}},
    {{1.0000, 0.0425, 0.3235, -0.1556, 0.4958}},
    {{1.0000, -0.4361, 0.0345, 0.5215, -0.4178}},
}};
constexpr std::array<std::array<double, 3>, 3> kK2 = {{
    {{1.0000, 0.5773, 0.3321}},
    {{1.0000, 0.2327, -0.3901}},
    {{1.0000, -0.6057, 0.3804}},
}};

// Pseudocode 112's constants.
constexpr double kAlpha = 0.76592833836465;
constexpr double kAlphaSmooth = 0.25;
constexpr double kGamma = 1.5;
constexpr double kEpsilon = 1.0e-9;

// The ducker's parameter bands are the 15 of acpl_max_num_param_bands.
[[nodiscard]] std::array<int, kSubbands> ducker_bands() noexcept {
    std::array<int, kSubbands> out{};
    for (int sb = 0; sb < kSubbands; ++sb) {
        out[at(sb)] = sb_to_pb(kMaxParamBands, sb);
    }
    return out;
}

}  // namespace

int sb_to_pb(int num_param_bands, int subband) noexcept {
    std::size_t column = 0;
    switch (num_param_bands) {
        case 15:
            column = 0;
            break;
        case 12:
            column = 1;
            break;
        case 9:
            column = 2;
            break;
        case 7:
            column = 3;
            break;
        default:
            return -1;
    }
    if (subband < 0 || subband >= kSubbands) {
        return -1;
    }
    int param_band = 0;
    for (const SbToPbRow& row : kSbToPb) {
        if (row.first_subband > subband) {
            break;
        }
        param_band = row.param_band[column];
    }
    return param_band;
}

Range quantised_range(Kind kind, Quant quant) noexcept {
    const bool fine = quant == Quant::kFine;
    switch (kind) {
        case Kind::kAlpha:
            return {0, fine ? 32 : 16};
        case Kind::kBeta:
            return {0, fine ? 8 : 4};
        case Kind::kBeta3:
            return {0, fine ? 16 : 8};
        case Kind::kGamma:
            return fine ? Range{-20, 20} : Range{-10, 10};
    }
    return {};
}

bool differential_decode(Kind kind, Quant quant, bool diff_time, int start_band, int num_bands,
                         std::span<const int, kMaxParamBands> coded, std::span<const int, kMaxParamBands> previous,
                         std::span<int, kMaxParamBands> out) noexcept {
    if (start_band < 0 || num_bands > kMaxParamBands || start_band >= num_bands) {
        return false;
    }
    const Range range = quantised_range(kind, quant);
    std::ranges::fill(out, 0);
    for (int i = start_band; i < num_bands; ++i) {
        int value = coded[at(i)];
        if (diff_time) {
            value += previous[at(i)];
        } else if (i > start_band) {
            value += out[at(i - 1)];
        }
        if (value < range.min || value > range.max) {
            return false;
        }
        out[at(i)] = value;
    }
    return true;
}

AlphaValue dequantise_alpha(int q, Quant quant) noexcept {
    if (quant == Quant::kFine) {
        const std::size_t index = at(std::clamp(q, 0, 32));
        return {kAlphaFine[index], kIbetaFine[index]};
    }
    const std::size_t index = at(std::clamp(q, 0, 16));
    return {kAlphaCoarse[index], kIbetaCoarse[index]};
}

double dequantise_beta(int q, int ibeta, Quant quant) noexcept {
    if (quant == Quant::kFine) {
        return kBetaFine[at(std::clamp(q, 0, 8))][at(std::clamp(ibeta, 0, 8))];
    }
    return kBetaCoarse[at(std::clamp(q, 0, 4))][at(std::clamp(ibeta, 0, 4))];
}

double beta3_step(Quant quant) noexcept {
    return quant == Quant::kFine ? 0.125 : 0.25;
}

double gamma_step(Quant quant) noexcept {
    return quant == Quant::kFine ? 1638.0 / 16384.0 : 3276.0 / 16384.0;
}

void interpolate(const Framing& framing, int num_param_bands, const ParamSets& values, const ParamPrev& prev,
                 int num_ts, std::span<double> out) noexcept {
    if (num_ts <= 0 || out.size() < at(num_ts) * kSubbands) {
        return;
    }
    const bool two = framing.num_param_sets == 2;
    const int ts_2 = num_ts / 2;
    for (int sb = 0; sb < kSubbands; ++sb) {
        const int pb = std::max(sb_to_pb(num_param_bands, sb), 0);
        const double p = prev[at(sb)];
        const double v0 = values[0][at(pb)];
        const double v1 = values[1][at(pb)];
        for (int ts = 0; ts < num_ts; ++ts) {
            double value = 0.0;
            if (!framing.steep) {
                if (!two) {
                    value = p + (ts + 1) * (v0 - p) / num_ts;
                } else if (ts < ts_2) {
                    value = p + (ts + 1) * (v0 - p) / ts_2;
                } else {
                    value = v0 + (ts - ts_2 + 1) * (v1 - v0) / (num_ts - ts_2);
                }
            } else if (ts < framing.param_timeslot[0]) {
                value = p;
            } else if (!two || ts < framing.param_timeslot[1]) {
                value = v0;
            } else {
                value = v1;
            }
            out[at(ts) * kSubbands + at(sb)] = value;
        }
    }
}

void end_frame(const Framing& framing, int num_param_bands, const ParamSets& values, ParamPrev& prev) noexcept {
    const std::size_t last = framing.num_param_sets == 2 ? 1 : 0;
    for (int sb = 0; sb < kSubbands; ++sb) {
        const int pb = std::max(sb_to_pb(num_param_bands, sb), 0);
        prev[at(sb)] = values[last][at(pb)];
    }
}

int region_of(int subband) noexcept {
    if (subband >= kRegions[2].first_subband) {
        return 2;
    }
    return subband >= kRegions[1].first_subband ? 1 : 0;
}

std::span<const double> coefficients(int decorrelator, int region) noexcept {
    const std::size_t d = at(std::clamp(decorrelator, 0, kDecorrelators - 1));
    switch (region) {
        case 0:
            return kK0[d];
        case 1:
            return kK1[d];
        default:
            return kK2[d];
    }
}

template <typename Real>
Decorrelator<Real>::Decorrelator(int index) noexcept : index_(std::clamp(index, 0, kDecorrelators - 1)) {}

template <typename Real>
void Decorrelator<Real>::reset() noexcept {
    x_history_.fill(Complex{});
    y_history_.fill(Complex{});
}

template <typename Real>
void Decorrelator<Real>::process(std::span<const Complex> in, std::span<Complex> out, int num_ts) noexcept {
    const std::size_t n = at(num_ts);
    if (num_ts <= 0 || num_ts > kMaxSlots || in.size() < n * kSubbands || out.size() < n * kSubbands) {
        return;
    }
    // Per subband: x[ts - k] for k up to kInputHistory, and y[ts - i] for i
    // up to kOutputHistory, from the history before slot 0 (Pseudocode 111's
    // NOTE: negative indices reach the frames before).
    constexpr auto kIn = static_cast<std::size_t>(kInputHistory);
    constexpr auto kOut = static_cast<std::size_t>(kOutputHistory);
    std::array<Complex, kIn + static_cast<std::size_t>(kMaxSlots)> x{};
    std::array<Complex, kOut + static_cast<std::size_t>(kMaxSlots)> y{};
    for (int sb = 0; sb < kSubbands; ++sb) {
        const auto s = at(sb);
        const int region = region_of(sb);
        const std::span<const double> a = coefficients(index_, region);
        const auto delay = at(kRegions[at(region)].delay);
        const auto length = at(kRegions[at(region)].length);
        for (std::size_t k = 0; k < kIn; ++k) {
            x[k] = x_history_[k * kSubbands + s];
        }
        for (std::size_t k = 0; k < kOut; ++k) {
            y[k] = y_history_[k * kSubbands + s];
        }
        for (std::size_t ts = 0; ts < n; ++ts) {
            x[kIn + ts] = in[ts * kSubbands + s];
        }
        for (std::size_t ts = 0; ts < n; ++ts) {
            // b[i] = a[length - i]; a[0] is 1 in every table, kept as printed.
            Complex acc = static_cast<Real>(a[length]) * x[kIn + ts - delay];
            for (std::size_t i = 1; i <= length; ++i) {
                acc += static_cast<Real>(a[length - i]) * x[kIn + ts - i - delay] -
                       static_cast<Real>(a[i]) * y[kOut + ts - i];
            }
            y[kOut + ts] = acc / static_cast<Real>(a[0]);
            out[ts * kSubbands + s] = y[kOut + ts];
        }
        for (std::size_t k = 0; k < kIn; ++k) {
            x_history_[k * kSubbands + s] = x[n + k];
        }
        for (std::size_t k = 0; k < kOut; ++k) {
            y_history_[k * kSubbands + s] = y[n + k];
        }
    }
}

template <typename Real>
void TransientDucker<Real>::reset() noexcept {
    peak_decay_.fill(Real{});
    smooth_.fill(Real{});
    smooth_peak_diff_.fill(Real{});
}

template <typename Real>
void TransientDucker<Real>::process(std::span<Complex> inout, int num_ts) noexcept {
    const std::size_t n = at(num_ts);
    if (num_ts <= 0 || inout.size() < n * kSubbands) {
        return;
    }
    static const std::array<int, kSubbands> kBand = ducker_bands();
    const auto alpha = static_cast<Real>(kAlpha);
    const auto smoothing = static_cast<Real>(kAlphaSmooth);
    const auto gamma = static_cast<Real>(kGamma);
    const auto epsilon = static_cast<Real>(kEpsilon);
    for (std::size_t ts = 0; ts < n; ++ts) {
        // Pseudocode 113, then 112, then 114, for this slot.
        std::array<Real, kMaxParamBands> energy{};
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            energy[at(kBand[sb])] += std::norm(inout[ts * kSubbands + sb]);
        }
        std::array<Real, kMaxParamBands> gain{};
        for (std::size_t pb = 0; pb < at(kMaxParamBands); ++pb) {
            peak_decay_[pb] = alpha * peak_decay_[pb] < energy[pb] ? energy[pb] : alpha * peak_decay_[pb];
            smooth_[pb] = (Real{1} - smoothing) * smooth_[pb] + smoothing * energy[pb];
            smooth_peak_diff_[pb] =
                (Real{1} - smoothing) * smooth_peak_diff_[pb] + smoothing * (peak_decay_[pb] - energy[pb]);
            gain[pb] = gamma * smooth_peak_diff_[pb] > smooth_[pb]
                           ? smooth_[pb] / (gamma * (smooth_peak_diff_[pb] + epsilon))
                           : Real{1};
        }
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            inout[ts * kSubbands + sb] *= gain[at(kBand[sb])];
        }
    }
}

template class Decorrelator<double>;
template class TransientDucker<double>;

}  // namespace ac4::detail::acpl
