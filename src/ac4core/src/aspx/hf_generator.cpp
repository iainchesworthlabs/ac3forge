#include "iclforge/ac4core/aspx/hf_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "iclforge/arithmetic/scalar_math.hpp"

namespace iclforge::ac4::detail::aspx {
namespace {

// Pseudocode 85's two dB conversions, power-referenced (10, not 20): a power
// ratio's decibels are 10 log10(x), and its inverse is 10^(y / 20). At
// Real = double this calls std::log10/std::pow exactly as the code always
// did (bit-identical: the default build's tests and hashes hold unchanged).
// At Real = float (planning/ac4.md, D14a) it instead takes the equal forms
// (10 / log2 10) log2(x) and 2^(y log2(10) / 20) through iclforge::internal's
// scalar_log2/scalar_exp2, never std::log10f/std::powf directly: two
// platforms' libm can disagree in the last bit on the same float input,
// which would break decision 26's promise of identical output on the host,
// the Cortex-M3 leg, the S3 and the P4 (this PR's report, "hf_generator.cpp's
// dB gains are not yet cross-platform-safe at float" - #1096 flagged this as
// a known gap, closed here before pcm/ retemplating gives it a float caller).
// The two forms are mathematically equal but not bit-identical to each
// other, so the branch is on Real, not just a shared formula.
constexpr double kTenOverLog2Of10 = 3.010299956639812;   // 10 / log2(10), for 10*log10(x)
constexpr double kLog2Of10Over20 = 0.16609640474436812;  // log2(10) / 20, for 10^(y/20)

template <typename Real>
[[nodiscard]] Real power_db(Real x) noexcept {
    if constexpr (std::is_same_v<Real, double>) {
        return Real{10} * std::log10(x);
    } else {
        return static_cast<Real>(kTenOverLog2Of10) * iclforge::internal::scalar_log2(x);
    }
}

template <typename Real>
[[nodiscard]] Real from_power_db(Real db) noexcept {
    if constexpr (std::is_same_v<Real, double>) {
        return std::pow(Real{10}, db / Real{20});
    } else {
        return iclforge::internal::scalar_exp2(static_cast<Real>(kLog2Of10Over20) * db);
    }
}

// Pseudocode 87's limit on a prediction coefficient, |alpha| >= 4. At Real =
// double it takes the complex magnitude, std::hypot, as it always did. At
// Real = float it compares the squared magnitude with 16, which is float
// multiplies and an add: hypotf differs in the last bit between C libraries, and
// a coefficient that one platform finds just under the limit and another at it
// zeroes a subband's prediction on the second only (planning/ac4.md, D14a4).
template <typename Real>
[[nodiscard]] bool reaches_limit(dsp::Complex<Real> alpha) noexcept {
    if constexpr (std::is_same_v<Real, double>) {
        return abs(alpha) >= Real{4};
    } else {
        return norm(alpha) >= Real{16};
    }
}

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
//
// The orthonormal vectors depend on the number of points alone, so they are made once
// for a count (build_cubic_basis: the front half of what this function was, step for
// step, so the doubles are the same) and each frame projects on them (fit_cubic).
void build_cubic_basis(std::size_t n, CubicBasis& cubic) {
    cubic.n = n;
    cubic.empty = {};
    for (std::size_t k = 0; k < cubic.basis.size(); ++k) {
        cubic.basis[k].assign(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = n > 1 ? (2.0 * static_cast<double>(i) - static_cast<double>(n - 1)) /
                                         static_cast<double>(n - 1)
                                   : 0.0;
            cubic.basis[k][i] = std::pow(t, static_cast<double>(k));
        }
    }
    for (std::size_t k = 0; k < cubic.basis.size(); ++k) {
        std::vector<double>& basis_k = cubic.basis[k];
        for (std::size_t m = 0; m < k; ++m) {
            double dot = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                dot += basis_k[i] * cubic.basis[m][i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                basis_k[i] -= dot * cubic.basis[m][i];
            }
        }
        double norm = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            norm += basis_k[i] * basis_k[i];
        }
        // Fewer points than coefficients: this power adds nothing new.
        if (norm < 1e-18) {
            std::fill_n(basis_k.begin(), n, 0.0);
            cubic.empty[k] = true;
            continue;
        }
        norm = std::sqrt(norm);
        for (std::size_t i = 0; i < n; ++i) {
            basis_k[i] /= norm;
        }
    }
}

template <typename Real>
void fit_cubic(std::span<const Real> y, std::span<Real> fitted, const CubicBasis& cubic) {
    const std::size_t n = y.size();
    std::ranges::fill(fitted, Real{});
    for (std::size_t k = 0; k < cubic.basis.size(); ++k) {
        if (cubic.empty[k]) {
            continue;
        }
        const std::vector<double>& basis_k = cubic.basis[k];
        double projection = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            projection += basis_k[i] * static_cast<double>(y[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            fitted[i] += static_cast<Real>(projection * basis_k[i]);
        }
    }
}
}  // namespace

template <typename Real>
void preflattening_gains(std::span<const dsp::Complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Real> gain_vec) {
    CubicBasis cubic;
    preflattening_gains<Real>(q_low, sbx, ts_begin, ts_end, gain_vec, cubic);
}

template <typename Real>
void preflattening_gains(std::span<const dsp::Complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Real> gain_vec, CubicBasis& cubic) {
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
            energy += norm(q_low[at(ts) * kSubbands + sb]);
        }
        energy /= static_cast<Real>(ts_end - ts_begin);
        pow_env[sb] = power_db(energy + Real{1});
        mean_energy += pow_env[sb];
    }
    mean_energy /= static_cast<Real>(n);
    std::vector<Real> slope(n);
    if (cubic.n != n) {
        build_cubic_basis(n, cubic);
    }
    fit_cubic<Real>(pow_env, slope, cubic);
    for (std::size_t sb = 0; sb < n; ++sb) {
        gain_vec[sb] = from_power_db(mean_energy - slope[sb]);
    }
}

template <typename Real>
void prediction_coefficients(std::span<const dsp::Complex<Real>> q_low_ext, int num_ts_ext, int sba,
                             std::span<dsp::Complex<Real>> alpha0,
                             std::span<dsp::Complex<Real>> alpha1) {
    using Complex = dsp::Complex<Real>;
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
                           conj(q_low_ext[at(ts - 2 * j) * kSubbands + at(sb)]);
                }
                cov[at(i)][at(j)] = sum;
            }
        }
        // Pseudocode 87. alpha0 is -(cov01 + alpha1 conj(cov12)) / cov11, the
        // solution of the normal equations that give alpha1 as printed
        // (src/ac4dec/ERRATA.md, "alpha0's parentheses").
        const Complex denom = cov[2][2] * cov[1][1] - norm(cov[1][2]) * regularise;
        Complex a1{};
        if (denom != Complex{}) {
            a1 = (cov[0][1] * cov[1][2] - cov[0][2] * cov[1][1]) / denom;
        }
        Complex a0{};
        if (cov[1][1] != Complex{}) {
            a0 = -(cov[0][1] + a1 * conj(cov[1][2])) / cov[1][1];
        }
        if (reaches_limit(a0) || reaches_limit(a1)) {
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
                        std::span<dsp::Complex<Real>> q_high) {
    using Complex = dsp::Complex<Real>;
    const int sbx = groups.sbx;
    const int sbz = groups.sbx + groups.num_sb_aspx;
    const int num_ts_ext = in.num_qmf_timeslots + in.ts_offset_hfgen + kTsOffsetHfadj;
    const std::span<const Complex> q_low = in.q_low_ext.subspan(at(kTsOffsetHfadj) * kSubbands);

    std::array<Real, kSubbands> gain_vec{};
    if (in.preflat) {
        preflattening_gains<Real>(q_low, sbx, in.ts_begin, in.ts_end, gain_vec, state.cubic);
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
                // Pre-flattening: the gain that flattens the low band's
                // fitted slope, where the text prints its inverse
                // (src/ac4dec/ERRATA.md, "Pre-flattening's direction").
                if (in.preflat) {
                    value *= gain_vec[at(p)];
                }
                slot[at(sb_high)] = value;
            }
            sum_sb_patches += patches.sbg_patch_num_sb[at(i)];
        }
    }
}

template void generate_high_band<Real>(const SubbandGroups&, const PatchTables&,
                                       const HfGeneratorInput<Real>&, HfGeneratorState<Real>&,
                                       std::span<dsp::Complex<Real>>);
template void preflattening_gains<Real>(std::span<const dsp::Complex<Real>>, int, int, int,
                                        std::span<Real>);
template void preflattening_gains<Real>(std::span<const dsp::Complex<Real>>, int, int, int,
                                        std::span<Real>, CubicBasis&);
template void prediction_coefficients<Real>(std::span<const dsp::Complex<Real>>, int, int,
                                            std::span<dsp::Complex<Real>>,
                                            std::span<dsp::Complex<Real>>);
// The A-SPX encoder (src/ac4enc/src/aspx/aspx_encoder.cpp) calls
// generate_high_band at double regardless of the decoder's scalar, to choose
// its interleaving as a decoder will reconstruct it (see this target's
// CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE), and tests/ac4core/test_ac4core_aspx.cpp
// calls the other two at double: a call inlined into generate_high_band<double>
// leaves no symbol for GCC to link a test against, where MSVC emits one anyway.
AC4CORE_ALSO_AT_DOUBLE(
    template void generate_high_band<double>(const SubbandGroups&, const PatchTables&,
                                             const HfGeneratorInput<double>&,
                                             HfGeneratorState<double>&,
                                             std::span<dsp::Complex<double>>);
    template void preflattening_gains<double>(std::span<const dsp::Complex<double>>, int, int, int,
                                              std::span<double>);
    template void preflattening_gains<double>(std::span<const dsp::Complex<double>>, int, int, int,
                                              std::span<double>, CubicBasis&);
    template void prediction_coefficients<double>(std::span<const dsp::Complex<double>>, int, int,
                                                  std::span<dsp::Complex<double>>,
                                                  std::span<dsp::Complex<double>>);)

}  // namespace iclforge::ac4::detail::aspx
