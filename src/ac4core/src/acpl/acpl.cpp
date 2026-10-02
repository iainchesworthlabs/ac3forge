#include "iclforge/ac4core/acpl/acpl.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace iclforge::ac4::detail::acpl {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// The doubles of (ts + 1) and its kin, as a table: Pseudocode 109 converts each from an integer at
// every subband, which is a call into soft float on a chip without double hardware. Past the table,
// the conversion.
constexpr std::array<double, kMaxSlots + 1> kRamp = [] {
    std::array<double, kMaxSlots + 1> table{};
    for (std::size_t k = 0; k < table.size(); ++k) {
        table[k] = static_cast<double>(k);
    }
    return table;
}();

[[nodiscard]] double ramp(int k) noexcept {
    return at(k) < kRamp.size() ? kRamp[at(k)] : static_cast<double>(k);
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

template <std::size_t N>
[[nodiscard]] constexpr std::array<std::int32_t, N> to_q30(const std::array<double, N>& values) {
    std::array<std::int32_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        const double scaled = values[i] * 1073741824.0;
        out[i] = static_cast<std::int32_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
    }
    return out;
}

template <std::size_t N>
[[nodiscard]] constexpr std::array<std::array<std::int32_t, N>, 3> to_q30(
    const std::array<std::array<double, N>, 3>& values) {
    return {to_q30(values[0]), to_q30(values[1]), to_q30(values[2])};
}

constexpr auto kK0Q30 = to_q30(kK0);
constexpr auto kK1Q30 = to_q30(kK1);
constexpr auto kK2Q30 = to_q30(kK2);

// Pseudocode 112's constants.
constexpr double kAlpha = 0.76592833836465;
constexpr double kAlphaSmooth = 0.25;
constexpr double kGamma = 1.5;
constexpr double kEpsilon = 1.0e-9;

// The parameter band of `subband` in Table 197's column `column` (0 for 15 bands,
// then 12, 9 and 7): the last row that starts at or before it.
[[nodiscard]] constexpr int param_band_of(std::size_t column, int subband) noexcept {
    int param_band = 0;
    for (const SbToPbRow& row : kSbToPb) {
        if (row.first_subband > subband) {
            break;
        }
        param_band = row.param_band[column];
    }
    return param_band;
}

// The ducker's parameter bands are the 15 of acpl_max_num_param_bands, by
// subband: a table in the program's read-only data, not a function-local static
// (planning/ac4.md, D14a's memory rules: nothing guarded, nothing lazy).
[[nodiscard]] constexpr std::array<int, kSubbands> ducker_bands() noexcept {
    std::array<int, kSubbands> out{};
    for (std::size_t sb = 0; sb < out.size(); ++sb) {
        out[sb] = param_band_of(0, static_cast<int>(sb));
    }
    return out;
}

constexpr std::array<int, kSubbands> kDuckerBand = ducker_bands();

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
    return param_band_of(column, subband);
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

Interpolator::Divisor::Divisor(int divisor) noexcept
    : n(static_cast<double>(divisor)),
      by_multiply(divisor > 0 && std::has_single_bit(static_cast<unsigned>(divisor))) {
    if (by_multiply) {
        reciprocal = 1.0 / n;
    }
}

Interpolator::Interpolator(const Framing& framing, int num_ts) noexcept
    : steep_(framing.steep),
      two_(framing.num_param_sets == 2),
      half_(num_ts / 2),
      slot_(framing.param_timeslot),
      whole_(num_ts),
      first_half_(num_ts / 2),
      second_half_(num_ts - num_ts / 2) {}

double Interpolator::at(const Column& column, int ts) const noexcept {
    if (!steep_) {
        if (!two_) {
            return column.prev + whole_(ramp(ts + 1) * column.rise);
        }
        if (ts < half_) {
            return column.prev + first_half_(ramp(ts + 1) * column.rise);
        }
        return column.first + second_half_(ramp(ts - half_ + 1) * column.step);
    }
    if (ts < slot_[0]) {
        return column.prev;
    }
    return !two_ || ts < slot_[1] ? column.first : column.second;
}

void interpolate(const Framing& framing, int num_param_bands, const ParamSets& values, const ParamPrev& prev,
                 int num_ts, std::span<double> out) noexcept {
    if (num_ts <= 0 || out.size() < at(num_ts) * kSubbands) {
        return;
    }
    const Interpolator interpolator(framing, num_ts);
    int run_band = -1;
    std::uint64_t run_prev = 0;
    for (int sb = 0; sb < kSubbands; ++sb) {
        const int pb = std::max(sb_to_pb(num_param_bands, sb), 0);
        const double p = prev[at(sb)];
        const auto p_bits = std::bit_cast<std::uint64_t>(p);
        if (sb > 0 && pb == run_band && p_bits == run_prev) {
            for (int ts = 0; ts < num_ts; ++ts) {
                const std::size_t i = at(ts) * kSubbands + at(sb);
                out[i] = out[i - 1];
            }
            continue;
        }
        run_band = pb;
        run_prev = p_bits;
        const Interpolator::Column column =
            Interpolator::column(p, values[0][at(pb)], values[1][at(pb)]);
        for (int ts = 0; ts < num_ts; ++ts) {
            out[at(ts) * kSubbands + at(sb)] = interpolator.at(column, ts);
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

std::span<const std::int32_t> coefficients_q30(int decorrelator, int region) noexcept {
    const std::size_t d = at(std::clamp(decorrelator, 0, kDecorrelators - 1));
    switch (region) {
        case 0:
            return kK0Q30[d];
        case 1:
            return kK1Q30[d];
        default:
            return kK2Q30[d];
    }
}

template <typename Real>
Decorrelator<Real>::Decorrelator(int index) noexcept
    : index_(std::clamp(index, 0, kDecorrelators - 1)) {
    for (std::size_t region = 0; region < coefficients_.size(); ++region) {
        const std::span<const double> a = coefficients(index_, static_cast<int>(region));
        for (std::size_t i = 0; i < a.size(); ++i) {
            coefficients_[region][i] = static_cast<Real>(a[i]);
        }
    }
}

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
        const std::array<Real, 8>& a = coefficients_[at(region)];
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
        if constexpr (dsp::kFixed<Real>) {
            // a[0] is 1 in every table, so there is nothing to divide by.
            const std::span<const std::int32_t> aq = coefficients_q30(index_, region);
            for (std::size_t ts = 0; ts < n; ++ts) {
                const auto tap = [&](std::int32_t coefficient, Complex v, std::uint64_t& re, std::uint64_t& im) {
                    re += static_cast<std::uint64_t>(static_cast<std::int64_t>(v.re.raw) * coefficient);
                    im += static_cast<std::uint64_t>(static_cast<std::int64_t>(v.im.raw) * coefficient);
                };
                std::uint64_t re = 0;
                std::uint64_t im = 0;
                tap(aq[length], x[kIn + ts - delay], re, im);
                for (std::size_t i = 1; i <= length; ++i) {
                    tap(aq[length - i], x[kIn + ts - i - delay], re, im);
                    tap(-aq[i], y[kOut + ts - i], re, im);
                }
                const auto round = [](std::uint64_t sum) {
                    return Real::from_raw(static_cast<std::int32_t>(
                        (static_cast<std::int64_t>(sum) + (std::int64_t{1} << 29U)) >> 30U));
                };
                y[kOut + ts] = Complex{round(re), round(im)};
                out[ts * kSubbands + s] = y[kOut + ts];
            }
        } else {
            for (std::size_t ts = 0; ts < n; ++ts) {
                // b[i] = a[length - i]; a[0] is 1 in every table, kept as printed.
                Complex acc = a[length] * x[kIn + ts - delay];
                for (std::size_t i = 1; i <= length; ++i) {
                    acc += a[length - i] * x[kIn + ts - i - delay] - a[i] * y[kOut + ts - i];
                }
                y[kOut + ts] = acc / a[0];
                out[ts * kSubbands + s] = y[kOut + ts];
            }
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
    peak_decay_.fill(Energy{});
    smooth_.fill(Energy{});
    smooth_peak_diff_.fill(Energy{});
}

template <typename Real>
void TransientDucker<Real>::process(std::span<Complex> inout, int num_ts) noexcept {
    const std::size_t n = at(num_ts);
    if (num_ts <= 0 || inout.size() < n * kSubbands) {
        return;
    }
    const std::array<int, kSubbands>& kBand = kDuckerBand;
    const auto alpha = static_cast<Energy>(kAlpha);
    const auto smoothing = static_cast<Energy>(kAlphaSmooth);
    const auto gamma = static_cast<Energy>(kGamma);
    // An energy of the double decoder's QMF domain, in the scalar's (dsp/scalar_traits.hpp).
    const auto epsilon = dsp::qmf_energy<Real>(static_cast<Energy>(kEpsilon));
    for (std::size_t ts = 0; ts < n; ++ts) {
        // Pseudocode 113, then 112, then 114, for this slot.
        std::array<Energy, kMaxParamBands> energy{};
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            energy[at(kBand[sb])] += dsp::energy_of(inout[ts * kSubbands + sb]);
        }
        std::array<Energy, kMaxParamBands> gain{};
        for (std::size_t pb = 0; pb < at(kMaxParamBands); ++pb) {
            peak_decay_[pb] = alpha * peak_decay_[pb] < energy[pb] ? energy[pb] : alpha * peak_decay_[pb];
            smooth_[pb] = (Energy{1} - smoothing) * smooth_[pb] + smoothing * energy[pb];
            smooth_peak_diff_[pb] =
                (Energy{1} - smoothing) * smooth_peak_diff_[pb] + smoothing * (peak_decay_[pb] - energy[pb]);
            gain[pb] = gamma * smooth_peak_diff_[pb] > smooth_[pb]
                           ? smooth_[pb] / (gamma * (smooth_peak_diff_[pb] + epsilon))
                           : Energy{1};
        }
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            inout[ts * kSubbands + sb] = dsp::apply_gain<Real>(gain[at(kBand[sb])], inout[ts * kSubbands + sb]);
        }
    }
}

template class Decorrelator<Real>;
template class TransientDucker<Real>;
// ajoc::Reconstruction<double> (ajoc/ajoc.cpp, itself needed at double for
// the A-JOC encoder and ac4core's own tests) holds these at its own Real,
// double in that instantiation; ac4core's tests
// (tests/ac4core/test_ac4core_acpl.cpp, test_ac4core_ajoc.cpp) also exercise
// both directly (see this target's CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE).
AC4CORE_ALSO_AT_DOUBLE(template class Decorrelator<double>; template class TransientDucker<double>;)

}  // namespace iclforge::ac4::detail::acpl
