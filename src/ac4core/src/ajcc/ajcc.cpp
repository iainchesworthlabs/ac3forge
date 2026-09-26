#include "ajcc/ajcc.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ac4::detail::ajcc {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

constexpr double kRootHalf = std::numbers::sqrt2 / 2.0;  // 1/sqrt(2)

}  // namespace

int num_param_bands(int num_param_bands_id) noexcept {
    constexpr std::array<int, 4> kBands = {15, 12, 9, 7};
    return kBands[at(std::clamp(num_param_bands_id, 0, 3))];
}

acpl::Range quantised_range(Kind kind, acpl::Quant quant) noexcept {
    const bool fine = quant == acpl::Quant::kFine;
    if (kind == Kind::kDry) {
        return {0, fine ? 22 : 11};
    }
    return {0, fine ? 40 : 20};
}

double dequantise(Kind kind, int q, acpl::Quant quant) noexcept {
    const double delta = quant == acpl::Quant::kFine ? 0.1 : 0.2;
    const double offset = kind == Kind::kDry ? 0.6 : 2.0;
    return static_cast<double>(q) * delta - offset;
}

bool differential_decode(acpl::Range range, bool diff_time, int num_bands,
                         std::span<const int, kMaxParamBands> coded,
                         std::span<const int, kMaxParamBands> previous,
                         std::span<int, kMaxParamBands> out) noexcept {
    if (num_bands <= 0 || num_bands > kMaxParamBands) {
        return false;
    }
    std::ranges::fill(out, 0);
    for (int i = 0; i < num_bands; ++i) {
        int value = coded[at(i)];
        if (diff_time) {
            value += previous[at(i)];
        } else if (i > 0) {
            value += out[at(i - 1)];
        }
        if (value < range.min || value > range.max) {
            return false;
        }
        out[at(i)] = value;
    }
    return true;
}

std::array<double, kModule2Coefficients> module_2(int core_mode, const ModuleParams& p) noexcept {
    // d0 to d9, then w0 to w14, as Pseudocode 11 sets them; unset ones are 0.
    std::array<double, kModule2Coefficients> c{};
    const auto d = [&c](std::size_t k) -> double& { return c[k]; };
    const auto w = [&c](std::size_t k) -> double& { return c[10 + k]; };
    if (core_mode == 0) {
        d(0) = (1.0 + p.alpha) / 2.0;
        d(3) = (1.0 - p.alpha) / 2.0;
        d(6) = p.dry1;
        d(7) = p.dry2;
        d(9) = 1.0 - p.dry1 - p.dry2;
        w(0) = p.beta / 2.0;
        w(3) = -1.0 * p.beta / 2.0;
        w(6) = (p.wet1 + p.wet3) * kRootHalf;
        w(7) = -1.0 * p.wet3 * kRootHalf;
        w(9) = -1.0 * p.wet1 * kRootHalf;
        w(11) = (p.wet3 + p.wet2) * kRootHalf;
        w(12) = -1.0 * p.wet2 * kRootHalf;
        w(14) = -1.0 * p.wet3 * kRootHalf;
    } else {
        d(0) = p.dry1;
        d(1) = p.dry2;
        d(2) = 1.0 - p.dry1 - p.dry2;
        d(8) = (1.0 + p.alpha) / 2.0;
        d(9) = (1.0 - p.alpha) / 2.0;
        w(0) = (p.wet1 + p.wet3) * kRootHalf;
        w(1) = -1.0 * p.wet3 * kRootHalf;
        w(2) = -1.0 * p.wet1 * kRootHalf;
        w(5) = (p.wet3 + p.wet2) * kRootHalf;
        w(6) = -1.0 * p.wet2 * kRootHalf;
        w(7) = -1.0 * p.wet3 * kRootHalf;
        w(13) = p.beta / 2.0;
        w(14) = -1.0 * p.beta / 2.0;
    }
    return c;
}

std::array<double, kModule4Coefficients> module_4(int core_mode, const ModuleParams& p) noexcept {
    // d0 to d5, then w0 to w5, as Pseudocode 14 sets them.
    std::array<double, kModule4Coefficients> c{};
    const auto d = [&c](std::size_t k) -> double& { return c[k]; };
    const auto w = [&c](std::size_t k) -> double& { return c[6 + k]; };
    if (core_mode == 0) {
        const double wet = std::sqrt(0.5 * p.wet1 * p.wet1 + 0.5 * p.wet3 * p.wet3);
        d(0) = (1.0 + p.alpha) / 2.0;
        d(2) = (1.0 - p.alpha) / 2.0;
        d(4) = p.dry1 + p.dry2;
        d(5) = 1.0 - p.dry1 - p.dry2;
        w(0) = p.beta / 2.0;
        w(2) = -p.beta / 2.0;
        w(4) = -wet;
        w(5) = wet;
    } else {
        const double front = p.wet1 + p.wet3;
        const double back = p.wet3 + p.wet2;
        const double wet = std::sqrt(0.5 * front * front + 0.5 * back * back);
        d(0) = p.dry1;
        d(1) = 1.0 - p.dry1;
        d(5) = 1.0;
        w(0) = wet;
        w(1) = -wet;
    }
    return c;
}

Term module_term(std::size_t coefficient, std::size_t outputs) noexcept {
    const std::size_t dry = 2 * outputs;
    if (coefficient < dry) {
        return {
            .output = coefficient % outputs, .decorrelated = false, .input = coefficient / outputs};
    }
    const std::size_t k = coefficient - dry;
    return {.output = k % outputs, .decorrelated = true, .input = k / outputs};
}

template <typename Real>
void accumulate(std::span<const Real> weight, std::span<const std::complex<Real>> in,
                std::span<std::complex<Real>> out, int num_ts) noexcept {
    const std::size_t n = at(std::max(num_ts, 0)) * at(kSubbands);
    if (weight.size() < n || in.size() < n || out.size() < n) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        out[i] += weight[i] * in[i];
    }
}

template <typename Real>
void PreModification<Real>::process(int core_mode, int num_ts, std::span<const Complex> in1,
                                    std::span<const Complex> in2, std::span<const Complex> in3,
                                    std::span<const Complex> in4, std::span<Complex> out1,
                                    std::span<Complex> out2) noexcept {
    const std::size_t n = at(std::max(num_ts, 0)) * at(kSubbands);
    if (in1.size() < n || in2.size() < n || in3.size() < n || in4.size() < n || out1.size() < n ||
        out2.size() < n) {
        return;
    }
    if (!primed_) {
        core_mode_prev_ = core_mode;
        primed_ = true;
    }
    Real g{0};
    Real d{0};
    if (core_mode == core_mode_prev_) {
        if (core_mode == 0) {
            g = Real{1};
        }
    } else {
        const Real step = Real{1} / static_cast<Real>(num_ts);
        if (core_mode == 0) {
            d = step;
        } else {
            g = Real{1};
            d = -step;
        }
        core_mode_prev_ = core_mode;
    }
    for (std::size_t ts = 0; ts < at(num_ts); ++ts) {
        g += d;
        for (std::size_t sb = 0; sb < at(kSubbands); ++sb) {
            const std::size_t i = ts * at(kSubbands) + sb;
            out1[i] = g * in2[i] + (Real{1} - g) * in1[i];
            out2[i] = g * in4[i] + (Real{1} - g) * in3[i];
        }
    }
}

template void accumulate<double>(std::span<const double>, std::span<const std::complex<double>>,
                                 std::span<std::complex<double>>, int) noexcept;
template class PreModification<double>;

}  // namespace ac4::detail::ajcc
