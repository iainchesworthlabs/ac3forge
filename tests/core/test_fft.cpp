#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "ac3/core/fft.hpp"

namespace {

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) < tol; }

// fft.hpp's own statement of what dft512 computes, evaluated literally:
//   Z[k] = (1/N) * sum_n (x_re[n] + j.x_im[n]) * (cos(2pi kn/N) - j.sin(2pi kn/N))
// O(N^2) and far too slow for the codec, but it is the reference this
// primitive's fast structure has to reproduce. The (n*k) % 512 keeps the
// angle inside one period: std::cos of a large un-reduced angle is not
// bit-identical to std::cos of the small angle congruent to it (mdct.cpp's
// InnerSumTable measured ~1.3e-13 at ~792 radians), and n*k reaches 261,121
// here, so an un-reduced reference would be measuring its OWN error.
struct DirectDft {
    std::array<double, 512> re{};
    std::array<double, 512> im{};
};

DirectDft direct_dft512(const std::array<double, 512>& x_re, const std::array<double, 512>& x_im) {
    DirectDft out{};
    for (std::size_t k = 0; k < 512; ++k) {
        double acc_re = 0.0;
        double acc_im = 0.0;
        for (std::size_t n = 0; n < 512; ++n) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>((n * k) % 512) / 512.0;
            const double c = std::cos(angle);
            const double s = -std::sin(angle);
            acc_re += x_re[n] * c - x_im[n] * s;
            acc_im += x_re[n] * s + x_im[n] * c;
        }
        out.re[k] = acc_re / 512.0;
        out.im[k] = acc_im / 512.0;
    }
    return out;
}

// Peak-normalised worst error, the same measure tests/core/test_mdct_fast.cpp
// uses and for the same reason: a per-bin denominator blows up on the
// near-zero bins any real spectrum is mostly made of.
double max_rel_error(const std::array<double, 512>& fast_re, const std::array<double, 512>& fast_im,
                     const DirectDft& direct) {
    double peak = 0.0;
    double worst = 0.0;
    for (std::size_t k = 0; k < 512; ++k) {
        peak = std::max(peak, std::hypot(direct.re[k], direct.im[k]));
        worst = std::max(worst, std::hypot(fast_re[k] - direct.re[k], fast_im[k] - direct.im[k]));
    }
    return worst / std::max(peak, 1e-12);
}

// Real-audio-shaped input, not silence and not one stationary tone: two
// partials at a per-block phase offset, the same shape test_mdct_fast.cpp's
// own tone_block builds, so consecutive blocks cover different spectra.
std::array<double, 512> tone_block(int block_offset) {
    std::array<double, 512> block{};
    for (int n = 0; n < 512; ++n) {
        const double t = static_cast<double>(block_offset * 256 + n) / 48000.0;
        block[static_cast<std::size_t>(n)] =
            0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * t) +
            0.15 * std::sin(2.0 * std::numbers::pi * 2500.0 * t);
    }
    return block;
}

}  // namespace

TEST_CASE("dft512 of a unit impulse is a flat 1/N spectrum", "[fft]") {
    // x[n] = delta[n] -> Z[k] = (1/N) * 1 for every k, the direct-form sum's
    // simplest possible check: every twiddle factor is multiplied by zero
    // except at n = 0, where cos(0) = 1 and sin(0) = 0.
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    re[0] = 1.0;
    ac3::dft512(re, im, real_out, imag_out);
    for (int k = 0; k < 512; ++k) {
        CAPTURE(k);
        CHECK(near(real_out[static_cast<std::size_t>(k)], 1.0 / 512.0));
        CHECK(near(imag_out[static_cast<std::size_t>(k)], 0.0));
    }
}

TEST_CASE("dft512 of a DC signal concentrates entirely in bin 0", "[fft]") {
    // x[n] = 1 for all n -> Z[0] = 1, Z[k != 0] = 0 (every other bin's kernel
    // sums a full period of a root of unity, which cancels exactly).
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    re.fill(1.0);
    ac3::dft512(re, im, real_out, imag_out);
    CHECK(near(real_out[0], 1.0));
    CHECK(near(imag_out[0], 0.0));
    for (int k = 1; k < 512; ++k) {
        CAPTURE(k);
        CHECK(near(real_out[static_cast<std::size_t>(k)], 0.0, 1e-8));
        CHECK(near(imag_out[static_cast<std::size_t>(k)], 0.0, 1e-8));
    }
}

TEST_CASE("dft512 of a bin-aligned real cosine splits evenly between k and N-k",
          "[fft]") {
    // x[n] = cos(2*pi*f*n/N) = 0.5*e^{j2pi f n/N} + 0.5*e^{-j2pi f n/N}, so
    // (given this DFT's e^{-j2pi kn/N} kernel) energy lands entirely at
    // k = f and k = N - f, real-valued (zero phase) at both.
    constexpr int kBin = 3;
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    for (int n = 0; n < 512; ++n) {
        re[static_cast<std::size_t>(n)] =
            std::cos(2.0 * std::numbers::pi * kBin * n / 512.0);
    }
    ac3::dft512(re, im, real_out, imag_out);
    CHECK(near(real_out[kBin], 0.5, 1e-8));
    CHECK(near(imag_out[kBin], 0.0, 1e-8));
    CHECK(near(real_out[512 - kBin], 0.5, 1e-8));
    CHECK(near(imag_out[512 - kBin], 0.0, 1e-8));
    // A bin well away from both peaks should carry negligible energy.
    CHECK(near(real_out[100], 0.0, 1e-8));
    CHECK(near(imag_out[100], 0.0, 1e-8));
}

TEST_CASE("dft512 is linear", "[fft]") {
    // Z(a*x + b*y) == a*Z(x) + b*Z(y) - a property the direct-form sum has by
    // construction, but worth pinning down since a fast (radix-2) rewrite of
    // this primitive must preserve it exactly.
    std::array<double, 512> x{};
    std::array<double, 512> y{};
    std::array<double, 512> zero{};
    x[1] = 1.0;
    y[7] = 1.0;
    std::array<double, 512> zx_re{}, zx_im{}, zy_re{}, zy_im{};
    ac3::dft512(x, zero, zx_re, zx_im);
    ac3::dft512(y, zero, zy_re, zy_im);

    std::array<double, 512> combined{};
    for (int n = 0; n < 512; ++n) {
        combined[static_cast<std::size_t>(n)] =
            2.0 * x[static_cast<std::size_t>(n)] - 0.5 * y[static_cast<std::size_t>(n)];
    }
    std::array<double, 512> zc_re{}, zc_im{};
    ac3::dft512(combined, zero, zc_re, zc_im);

    for (int k = 0; k < 512; ++k) {
        CAPTURE(k);
        const double expected_re =
            2.0 * zx_re[static_cast<std::size_t>(k)] - 0.5 * zy_re[static_cast<std::size_t>(k)];
        const double expected_im =
            2.0 * zx_im[static_cast<std::size_t>(k)] - 0.5 * zy_im[static_cast<std::size_t>(k)];
        CHECK(near(zc_re[static_cast<std::size_t>(k)], expected_re, 1e-8));
        CHECK(near(zc_im[static_cast<std::size_t>(k)], expected_im, 1e-8));
    }
}

TEST_CASE("dft512 agrees with the direct-form summation on real audio", "[fft]") {
    // The bound every other fast transform in this library is held to
    // (tests/core/test_mdct_fast.cpp's kFastTolerance): the fast structure is
    // an implementation of the sum fft.hpp states, and has to reproduce it.
    // Six consecutive blocks - three-plus frames' worth - rather than one, on
    // this project's own "silence and frame 0 give false passes" rule.
    constexpr double kTolerance = 1e-10;
    std::array<double, 512> zero{};
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        const auto x = tone_block(block);
        std::array<double, 512> fast_re{};
        std::array<double, 512> fast_im{};
        ac3::dft512(x, zero, fast_re, fast_im);
        const double err = max_rel_error(fast_re, fast_im, direct_dft512(x, zero));
        CAPTURE(err);
        CHECK(err < kTolerance);
    }
}

TEST_CASE("dft512 agrees with the direct-form summation on complex input", "[fft]") {
    // ecpl's step 3 hands this transform a genuinely complex signal (the
    // xcos3/xsin3 twiddle puts energy in both parts), so the real-input case
    // above is not the whole contract.
    constexpr double kTolerance = 1e-10;
    for (int block = 0; block < 3; ++block) {
        CAPTURE(block);
        const auto x_re = tone_block(block);
        const auto x_im = tone_block(block + 3);
        std::array<double, 512> fast_re{};
        std::array<double, 512> fast_im{};
        ac3::dft512(x_re, x_im, fast_re, fast_im);
        const double err = max_rel_error(fast_re, fast_im, direct_dft512(x_re, x_im));
        CAPTURE(err);
        CHECK(err < kTolerance);
    }
}
