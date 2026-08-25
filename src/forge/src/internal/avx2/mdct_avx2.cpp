#include "mdct_avx2.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "ac3/core/window.hpp"

#include "simd_avx2.hpp"

namespace ac3::internal::avx2 {

void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    const double* const in = x.data();
    double* const out = windowed.data();
    for (std::size_t n = 0; n < 512; n += 4) {
        (f64x4::load(in + n) * f64x4::load(&ac3::kAnalysisWindow[n])).store(out + n);
    }
}

void dct4_pre_twiddle(std::span<const double> u, std::span<const double> pre_re,
                      std::span<const double> pre_im, std::span<const std::uint16_t> bitrev,
                      std::span<double> z_re, std::span<double> z_im) {
    const std::size_t p = pre_re.size();
    const std::size_t m_len = u.size();  // M
    for (std::size_t m = 0; m < p; m += 4) {
        const auto a = f64x4::set(u[2 * m], u[2 * m + 2], u[2 * m + 4], u[2 * m + 6]);
        const auto b = f64x4::set(u[m_len - 1 - 2 * m], u[m_len - 3 - 2 * m],
                                  u[m_len - 5 - 2 * m], u[m_len - 7 - 2 * m]);
        const auto pre_re_v = f64x4::load(&pre_re[m]);
        const auto pre_im_v = f64x4::load(&pre_im[m]);
        const auto zr = a * pre_re_v - b * pre_im_v;
        const auto zi = a * pre_im_v + b * pre_re_v;
        const std::size_t d0 = bitrev[m];
        const std::size_t d1 = bitrev[m + 1];
        const std::size_t d2 = bitrev[m + 2];
        const std::size_t d3 = bitrev[m + 3];
        z_re[d0] = zr.lane0();
        z_im[d0] = zi.lane0();
        z_re[d1] = zr.lane1();
        z_im[d1] = zi.lane1();
        z_re[d2] = zr.lane2();
        z_im[d2] = zi.lane2();
        z_re[d3] = zr.lane3();
        z_im[d3] = zi.lane3();
    }
}

void dct4_post_twiddle(std::span<const double> z_re, std::span<const double> z_im,
                       std::span<const double> post_re, std::span<const double> post_im,
                       double scale, std::span<double> out) {
    const std::size_t p = z_re.size();
    const auto scale_v = f64x4::broadcast(scale);
    for (std::size_t k = 0; k < p; k += 4) {
        const auto zr = f64x4::load(&z_re[k]);
        const auto zi = f64x4::load(&z_im[k]);
        const auto post_re_v = f64x4::load(&post_re[k]);
        const auto post_im_v = f64x4::load(&post_im[k]);
        const auto even = scale_v * (zr * post_re_v - zi * post_im_v);
        const auto odd = scale_v * (-(zr * post_im_v + zi * post_re_v));
        out[2 * k] = even.lane0();
        out[2 * k + 2] = even.lane1();
        out[2 * k + 4] = even.lane2();
        out[2 * k + 6] = even.lane3();
        const std::size_t m_len = out.size();
        out[m_len - 1 - 2 * k] = odd.lane0();
        out[m_len - 3 - 2 * k] = odd.lane1();
        out[m_len - 5 - 2 * k] = odd.lane2();
        out[m_len - 7 - 2 * k] = odd.lane3();
    }
}

void imdct512_pre_twiddle(std::span<const double> coeffs, std::span<const double> cos1,
                          std::span<const double> sin1, std::span<const std::uint16_t> bitrev,
                          std::span<double> z_re, std::span<double> z_im) {
    const std::size_t k_half_n = coeffs.size();  // 512
    const std::size_t quarter = cos1.size();      // 128
    for (std::size_t k = 0; k < quarter; k += 4) {
        const auto a = f64x4::set(coeffs[k_half_n - 2 * k - 1], coeffs[k_half_n - 2 * k - 3],
                                  coeffs[k_half_n - 2 * k - 5], coeffs[k_half_n - 2 * k - 7]);
        const auto b = f64x4::set(coeffs[2 * k], coeffs[2 * k + 2], coeffs[2 * k + 4],
                                  coeffs[2 * k + 6]);
        const auto c = f64x4::load(&cos1[k]);
        const auto sn = f64x4::load(&sin1[k]);
        const auto zr = a * c - b * sn;
        const auto zi = -(b * c + a * sn);
        const std::size_t d0 = bitrev[k];
        const std::size_t d1 = bitrev[k + 1];
        const std::size_t d2 = bitrev[k + 2];
        const std::size_t d3 = bitrev[k + 3];
        z_re[d0] = zr.lane0();
        z_im[d0] = zi.lane0();
        z_re[d1] = zr.lane1();
        z_im[d1] = zi.lane1();
        z_re[d2] = zr.lane2();
        z_im[d2] = zi.lane2();
        z_re[d3] = zr.lane3();
        z_im[d3] = zi.lane3();
    }
}

void imdct512_negate_copy(std::span<const double> z_re, std::span<const double> z_im,
                          std::span<double> t_re, std::span<double> t_im) {
    const std::size_t n_len = z_re.size();
    for (std::size_t n = 0; n < n_len; n += 4) {
        f64x4::load(&z_re[n]).store(&t_re[n]);
        (-f64x4::load(&z_im[n])).store(&t_im[n]);
    }
}

void imdct512_post_twiddle(std::span<const double> cos1, std::span<const double> sin1,
                           std::span<const double> t_re, std::span<const double> t_im,
                           std::span<double> y_re, std::span<double> y_im) {
    const std::size_t n_len = cos1.size();
    for (std::size_t n = 0; n < n_len; n += 4) {
        const auto c = f64x4::load(&cos1[n]);
        const auto sn = f64x4::load(&sin1[n]);
        const auto tr = f64x4::load(&t_re[n]);
        const auto ti = f64x4::load(&t_im[n]);
        (tr * c - ti * sn).store(&y_re[n]);
        (ti * c + tr * sn).store(&y_im[n]);
    }
}

void imdct256_post_twiddle(std::span<const double> cos2, std::span<const double> sin2,
                           std::span<const double> t1_re, std::span<const double> t1_im,
                           std::span<const double> t2_re, std::span<const double> t2_im,
                           std::span<double> y1_re, std::span<double> y1_im,
                           std::span<double> y2_re, std::span<double> y2_im) {
    const std::size_t n_len = cos2.size();
    for (std::size_t n = 0; n < n_len; n += 4) {
        const auto c = f64x4::load(&cos2[n]);
        const auto sn = f64x4::load(&sin2[n]);
        const auto t1r = f64x4::load(&t1_re[n]);
        const auto t1i = f64x4::load(&t1_im[n]);
        const auto t2r = f64x4::load(&t2_re[n]);
        const auto t2i = f64x4::load(&t2_im[n]);
        (t1r * c - t1i * sn).store(&y1_re[n]);
        (t1i * c + t1r * sn).store(&y1_im[n]);
        (t2r * c - t2i * sn).store(&y2_re[n]);
        (t2i * c + t2r * sn).store(&y2_im[n]);
    }
}

void imdct512_windowed_batch4(std::span<const double> coeffs0, std::span<const double> coeffs1,
                              std::span<const double> coeffs2, std::span<const double> coeffs3,
                              std::span<const double> cos1, std::span<const double> sin1,
                              const ac3::internal::FftTables<128>& fft, std::span<double> x0,
                              std::span<double> x1, std::span<double> x2, std::span<double> x3) {
    constexpr std::size_t kQuarter = 128;
    constexpr std::size_t kEighth = 64;
    constexpr std::size_t kHalfN = 256;  // coeffsN.size()

    // Steps 2-3: pre-twiddle + in-place FFT, one f64x4 per bin (all four
    // objects' values for that bin) instead of one f64x2/f64x4 per group
    // of bins within one object. Same sign convention and gather shape as
    // imdct512_pre_twiddle above - the gather here walks the SAME index
    // across four independent coefficient spans instead of four
    // consecutive bins of one - and the same bitrev scatter target, except
    // the "scatter" is a single f64x4 store per bin (all four objects move
    // together) rather than four separate scalar stores, since the
    // destination is already object-interleaved by construction.
    std::array<f64x4, kQuarter> z_re{};
    std::array<f64x4, kQuarter> z_im{};
    for (std::size_t k = 0; k < kQuarter; ++k) {
        const auto a = f64x4::set(coeffs0[kHalfN - 2 * k - 1], coeffs1[kHalfN - 2 * k - 1],
                                  coeffs2[kHalfN - 2 * k - 1], coeffs3[kHalfN - 2 * k - 1]);
        const auto b = f64x4::set(coeffs0[2 * k], coeffs1[2 * k], coeffs2[2 * k], coeffs3[2 * k]);
        const double c = cos1[k];
        const double sn = sin1[k];
        const auto zr = (a * c) - (b * sn);
        const auto zi = -((b * c) + (a * sn));
        const std::size_t d = fft.bitrev[k];
        z_re[d] = zr;
        z_im[d] = zi;
    }
    fft_forward_bitrev<kQuarter, f64x4>(fft, z_re, z_im);

    // Post-FFT negate-copy: unit stride, nothing to gather or scatter -
    // same as imdct512_negate_copy above, just f64x4-per-bin.
    std::array<f64x4, kQuarter> t_re{};
    std::array<f64x4, kQuarter> t_im{};
    for (std::size_t n = 0; n < kQuarter; ++n) {
        t_re[n] = z_re[n];
        t_im[n] = -z_im[n];
    }

    // Step 4 post-twiddle: unit stride - same as imdct512_post_twiddle
    // above, just f64x4-per-bin.
    std::array<f64x4, kQuarter> y_re{};
    std::array<f64x4, kQuarter> y_im{};
    for (std::size_t n = 0; n < kQuarter; ++n) {
        const double c = cos1[n];
        const double sn = sin1[n];
        y_re[n] = (t_re[n] * c) - (t_im[n] * sn);
        y_im[n] = (t_im[n] * c) + (t_re[n] * sn);
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field
    // from imdct512_windowed's own step 5 (mdct.cpp) - the one place this
    // function scatters out to four SEPARATE destinations, since x0..x3
    // are four independent objects' own output buffers, not an
    // object-interleaved one. `scatter4` is the one new piece: extracting
    // all four lanes of one already-computed f64x4 result into the four
    // outputs, rather than four independent scalar computations.
    const auto& w = ac3::kAnalysisWindow;
    const auto yr = [&](std::size_t i) { return y_re[i]; };
    const auto yi = [&](std::size_t i) { return y_im[i]; };
    const auto scatter4 = [](f64x4 v, double& p0, double& p1, double& p2, double& p3) {
        p0 = v.lane0();
        p1 = v.lane1();
        p2 = v.lane2();
        p3 = v.lane3();
    };
    for (std::size_t n = 0; n < kEighth; ++n) {
        scatter4(-yi(kEighth + n) * w[2 * n], x0[2 * n], x1[2 * n], x2[2 * n], x3[2 * n]);
        scatter4(yr(kEighth - n - 1) * w[2 * n + 1], x0[2 * n + 1], x1[2 * n + 1], x2[2 * n + 1],
                x3[2 * n + 1]);
        scatter4(-yr(n) * w[kQuarter + 2 * n], x0[kQuarter + 2 * n], x1[kQuarter + 2 * n],
                x2[kQuarter + 2 * n], x3[kQuarter + 2 * n]);
        scatter4(yi(kQuarter - n - 1) * w[kQuarter + 2 * n + 1], x0[kQuarter + 2 * n + 1],
                x1[kQuarter + 2 * n + 1], x2[kQuarter + 2 * n + 1], x3[kQuarter + 2 * n + 1]);
        scatter4(-yr(kEighth + n) * w[kHalfN - 2 * n - 1], x0[kHalfN + 2 * n],
                x1[kHalfN + 2 * n], x2[kHalfN + 2 * n], x3[kHalfN + 2 * n]);
        scatter4(yi(kEighth - n - 1) * w[kHalfN - 2 * n - 2], x0[kHalfN + 2 * n + 1],
                x1[kHalfN + 2 * n + 1], x2[kHalfN + 2 * n + 1], x3[kHalfN + 2 * n + 1]);
        scatter4(yi(n) * w[kQuarter - 2 * n - 1], x0[3 * kQuarter + 2 * n],
                x1[3 * kQuarter + 2 * n], x2[3 * kQuarter + 2 * n], x3[3 * kQuarter + 2 * n]);
        scatter4(-yr(kQuarter - n - 1) * w[kQuarter - 2 * n - 2], x0[3 * kQuarter + 2 * n + 1],
                x1[3 * kQuarter + 2 * n + 1], x2[3 * kQuarter + 2 * n + 1],
                x3[3 * kQuarter + 2 * n + 1]);
    }
}

}  // namespace ac3::internal::avx2
