#include "reference_transform.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

#include "ac3/core/window.hpp"  // kTransformLength

// The variant of src/core/reference_transform.hpp that CARRIES the direct-form
// tables - what every build except the minimum-footprint decoder profile
// compiles (roadmap PF7; see the header for the seam and the byte counts, and
// src/forge/CMakeLists.txt for the selection).
//
// Nothing here is new code: the tables and the two evaluation loops moved
// verbatim out of src/core/mdct.cpp so they could be left out of a build.
// Their own reasoning - why a full matrix rather than a period-reduced one,
// and what precomputing bought - stayed with them.

namespace ac3::internal {

namespace {

constexpr int kN = kTransformLength;  // 512
constexpr double kPi = std::numbers::pi;

// §7.9.4.2 step 3's N/4-point complex "IFFT" over the kQuarter x kQuarter
// grid. This depends only on (k, n), never on the coefficients being
// transformed, so it is the same fixed matrix on every call and is computed
// once instead of (kQuarter^2 =) 16,384 fresh cos+sin pairs per transform.
//
// Deliberately a full matrix, not a period-128 table indexed by (k*n) % 128
// (the trick fft.cpp's dft512 uses for its own twiddles):
// std::cos(8*pi*k*n/N) reaches an UN-reduced angle of ~792 radians at
// k=n=127, and empirically std::cos of that large angle differs from
// std::cos of the small angle it is congruent to mod 2*pi by ~1.3e-13 -
// nowhere near bit-identical, just close enough to hide under mdct.cpp's
// existing 1e-10 golden tolerances. Storing the exact expression this loop
// already evaluated, just once instead of every call, has no such gap.
struct InnerSumTable {
    static constexpr int kDim = kN / 4;  // 128
    std::array<std::array<double, kDim>, kDim> cos{};
    std::array<std::array<double, kDim>, kDim> sin{};
    InnerSumTable() {
        for (int n = 0; n < kDim; ++n) {
            for (int k = 0; k < kDim; ++k) {
                const double angle = 8.0 * kPi * k * n / kN;
                cos[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::cos(angle);
                sin[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::sin(angle);
            }
        }
    }
};

const InnerSumTable& inner_sum_table() {
    static const InnerSumTable t;
    return t;
}

// The same idea, for the two independent N/8-point "IFFT" sums
// (angle = 16*pi*k*n/N over the kEighth x kEighth grid).
struct InnerSumPairTable {
    static constexpr int kDim = kN / 8;  // 64
    std::array<std::array<double, kDim>, kDim> cos{};
    std::array<std::array<double, kDim>, kDim> sin{};
    InnerSumPairTable() {
        for (int n = 0; n < kDim; ++n) {
            for (int k = 0; k < kDim; ++k) {
                const double angle = 16.0 * kPi * k * n / kN;
                cos[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::cos(angle);
                sin[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = std::sin(angle);
            }
        }
    }
};

const InnerSumPairTable& inner_sum_pair_table() {
    static const InnerSumPairTable t;
    return t;
}

// §8.2.3.2 direct form, generalized over the transform length and alpha:
// alpha = 0/N=512 is the long transform; alpha = -1/+1 at N=256 are the two
// halves of a block-switched block.
//
// cos(phase) depends only on (k, n, alpha), never on the windowed signal
// itself, and alpha only ever takes the three values above - so it is the
// same fixed N_len x (N_len/2) matrix on every call. This used to compute
// std::cos(phase) fresh inside the loop below, exactly like the inverse
// transform's own step 3 does NOT. Measured with Tracy
// (docs/platforms/android.md's performance investigation): that
// recomputation was ~79% of the ENTIRE encoder's per-frame cost - 131,072
// std::cos() calls per 512-point transform, 36 transforms a frame
// (6 channels x 6 blocks). Precomputing the matrix once, the same way the
// inverse transform already does, produces bit-identical coefficients (same
// phase formula, same std::cos(), same accumulation order - only WHEN it
// runs changes) while removing that cost from the hot path entirely.
template <int NLen>
struct ForwardCosTable {
    static constexpr int kHalf = NLen / 2;
    std::array<std::array<double, static_cast<std::size_t>(NLen)>, static_cast<std::size_t>(kHalf)>
        value{};
    explicit ForwardCosTable(double alpha) {
        for (int k = 0; k < kHalf; ++k) {
            const double factor = 2.0 * k + 1.0;
            for (int n = 0; n < NLen; ++n) {
                const double phase = (2.0 * kPi / (4.0 * NLen)) * (2.0 * n + 1.0) * factor +
                                      (kPi / 4.0) * factor * (1.0 + alpha);
                value[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)] = std::cos(phase);
            }
        }
    }
};

const ForwardCosTable<512>& forward_cos_table_long() {
    static const ForwardCosTable<512> t(0.0);
    return t;
}

const ForwardCosTable<256>& forward_cos_table_first() {
    static const ForwardCosTable<256> t(-1.0);
    return t;
}

const ForwardCosTable<256>& forward_cos_table_second() {
    static const ForwardCosTable<256> t(1.0);
    return t;
}

template <int NLen>
void mdct_forward_core(std::span<const double> windowed, const ForwardCosTable<NLen>& table,
                       std::span<double> coeffs) {
    for (int k = 0; k < NLen / 2; ++k) {
        double sum = 0.0;
        for (int n = 0; n < NLen; ++n) {
            sum += windowed[static_cast<std::size_t>(n)] *
                   table.value[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)];
        }
        coeffs[static_cast<std::size_t>(k)] = (-2.0 / NLen) * sum;
    }
}

// The two inner sums share this shape; P is the grid dimension and `table`
// the matching matrix. Templated on the table type rather than on P alone so
// the 128- and 64-point forms each keep their own compile-time bounds.
template <std::size_t P, class Table>
void inner_sum_core(const Table& table, std::span<const double, P> z_re,
                    std::span<const double, P> z_im, std::span<double, P> t_re,
                    std::span<double, P> t_im) {
    for (std::size_t n = 0; n < P; ++n) {
        double re = 0.0;
        double im = 0.0;
        const auto& row_c = table.cos[n];
        const auto& row_s = table.sin[n];
        for (std::size_t k = 0; k < P; ++k) {
            const double c = row_c[k];
            const double s = row_s[k];
            re += z_re[k] * c - z_im[k] * s;
            im += z_re[k] * s + z_im[k] * c;
        }
        t_re[n] = re;
        t_im[n] = im;
    }
}

}  // namespace

void reference_mdct512_forward(std::span<const double, 512> windowed,
                               std::span<double, 256> coeffs) {
    mdct_forward_core<512>(windowed, forward_cos_table_long(), coeffs);
}

void reference_mdct256_forward_first(std::span<const double, 256> windowed,
                                     std::span<double, 128> coeffs) {
    mdct_forward_core<256>(windowed, forward_cos_table_first(), coeffs);
}

void reference_mdct256_forward_second(std::span<const double, 256> windowed,
                                      std::span<double, 128> coeffs) {
    mdct_forward_core<256>(windowed, forward_cos_table_second(), coeffs);
}

void reference_inner_sum_128(std::span<const double, 128> z_re,
                             std::span<const double, 128> z_im, std::span<double, 128> t_re,
                             std::span<double, 128> t_im) {
    inner_sum_core<128>(inner_sum_table(), z_re, z_im, t_re, t_im);
}

void reference_inner_sum_64(std::span<const double, 64> z_re, std::span<const double, 64> z_im,
                            std::span<double, 64> t_re, std::span<double, 64> t_im) {
    inner_sum_core<64>(inner_sum_pair_table(), z_re, z_im, t_re, t_im);
}

}  // namespace ac3::internal
