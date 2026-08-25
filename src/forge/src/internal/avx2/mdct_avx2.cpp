#include "mdct_avx2.hpp"

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

}  // namespace ac3::internal::avx2
