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

    // Layout seam, entry side: interleave the four objects' spectra into
    // one f64x4 per bin, 4x4 blocks at a time - four contiguous vector
    // loads (one per object) and eight shuffles per sixteen doubles, in
    // place of the two f64x4::set serial-insert gathers per pre-twiddle
    // iteration an earlier version of this function paid (see this
    // function's own doc comment in mdct_avx2.hpp for the measured
    // history). 8KB of dense scratch, L1-resident for the whole call.
    std::array<f64x4, kHalfN> spectra{};
    for (std::size_t bin = 0; bin < kHalfN; bin += 4) {
        auto r0 = f64x4::load(&coeffs0[bin]);
        auto r1 = f64x4::load(&coeffs1[bin]);
        auto r2 = f64x4::load(&coeffs2[bin]);
        auto r3 = f64x4::load(&coeffs3[bin]);
        transpose4x4(r0, r1, r2, r3);
        spectra[bin] = r0;
        spectra[bin + 1] = r1;
        spectra[bin + 2] = r2;
        spectra[bin + 3] = r3;
    }

    // Steps 2-3: pre-twiddle + in-place FFT, one f64x4 per bin (all four
    // objects' values for that bin) instead of one f64x2/f64x4 per group
    // of bins within one object. Same sign convention as
    // imdct512_pre_twiddle above - and, with spectra interleaved, both
    // reads are plain indexed vector loads rather than gathers; the
    // bitrev "scatter" is a single f64x4 store per bin (all four objects
    // move together), since the destination is object-interleaved by
    // construction.
    std::array<f64x4, kQuarter> z_re{};
    std::array<f64x4, kQuarter> z_im{};
    for (std::size_t k = 0; k < kQuarter; ++k) {
        const auto a = spectra[kHalfN - 2 * k - 1];
        const auto b = spectra[2 * k];
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
    // from imdct512_windowed's own step 5 (mdct.cpp) - with the layout
    // seam's exit side folded straight in. Each window region writes two
    // consecutive output rows per n, so a PAIR of n makes four consecutive
    // rows per region; transposing those four row-vectors (each holding
    // all four objects at one row) turns them into four per-object runs of
    // four contiguous samples, stored with one plain vector store each -
    // in place of the lane0()..lane3() extraction scatter (four dependent
    // extract+store chains per row, 512 rows a call) an earlier version of
    // this function paid. The multiplies below are the identical
    // operations imdct512_windowed's own step 5 performs, in the identical
    // order - each formula is the original evaluated at n and n + 1 - and
    // the transpose after them moves finished values only, so this stays
    // bit-identical to four separate scalar calls.
    const auto& w = ac3::kAnalysisWindow;
    const auto yr = [&](std::size_t i) { return y_re[i]; };
    const auto yi = [&](std::size_t i) { return y_im[i]; };
    const auto store_run = [&](std::size_t base, f64x4 r0, f64x4 r1, f64x4 r2, f64x4 r3) {
        transpose4x4(r0, r1, r2, r3);
        r0.store(&x0[base]);
        r1.store(&x1[base]);
        r2.store(&x2[base]);
        r3.store(&x3[base]);
    };
    for (std::size_t n = 0; n < kEighth; n += 2) {
        store_run(2 * n,  //
                  -yi(kEighth + n) * w[2 * n],  //
                  yr(kEighth - n - 1) * w[2 * n + 1],
                  -yi(kEighth + n + 1) * w[2 * n + 2],  //
                  yr(kEighth - n - 2) * w[2 * n + 3]);
        store_run(kQuarter + 2 * n,  //
                  -yr(n) * w[kQuarter + 2 * n],  //
                  yi(kQuarter - n - 1) * w[kQuarter + 2 * n + 1],
                  -yr(n + 1) * w[kQuarter + 2 * n + 2],
                  yi(kQuarter - n - 2) * w[kQuarter + 2 * n + 3]);
        store_run(kHalfN + 2 * n,  //
                  -yr(kEighth + n) * w[kHalfN - 2 * n - 1],
                  yi(kEighth - n - 1) * w[kHalfN - 2 * n - 2],
                  -yr(kEighth + n + 1) * w[kHalfN - 2 * n - 3],
                  yi(kEighth - n - 2) * w[kHalfN - 2 * n - 4]);
        store_run(3 * kQuarter + 2 * n,  //
                  yi(n) * w[kQuarter - 2 * n - 1],  //
                  -yr(kQuarter - n - 1) * w[kQuarter - 2 * n - 2],
                  yi(n + 1) * w[kQuarter - 2 * n - 3],
                  -yr(kQuarter - n - 2) * w[kQuarter - 2 * n - 4]);
    }
}

void mdct512_forward_batch4(std::span<const double> w0, std::span<const double> w1,
                            std::span<const double> w2, std::span<const double> w3,
                            std::span<const double> pre_re, std::span<const double> pre_im,
                            std::span<const double> post_re, std::span<const double> post_im,
                            const ac3::internal::FftTables<128>& fft, double scale,
                            std::span<double> c0, std::span<double> c1, std::span<double> c2,
                            std::span<double> c3) {
    constexpr std::size_t kQ = 128;       // NLen / 4
    constexpr std::size_t kM = 256;       // NLen / 2, the DCT-IV length
    constexpr std::size_t kP = 128;       // kM / 2, the FFT length

    // Load four blocks' values at four CONSECUTIVE indices and interleave
    // them: on return r0..r3 hold index base+0..base+3, each with all four
    // blocks in its lanes.
    const auto gather4 = [&](std::size_t base, f64x4& r0, f64x4& r1, f64x4& r2, f64x4& r3) {
        r0 = f64x4::load(&w0[base]);
        r1 = f64x4::load(&w1[base]);
        r2 = f64x4::load(&w2[base]);
        r3 = f64x4::load(&w3[base]);
        transpose4x4(r0, r1, r2, r3);
    };

    // Steps 1: the quarter fold, straight into interleaved form. Each of
    // the two halves pairs one ascending index walk with one descending
    // one; the descending walk loads the SAME contiguous range and simply
    // consumes the four transposed results back-to-front, so it needs no
    // reversal instruction of its own.
    std::array<f64x4, kM> u{};
    for (std::size_t i = 0; i < kQ; i += 4) {
        f64x4 a0{}, a1{}, a2{}, a3{};
        gather4(3 * kQ + i, a0, a1, a2, a3);  // w[3Q+i .. +3]
        f64x4 d0{}, d1{}, d2{}, d3{};
        gather4(3 * kQ - 4 - i, d0, d1, d2, d3);  // w[3Q-4-i .. 3Q-1-i]
        // u[i + n] = -w[3Q-1-i-n] - w[3Q+i+n]
        u[i] = (-d3) - a0;
        u[i + 1] = (-d2) - a1;
        u[i + 2] = (-d1) - a2;
        u[i + 3] = (-d0) - a3;
    }
    for (std::size_t j = 0; j < kQ; j += 4) {
        f64x4 b0{}, b1{}, b2{}, b3{};
        gather4(j, b0, b1, b2, b3);  // w[j .. j+3]
        f64x4 e0{}, e1{}, e2{}, e3{};
        gather4(2 * kQ - 4 - j, e0, e1, e2, e3);  // w[2Q-4-j .. 2Q-1-j]
        // u[Q + j + n] = w[j + n] - w[2Q-1-j-n]
        u[kQ + j] = b0 - e3;
        u[kQ + j + 1] = b1 - e2;
        u[kQ + j + 2] = b2 - e1;
        u[kQ + j + 3] = b3 - e0;
    }

    // dct4_scaled's pre-twiddle: same arithmetic and same bitrev scatter
    // target as dct4_pre_twiddle above, but u is already interleaved, so
    // both reads are indexed vector loads and the scatter is one vector
    // store per m.
    std::array<f64x4, kP> z_re{};
    std::array<f64x4, kP> z_im{};
    for (std::size_t m = 0; m < kP; ++m) {
        const auto a = u[2 * m];
        const auto b = u[kM - 1 - 2 * m];
        const double pr = pre_re[m];
        const double pi = pre_im[m];
        const std::size_t d = fft.bitrev[m];
        z_re[d] = (a * pr) - (b * pi);
        z_im[d] = (a * pi) + (b * pr);
    }
    fft_forward_bitrev<kP, f64x4>(fft, z_re, z_im);

    // dct4_scaled's post-twiddle, into an interleaved coefficient scratch:
    // the even/odd split writes stride +2 and -2, which is not a
    // contiguous run in the OUTPUT index, so this pass keeps the
    // interleaved form and the de-interleave happens once, below.
    std::array<f64x4, kM> out{};
    for (std::size_t k = 0; k < kP; ++k) {
        const auto zr = z_re[k];
        const auto zi = z_im[k];
        const double qr = post_re[k];
        const double qi = post_im[k];
        out[2 * k] = scale * ((zr * qr) - (zi * qi));
        out[kM - 1 - 2 * k] = scale * (-((zr * qi) + (zi * qr)));
    }

    // Layout seam, exit side: four consecutive coefficient indices
    // transpose back into four per-block contiguous runs, one vector store
    // each - the same trick the entry side uses, run once over kM rows.
    for (std::size_t r = 0; r < kM; r += 4) {
        auto r0 = out[r];
        auto r1 = out[r + 1];
        auto r2 = out[r + 2];
        auto r3 = out[r + 3];
        transpose4x4(r0, r1, r2, r3);
        r0.store(&c0[r]);
        r1.store(&c1[r]);
        r2.store(&c2[r]);
        r3.store(&c3[r]);
    }
}

}  // namespace ac3::internal::avx2
