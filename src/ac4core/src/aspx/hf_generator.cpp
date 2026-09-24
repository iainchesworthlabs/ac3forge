#include "aspx/hf_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ac4::detail::aspx {
namespace {

constexpr std::size_t kSubbands = 64;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Table 195, new_chirp by [aspx_tna_mode_prev][aspx_tna_mode]: None, Light,
// Moderate, Heavy.
constexpr std::array<std::array<double, 4>, 4> kNewChirp = {{
    {0.0, 0.6, 0.9, 0.98},
    {0.6, 0.75, 0.9, 0.98},
    {0.0, 0.75, 0.9, 0.98},
    {0.0, 0.75, 0.9, 0.98},
}};

// The least squares cubic through (i, y[i]) for i < y.size(), evaluated at
// every i. The fit is made against 1, t, t^2 and t^3 with t = i scaled to
// [-1, 1], orthonormalised by modified Gram-Schmidt: the same cubics as
// powers of i, without their conditioning. The values are what
// polynomial_fit()'s coefficients give back (Pseudocode 85).
template <typename Real>
void fit_cubic(std::span<const Real> y, std::span<Real> fitted) {
    const std::size_t n = y.size();
    std::array<std::vector<double>, 4> basis;
    for (std::size_t k = 0; k < basis.size(); ++k) {
        basis[k].resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = n > 1 ? (2.0 * static_cast<double>(i) - static_cast<double>(n - 1)) /
                                         static_cast<double>(n - 1)
                                   : 0.0;
            basis[k][i] = std::pow(t, static_cast<double>(k));
        }
    }
    std::ranges::fill(fitted, Real{});
    for (std::size_t k = 0; k < basis.size(); ++k) {
        for (std::size_t m = 0; m < k; ++m) {
            double dot = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                dot += basis[k][i] * basis[m][i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                basis[k][i] -= dot * basis[m][i];
            }
        }
        double norm = 0.0;
        for (const double v : basis[k]) {
            norm += v * v;
        }
        // Fewer points than coefficients: this power adds nothing new.
        if (norm < 1e-18) {
            std::ranges::fill(basis[k], 0.0);
            continue;
        }
        norm = std::sqrt(norm);
        double projection = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            basis[k][i] /= norm;
            projection += basis[k][i] * static_cast<double>(y[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            fitted[i] += static_cast<Real>(projection * basis[k][i]);
        }
    }
}

}  // namespace

template <typename Real>
void preflattening_gains(std::span<const std::complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Real> gain_vec) {
    const auto n = at(sbx);
    if (ts_end <= ts_begin || n == 0) {
        std::ranges::fill(gain_vec.first(n), Real{1});
        return;
    }
    // Pseudocode 85: each subband's mean energy over the interval in dB, and
    // their mean.
    std::vector<Real> pow_env(n);
    Real mean_energy{};
    for (std::size_t sb = 0; sb < n; ++sb) {
        Real energy{};
        for (int ts = ts_begin; ts < ts_end; ++ts) {
            energy += std::norm(q_low[at(ts) * kSubbands + sb]);
        }
        energy /= static_cast<Real>(ts_end - ts_begin);
        pow_env[sb] = Real{10} * std::log10(energy + Real{1});
        mean_energy += pow_env[sb];
    }
    mean_energy /= static_cast<Real>(n);
    std::vector<Real> slope(n);
    fit_cubic<Real>(pow_env, slope);
    for (std::size_t sb = 0; sb < n; ++sb) {
        gain_vec[sb] = std::pow(Real{10}, (mean_energy - slope[sb]) / Real{20});
    }
}

template <typename Real>
void prediction_coefficients(std::span<const std::complex<Real>> q_low_ext, int num_ts_ext, int sba,
                             std::span<std::complex<Real>> alpha0,
                             std::span<std::complex<Real>> alpha1) {
    using Complex = std::complex<Real>;
    // EPSILON_INV of Pseudocode 87.
    const Real regularise = Real{1} / (Real{1} + std::ldexp(Real{1}, -20));
    for (int sb = 0; sb < sba; ++sb) {
        // Pseudocode 86: cov[i][j] for i < 3 and 0 < j < 3, over every second
        // slot, at lags of two slots.
        std::array<std::array<Complex, 3>, 3> cov{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 1; j < 3; ++j) {
                Complex sum{};
                for (int ts = kTsOffsetHfadj; ts < num_ts_ext; ts += 2) {
                    sum += q_low_ext[at(ts - 2 * i) * kSubbands + at(sb)] *
                           std::conj(q_low_ext[at(ts - 2 * j) * kSubbands + at(sb)]);
                }
                cov[at(i)][at(j)] = sum;
            }
        }
        // Pseudocode 87. alpha0 is -(cov01 + alpha1 conj(cov12)) / cov11, the
        // solution of the normal equations that give alpha1 as printed
        // (src/ac4dec/ERRATA.md, "alpha0's parentheses").
        const Complex denom = cov[2][2] * cov[1][1] - std::norm(cov[1][2]) * regularise;
        Complex a1{};
        if (denom != Complex{}) {
            a1 = (cov[0][1] * cov[1][2] - cov[0][2] * cov[1][1]) / denom;
        }
        Complex a0{};
        if (cov[1][1] != Complex{}) {
            a0 = -(cov[0][1] + a1 * std::conj(cov[1][2])) / cov[1][1];
        }
        if (std::abs(a0) >= Real{4} || std::abs(a1) >= Real{4}) {
            a0 = Complex{};
            a1 = Complex{};
        }
        alpha0[at(sb)] = a0;
        alpha1[at(sb)] = a1;
    }
}

template <typename Real>
void generate_high_band(const SubbandGroups& groups, const PatchTables& patches,
                        const HfGeneratorInput<Real>& in, HfGeneratorState<Real>& state,
                        std::span<std::complex<Real>> q_high) {
    using Complex = std::complex<Real>;
    const int sbx = groups.sbx;
    const int sbz = groups.sbx + groups.num_sb_aspx;
    const int num_ts_ext = in.num_qmf_timeslots + in.ts_offset_hfgen + kTsOffsetHfadj;
    const std::span<const Complex> q_low = in.q_low_ext.subspan(at(kTsOffsetHfadj) * kSubbands);

    std::array<Real, kSubbands> gain_vec{};
    if (in.preflat) {
        preflattening_gains<Real>(q_low, sbx, in.ts_begin, in.ts_end, gain_vec);
    }
    std::array<Complex, kSubbands> alpha0{};
    std::array<Complex, kSubbands> alpha1{};
    prediction_coefficients<Real>(in.q_low_ext, num_ts_ext, groups.sba, alpha0, alpha1);

    // Pseudocode 88.
    std::array<Real, kMaxSbgNoise> chirp{};
    for (int sbg = 0; sbg < groups.num_sbg_noise; ++sbg) {
        const auto mode = static_cast<std::size_t>(in.tna_mode[at(sbg)] & 3U);
        const auto prev = static_cast<std::size_t>(state.tna_mode_prev[at(sbg)] & 3U);
        auto new_chirp = static_cast<Real>(kNewChirp[prev][mode]);
        const Real prev_chirp = state.chirp_prev[at(sbg)];
        if (new_chirp < prev_chirp) {
            new_chirp = Real(0.75) * new_chirp + Real(0.25) * prev_chirp;
        } else {
            new_chirp = Real(0.90625) * new_chirp + Real(0.09375) * prev_chirp;
        }
        chirp[at(sbg)] = new_chirp < Real(0.015625) ? Real{} : new_chirp;
    }
    for (int sbg = 0; sbg < kMaxSbgNoise; ++sbg) {
        const bool used = sbg < groups.num_sbg_noise;
        state.chirp_prev[at(sbg)] = used ? chirp[at(sbg)] : Real{};
        state.tna_mode_prev[at(sbg)] = used ? in.tna_mode[at(sbg)] : std::uint8_t{0};
    }

    // Pseudocode 89. A subband no patch reaches (the last patch dropped for
    // being under three subbands wide) stays at zero.
    for (int ts = in.ts_begin; ts < in.ts_end; ++ts) {
        std::span<Complex> slot = q_high.subspan(at(ts) * kSubbands, kSubbands);
        std::fill(slot.begin() + sbx, slot.begin() + sbz, Complex{});
        int sum_sb_patches = 0;
        int g = 0;
        const int n = ts + kTsOffsetHfadj;
        for (int i = 0; i < patches.num_sbg_patches; ++i) {
            for (int sb = 0; sb < patches.sbg_patch_num_sb[at(i)]; ++sb) {
                const int sb_high = sbx + sum_sb_patches + sb;
                // The noise group sb_high is in; with borders that strictly
                // increase, the pseudocode's "if (sbg_noise[g+1] == sb_high)".
                while (g + 1 < groups.num_sbg_noise && groups.sbg_noise[at(g + 1)] <= sb_high) {
                    ++g;
                }
                const int p = patches.sbg_patch_start_sb[at(i)] + sb;
                const auto source = [&](int slot_index) {
                    return in.q_low_ext[at(slot_index) * kSubbands + at(p)];
                };
                const Real c = chirp[at(g)];
                Complex value = source(n) + c * alpha0[at(p)] * source(n - 2) +
                                c * c * alpha1[at(p)] * source(n - 4);
                if (in.preflat) {
                    value *= Real{1} / gain_vec[at(p)];
                }
                slot[at(sb_high)] = value;
            }
            sum_sb_patches += patches.sbg_patch_num_sb[at(i)];
        }
    }
}

template void generate_high_band<double>(const SubbandGroups&, const PatchTables&,
                                         const HfGeneratorInput<double>&, HfGeneratorState<double>&,
                                         std::span<std::complex<double>>);
template void preflattening_gains<double>(std::span<const std::complex<double>>, int, int, int,
                                          std::span<double>);
template void prediction_coefficients<double>(std::span<const std::complex<double>>, int, int,
                                              std::span<std::complex<double>>,
                                              std::span<std::complex<double>>);

}  // namespace ac4::detail::aspx
