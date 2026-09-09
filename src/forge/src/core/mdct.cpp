#include "ac3/core/mdct.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <type_traits>

#include "ac3/core/window.hpp"
#include "ac3/internal/arch/simd.hpp"

#include "cpu_features.hpp"
#include "fft_kernel.hpp"
#include "mdct_avx2.hpp"
#include "reference_transform.hpp"

namespace ac3 {

namespace {

constexpr int kN = kTransformLength;  // 512
constexpr double kPi = std::numbers::pi;

// §7.9.4.1 step 2: xcos1[k] = -cos(2pi(8k+1)/8N), xsin1[k] = -sin(2pi(8k+1)/8N).
//
// Scalar (roadmap PF7's float32 gap): the type the twiddles are STORED in.
// Computed in double and narrowed once on the way in, for the same reason
// fft_kernel.hpp's FftTables does it - the angle here is small and exact and
// deserves the library call at full precision whatever the table holds.
// Default double, so every existing use is the one it always was.
template <typename Scalar = double>
struct Twiddles {
    std::array<Scalar, kN / 4> cos1;
    std::array<Scalar, kN / 4> sin1;
    Twiddles() {
        for (int k = 0; k < kN / 4; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (8.0 * kN);
            cos1[static_cast<std::size_t>(k)] = static_cast<Scalar>(-std::cos(angle));
            sin1[static_cast<std::size_t>(k)] = static_cast<Scalar>(-std::sin(angle));
        }
    }
};

template <typename Scalar = double>
const Twiddles<Scalar>& twiddles() {
    static const Twiddles<Scalar> t;
    return t;
}

// §7.9.4.2 step 2: xcos2[k] = -cos(2pi(8k+1)/4N), xsin2[k] = -sin(2pi(8k+1)/4N)
// (N = 512 throughout this section, per the spec's own note — these are NOT
// the 256-sample transform's own N).
template <typename Scalar = double>
struct Twiddles2 {
    std::array<Scalar, kN / 8> cos2;
    std::array<Scalar, kN / 8> sin2;
    Twiddles2() {
        for (int k = 0; k < kN / 8; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (4.0 * kN);
            cos2[static_cast<std::size_t>(k)] = static_cast<Scalar>(-std::cos(angle));
            sin2[static_cast<std::size_t>(k)] = static_cast<Scalar>(-std::sin(angle));
        }
    }
};

template <typename Scalar = double>
const Twiddles2<Scalar>& twiddles2() {
    static const Twiddles2<Scalar> t;
    return t;
}

// §7.9.4.1 step 5's window, in the transform's own scalar type.
//
// kAnalysisWindow is consteval and double (ac3/core/window.hpp), which is the
// right thing for it to be: it is generated from a Kaiser-Bessel derivation
// whose intermediate sums want every bit they can get. A float32 transform
// still wants the correctly-rounded float OF that table rather than a table
// re-derived in float, so the double one stays the source and this narrows it
// once, lazily, in the builds that ask for it. The double case returns a
// reference to the constexpr table and copies nothing.
template <typename Scalar>
const std::array<Scalar, static_cast<std::size_t>(kN)>& analysis_window();

template <>
const std::array<double, static_cast<std::size_t>(kN)>& analysis_window<double>() {
    return kAnalysisWindow;
}

template <>
const std::array<float, static_cast<std::size_t>(kN)>& analysis_window<float>() {
    static const std::array<float, static_cast<std::size_t>(kN)> narrowed = [] {
        std::array<float, static_cast<std::size_t>(kN)> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<float>(kAnalysisWindow[i]);
        }
        return out;
    }();
    return narrowed;
}

// --- §7.9.4 fast N/4-FFT structure (the encoder-config default; see
// mdct512_forward's own doc comment and EncoderConfig::fast_mdct /
// eac3::FrameConfig::fast_mdct) ---------------------------------------------
//
// All three transforms now run fast paths, each with ITS OWN fold - a
// distinction that matters because the direct-form phase is
// theta_k(n) + phi_k(alpha), phi_k(alpha) = (pi/4)(2k+1)(1+alpha) - a shift
// that depends on k but never n - and phi_k(0) = (pi/4)(2k+1) is exactly the
// "+ N/4" term folded into the standard MDCT formula the LONG fold computes
// (X[k] = (-2/N) sum x[n] cos(2pi/N (n+1/2+N/4)(k+1/2))). That term is NOT
// zero, so it does not vanish for alpha = 0 - a fact worth stating plainly
// because an earlier version of this comment claimed alpha = -1 (phi_k = 0,
// the BARE cosine sum with no N/4 shift at all) was "the same formula" as
// alpha = 0 "just at a different NLen". It is not: phi_k(0) != phi_k(-1), so
// X_0 and X_{-1} are two different transforms of the same data, and a
// standalone numerical check (comparing the long fold against a hand
// reference implementing each phase separately) confirmed it reproduces
// X_0 to ~1e-15 but is off by 100%+ against X_{-1}. The short transforms'
// own folds (derived independently, each verified against ITS OWN
// direct-form table - see their function comments below) both land on the
// same scaled-DCT-IV core at M = 128: alpha = -1 is the DCT-IV of an
// antisymmetric half-fold, and alpha = +1's DST-IV-shaped sum is the
// DCT-IV of the reversed symmetric half-fold, via
// DST4(w)[k] = (-1)^k DCT4(w_R)[k].
//
// Every fold computes scale * DCT-IV(u) - u a length-M "folded" input -
// via one P = M/2-point complex FFT, the standard trick for a real-input
// DCT-IV. The long fold was verified 2026-08-14 against ForwardCosTable
// (the direct-form ground truth, now in
// src/core/transform/reference/reference_transform.cpp) to max relative error ~3e-12
// on both random data and real audio, the short folds 2026-08-15 the same
// way; see tests/core/test_mdct_fast.cpp, which asserts a 1e-10 bound on all
// three.

// Everything angle-dependent in the fold below, computed once per NLen -
// the same treatment Twiddles and the direct-form matrices give every
// other transform in this file, applied to the fast path itself (phase-5
// target 1 of the performance programme: this kernel runs 36x per frame in
// every encode path, plus 6x per object per frame inside band_energy, and
// its per-call cost was dominated by the 512 std::cos/std::sin libm calls
// below being made fresh on every transform). Two exactness classes:
//
// - pre/post twiddles: the EXACT expressions the fold used to evaluate per
//   call (std::cos/std::sin of -pi*m/M and -pi*(4k+1)/(4M)), stored instead
//   of re-evaluated - bit-identical values, the direct-form tables' own
//   reasoning (src/core/transform/reference/reference_transform.cpp).
// - FFT stage twiddles + bit-reversal permutation: the previous in-place
//   FFT generated each butterfly group's j-th twiddle by ITERATED complex
//   multiply (w *= wlen), so it carried j-1 accumulated rounding steps.
//   The table stores std::cos/std::sin of each exact angle -2*pi*j/len
//   instead - a (tiny) numerical change in the direction of MORE precision,
//   re-verified against the direct form's ground truth by
//   tests/core/test_mdct_fast.cpp's unchanged 1e-10 bound.
template <int NLen, typename Scalar = double>
struct FastMdctTables {
    static constexpr std::size_t kM = static_cast<std::size_t>(NLen) / 2;
    static constexpr std::size_t kP = kM / 2;
    // z[m] pre-twiddle exp(-i*pi*m/M), split re/im.
    std::array<Scalar, kP> pre_re{};
    std::array<Scalar, kP> pre_im{};
    // w[k] post-twiddle exp(-i*pi*(4k+1)/(4M)), split re/im.
    std::array<Scalar, kP> post_re{};
    std::array<Scalar, kP> post_im{};
    // The P-point FFT's own tables (digit-reversal permutation + stage
    // twiddles) - the shared kernel's, so dft512 runs the identical
    // machinery at P = 512; see fft_kernel.hpp.
    internal::FftTables<kP, Scalar> fft{};
    FastMdctTables() {
        for (std::size_t m = 0; m < kP; ++m) {
            const double ang = -kPi * static_cast<double>(m) / static_cast<double>(kM);
            pre_re[m] = static_cast<Scalar>(std::cos(ang));
            pre_im[m] = static_cast<Scalar>(std::sin(ang));
            const double ang2 =
                -kPi * (4.0 * static_cast<double>(m) + 1.0) / (4.0 * static_cast<double>(kM));
            post_re[m] = static_cast<Scalar>(std::cos(ang2));
            post_im[m] = static_cast<Scalar>(std::sin(ang2));
        }
    }
};

template <int NLen, typename Scalar = double>
const FastMdctTables<NLen, Scalar>& fast_mdct_tables() {
    static const FastMdctTables<NLen, Scalar> t;
    return t;
}

// The scaled DCT-IV every fold below lands on: out[j] = scale * DCT4_M(u)[j]
// for the length-M input `u`, via one P = M/2-point complex FFT - the
// standard real-input DCT-IV factorization. z[m] = (u[2m] + i*u[M-1-2m]) *
// exp(-i*pi*m/M); Z = FFT_P(z); w[k] = Z[k] * exp(-i*pi*(4k+1)/(4M));
// DCT4[2k] = Re(w[k]), DCT4[M-1-2k] = -Im(w[k]). NLen names the TRANSFORM
// whose tables carry this DCT-IV's twiddles (M = NLen/2), so the long
// transform runs it at M = 256 and both short transforms at M = 128.
// The pre- and post-twiddle loops run two m/k at a time through the arch
// seam (ROADMAP PF5), four at a time on AVX2-capable hardware
// (ac3::internal::cpu::has_avx2(), ROADMAP PF5's dynamic-dispatch
// follow-on - see mdct_avx2.hpp/.cpp). Both are complex multiplies whose
// ARITHMETIC is contiguous even though their memory access is not: the
// pre-twiddle gathers u at stride +2 and stride -2 and scatters its result
// to `bitrev[m]` (the kernel wants its input already digit-reversed - see
// fft_kernel.hpp - so the quarter-split that was already gathering
// u[2m]/u[M-1-2m] scatters on the way out instead of the kernel spending a
// pass permuting in place); the post-twiddle reads the kernel's natural-
// order output at stride +1 and scatters out to stride +2/-2. Neither seam
// carries a shuffle or scatter-store operation, so every gather and every
// scatter stays scalar (f64x2::set/f64x4::set to gather, lane0..lane1/
// lane3 to scatter) and only the arithmetic between them goes wide - which
// is where the time is. Every lane performs the identical operations on the
// identical values the scalar form did, so the coefficients are
// bit-identical regardless of width; see fft_kernel.hpp's own header
// comment for the algorithm this feeds and tests/core/test_simd_kernels.cpp
// for the bit-exactness check (both tiers).
//
// P is kM/2 - 128 for the long transform, 64 for the short pair - so it is
// always even and neither loop needs a scalar tail.
template <int NLen, typename Scalar = double>
void dct4_scaled(const FastMdctTables<NLen, Scalar>& t, std::span<const Scalar> u,
                 std::span<Scalar> out, Scalar scale) {
    constexpr std::size_t M = FastMdctTables<NLen, Scalar>::kM;
    constexpr std::size_t P = FastMdctTables<NLen, Scalar>::kP;
    // The double instantiation keeps every path it had - the AVX2 tier, the
    // f64x2 seam, the same arithmetic in the same order. It is pinned by the
    // fifteen bitstream hashes and by every gold reference, so this
    // templatization deliberately changes nothing it does.
    //
    // float takes the plain scalar loops below instead. The vector seam has an
    // f32x4 now (ROADMAP PF5's follow-on), and using it here is worth doing -
    // but it is a separate change with its own measurement, and correctness on
    // a path that had no float form at all comes first. The AVX2 kernels are
    // double-only in any case: internal::avx2::dct4_pre_twiddle and friends
    // take double spans, so the branch below is not merely skipped for float,
    // it could not be taken.
    constexpr bool kWide = std::is_same_v<Scalar, double>;
    std::array<Scalar, P> z_re{};
    std::array<Scalar, P> z_im{};
    if constexpr (kWide) {
    if (internal::cpu::has_avx2()) {
        internal::avx2::dct4_pre_twiddle(u, t.pre_re, t.pre_im, t.fft.bitrev, z_re, z_im);
    } else {
        for (std::size_t m = 0; m < P; m += 2) {
            const auto a = internal::arch::f64x2::set(u[2 * m], u[2 * m + 2]);
            const auto b = internal::arch::f64x2::set(u[M - 1 - 2 * m], u[M - 3 - 2 * m]);
            const auto pre_re = internal::arch::f64x2::load(&t.pre_re[m]);
            const auto pre_im = internal::arch::f64x2::load(&t.pre_im[m]);
            const auto zr = a * pre_re - b * pre_im;
            const auto zi = a * pre_im + b * pre_re;
            const std::size_t d0 = t.fft.bitrev[m];
            const std::size_t d1 = t.fft.bitrev[m + 1];
            z_re[d0] = zr.lane0();
            z_im[d0] = zi.lane0();
            z_re[d1] = zr.lane1();
            z_im[d1] = zi.lane1();
        }
    }
    } else {
        for (std::size_t m = 0; m < P; ++m) {
            const Scalar a = u[2 * m];
            const Scalar b = u[M - 1 - 2 * m];
            const std::size_t d = t.fft.bitrev[m];
            z_re[d] = a * t.pre_re[m] - b * t.pre_im[m];
            z_im[d] = a * t.pre_im[m] + b * t.pre_re[m];
        }
    }
    internal::fft_forward_bitrev<P, Scalar>(t.fft, z_re, z_im);

    if constexpr (kWide) {
    if (internal::cpu::has_avx2()) {
        internal::avx2::dct4_post_twiddle(z_re, z_im, t.post_re, t.post_im, scale, out);
        return;
    }
    const auto scale_v = internal::arch::f64x2::broadcast(scale);
    for (std::size_t k = 0; k < P; k += 2) {
        const auto zr = internal::arch::f64x2::load(&z_re[k]);
        const auto zi = internal::arch::f64x2::load(&z_im[k]);
        const auto post_re = internal::arch::f64x2::load(&t.post_re[k]);
        const auto post_im = internal::arch::f64x2::load(&t.post_im[k]);
        const auto even = scale_v * (zr * post_re - zi * post_im);
        const auto odd = scale_v * (-(zr * post_im + zi * post_re));
        out[2 * k] = even.lane0();
        out[2 * k + 2] = even.lane1();
        out[M - 1 - 2 * k] = odd.lane0();
        out[M - 3 - 2 * k] = odd.lane1();
    }
    } else {
        for (std::size_t k = 0; k < P; ++k) {
            const Scalar zr = z_re[k];
            const Scalar zi = z_im[k];
            out[2 * k] = scale * (zr * t.post_re[k] - zi * t.post_im[k]);
            out[M - 1 - 2 * k] = scale * (-(zr * t.post_im[k] + zi * t.post_re[k]));
        }
    }
}

// DCT-IV-via-FFT fast path for the LONG transform (alpha = 0; see the block
// comment above): M = NLen/2, Q = NLen/4. Quarters of `windowed`: a = [0,Q),
// b = [Q,2Q), c = [2Q,3Q), d = [3Q,4Q). u = concat(-c_R - d, a - b_R),
// R = reversed, length M; coeffs = (-2/NLen) * DCT4_M(u).
template <int NLen, typename Scalar = double>
void mdct_forward_fast_core(std::span<const Scalar> windowed, std::span<Scalar> coeffs) {
    constexpr std::size_t Q = static_cast<std::size_t>(NLen) / 4;
    constexpr std::size_t M = FastMdctTables<NLen, Scalar>::kM;
    const auto& t = fast_mdct_tables<NLen, Scalar>();

    std::array<Scalar, M> u{};
    for (std::size_t i = 0; i < Q; ++i) {
        // -c_R[i] - d[i] = -windowed[3Q-1-i] - windowed[3Q+i]
        u[i] = -windowed[3 * Q - 1 - i] - windowed[3 * Q + i];
    }
    for (std::size_t j = 0; j < Q; ++j) {
        // a[j] - b_R[j] = windowed[j] - windowed[2Q-1-j]
        u[Q + j] = windowed[j] - windowed[2 * Q - 1 - j];
    }
    dct4_scaled<NLen, Scalar>(t, u, coeffs, static_cast<Scalar>(-2.0 / NLen));
}

}  // namespace

// Two (four under AVX2) samples per iteration through the arch seam (ROADMAP
// PF5, widened in PF5's dynamic-dispatch follow-on - see
// docs/building.md's "Runtime AVX2 dispatch"). The plainest kernel in the
// codec - 512 independent multiplies, unit stride on all three arrays - and
// therefore both the one where the vector form is most obviously the same
// arithmetic as the scalar one it replaced, and the one Phase 1's own
// measurement found the clearest real win for. kN is 512, so neither width
// leaves a tail.
void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    if (internal::cpu::has_avx2()) {
        internal::avx2::apply_analysis_window(x, windowed);
        return;
    }
    const double* const in = x.data();
    double* const out = windowed.data();
    for (std::size_t n = 0; n < static_cast<std::size_t>(kN); n += 2) {
        (internal::arch::f64x2::load(in + n) *
         internal::arch::f64x2::load(&kAnalysisWindow[n]))
            .store(out + n);
    }
}

void mdct512_forward(std::span<const double, 512> windowed, std::span<double, 256> coeffs,
                     bool fast) {
    if (fast) {
        mdct_forward_fast_core<512, double>(windowed, coeffs);
    } else {
        internal::reference_mdct512_forward(windowed, coeffs);
    }
}

// The float32 forms of the two above (roadmap PF7's float32 gap), for the
// object reconstruction in src/oba/joc.cpp - which runs a FORWARD transform in
// a decode, analysing the bed it is about to un-mix (PF8). Every other forward
// caller is the encoder, and the encoder stays double: the fifteen bitstream
// hashes in tests/golden/bitstream-hashes.json pin its output and nothing here
// is on its path.
//
// No `fast` parameter, the same as the float32 inverse already declared in
// mdct.hpp: the direct evaluation is the spec's own statement of the transform
// and the oracle the fast path is validated against, so it stays double.
// Offering a `false` a float32 caller could pass and then ignoring it would be
// worse than not offering it.
void apply_analysis_window(std::span<const float, 512> x, std::span<float, 512> windowed) {
    // A scalar loop, not the arch seam, over the float window the inverse
    // already uses (analysis_window<float>(): the double table narrowed once,
    // lazily). This used to narrow kAnalysisWindow per element instead, which
    // is the same value - but on a single-precision FPU each of those 512
    // narrowings is a software routine, and JOC's bed analysis runs this
    // thirty times a frame (docs/platforms/esp32.md).
    const auto& window = analysis_window<float>();
    for (std::size_t n = 0; n < static_cast<std::size_t>(kN); ++n) {
        windowed[n] = x[n] * window[n];
    }
}

void mdct512_forward(std::span<const float, 512> windowed, std::span<float, 256> coeffs) {
    mdct_forward_fast_core<512, float>(windowed, coeffs);
}

void mdct256_forward_first(std::span<const double, 256> windowed, std::span<double, 128> coeffs,
                           bool fast) {
    // alpha = -1 is the BARE cosine sum (phi_k = 0, no "+N/4" phase shift at
    // all) - a genuinely different transform from alpha = 0's, which is why
    // reusing the long transform's quarter-fold for it was a math error an
    // earlier version of this file made (see mdct_forward_fast_core's
    // comment). Its OWN fold, derived from its own phase: with M = 128, the
    // kernel cos(pi(2n+1)(2k+1)/512) at n' = 255-n is the n-kernel negated
    // (cos(pi(2k+1) - phi) = -cos(phi)), so the upper half folds into the
    // lower with a minus sign and the transform is exactly the M-point
    // DCT-IV of v[n] = x[n] - x[255-n], scaled by -2/256. Verified against
    // this file's direct-form table (tests/core/test_mdct_fast.cpp).
    if (fast) {
        std::array<double, 128> v{};
        for (std::size_t n = 0; n < 128; ++n) {
            v[n] = windowed[n] - windowed[255 - n];
        }
        dct4_scaled<256, double>(fast_mdct_tables<256, double>(), v, coeffs, -2.0 / 256);
        return;
    }
    internal::reference_mdct256_forward_first(windowed, coeffs);
}

void mdct256_forward_second(std::span<const double, 256> windowed, std::span<double, 128> coeffs,
                            bool fast) {
    // alpha = +1's phase shift turns the kernel into a sine (DST-IV-shaped)
    // sum: cos(phi + (pi/2)(2k+1)) = (-1)^(k+1) sin(phi). Folding the upper
    // half (sin(pi(2k+1) - psi) = +sin(psi), so it ADDS: w[n] = x[n] +
    // x[255-n]) gives (-2/256)(-1)^(k+1) DST4_128(w) - and DST-IV is the
    // DCT-IV of the REVERSED input with alternating signs, DST4(w)[k] =
    // (-1)^k DCT4(w_R)[k], so the two (-1)-factors cancel to exactly
    // (+2/256) * DCT4_128(w_R), w_R[n] = x[127-n] + x[128+n]. The "harder,
    // DST-IV-shaped" transform the phase-4 scoping deferred thus lands on
    // the SAME core as its siblings, one reversal away. Verified against
    // this file's direct-form table (tests/core/test_mdct_fast.cpp).
    if (fast) {
        std::array<double, 128> w_r{};
        for (std::size_t n = 0; n < 128; ++n) {
            w_r[n] = windowed[127 - n] + windowed[128 + n];
        }
        dct4_scaled<256, double>(fast_mdct_tables<256, double>(), w_r, coeffs, 2.0 / 256);
        return;
    }
    internal::reference_mdct256_forward_second(windowed, coeffs);
}

// The float32 short-block pair: the two double forms' own folds above, in
// float, on the float tables the short-block inverse already builds. Fast
// only - see mdct.hpp.
void mdct256_forward_first(std::span<const float, 256> windowed, std::span<float, 128> coeffs) {
    std::array<float, 128> v{};
    for (std::size_t n = 0; n < 128; ++n) {
        v[n] = windowed[n] - windowed[255 - n];
    }
    dct4_scaled<256, float>(fast_mdct_tables<256, float>(), v, coeffs,
                            static_cast<float>(-2.0 / 256));
}

void mdct256_forward_second(std::span<const float, 256> windowed, std::span<float, 128> coeffs) {
    std::array<float, 128> w_r{};
    for (std::size_t n = 0; n < 128; ++n) {
        w_r[n] = windowed[127 - n] + windowed[128 + n];
    }
    dct4_scaled<256, float>(fast_mdct_tables<256, float>(), w_r, coeffs,
                            static_cast<float>(2.0 / 256));
}

// Templated on the coefficient type (roadmap PF7). Each vectorised section
// below has three branches: f32x4 for a float instantiation, the AVX2 tier's
// four-wide double kernels where the CPU has them, and f64x2 otherwise. The
// float branch is separate because the AVX2 kernels take double spans and
// f64x2 holds two doubles - a float instantiation cannot reach either - and
// it runs four lanes at a time for the same 128-bit register the double
// branch fits two in.
//
// All three compute the same expressions in the same order, so each is
// bit-exact against the scalar loop it replaced at its own precision; see the
// arch seam's own header for that argument and
// tests/core/test_simd_kernels.cpp for where it is checked.
//
// `if constexpr` rather than a preprocessor conditional, so
// tools/checks/check_platform_macros.ps1 stays satisfied. Note that it
// discards only inside a template, which is what lets the float branch name
// f32x4::load(&t_re[n]) on an array of doubles: the expression is dependent,
// so the discarded branch is never instantiated.
template <typename Scalar>
void imdct512_windowed_impl(std::span<const Scalar, 256> coeffs, std::span<Scalar, 512> x,
                            bool fast) {
    constexpr bool kWide = std::is_same_v<Scalar, double>;
    // The float32 entry point below has no `fast` parameter precisely because
    // there is no choice to offer: the direct form is double-only. Nothing can
    // reach here asking for it, and this says so rather than trusting it.
    assert(kWide || fast);
    const auto& tw = twiddles<Scalar>();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Steps 2 and 3: the pre-transform complex multiply
    // Z[k] = (X[N/2-2k-1] + j*X[2k]) * (xcos1[k] + j*xsin1[k]), then the
    // N/4-point complex "IFFT". The pseudocode's sum
    // z[n] = sum_k Z[k] * (cos(8*pi*k*n/N) + j*sin(8*pi*k*n/N)), no scaling,
    // is with its +j*sin convention exactly an unscaled INVERSE DFT of Z -
    // so the fast path is the identity IDFT(Z) = conj(FFT(conj(Z))) through
    // the same FFT kernel the forward's fast fold uses (its P = 128 tables
    // are the long fold's own, fast_mdct_tables<512>().fft). The direct
    // branch keeps the spec's own evaluation, now behind
    // src/core/reference_transform.hpp so its 256 KiB matrix can be left out
    // of a build entirely (roadmap PF7) rather than merely never touched.
    //
    // The fast branch writes step 2's output already conjugated and already
    // digit-reversed, which is what lets the kernel skip both the input
    // conjugation pass and the bit-reversal pass the previous core ran
    // (fft_kernel.hpp); the direct branch needs neither, so it writes
    // Z[k] straight.
    std::array<Scalar, kQuarter> z_re{};
    std::array<Scalar, kQuarter> z_im{};
    std::array<Scalar, kQuarter> t_re{};
    std::array<Scalar, kQuarter> t_im{};
    if (fast) {
        // Two k at a time through f64x2, four through f32x4 or the AVX2
        // tier (ROADMAP PF5, ac3::internal::cpu::has_avx2()), the same
        // gather-compute-scatter shape as dct4_scaled's pre-twiddle: the
        // coefficient gather runs at stride -2/+2 and the scatter target is
        // bitrev[k], so both ends stay scalar and only the six multiplies
        // and two adds between them go wide. kQuarter is 128, a multiple of
        // 4, so no width leaves a tail. See dct4_scaled's own comment for
        // the bit-exactness argument this shares.
        const auto& fft = fast_mdct_tables<512, Scalar>().fft;
        if constexpr (!kWide) {
            // Four k at a time through f32x4 - the same gather-compute-scatter
            // shape as the f64x2 branch below, with twice the lanes because a
            // float is half the width. kQuarter is 128, so this leaves no
            // tail either.
            constexpr std::size_t kHalfN = static_cast<std::size_t>(kN) / 2;
            for (std::size_t k = 0; k < static_cast<std::size_t>(kQuarter); k += 4) {
                const auto a = internal::arch::f32x4::set(
                    coeffs[kHalfN - 2 * k - 1], coeffs[kHalfN - 2 * k - 3],
                    coeffs[kHalfN - 2 * k - 5], coeffs[kHalfN - 2 * k - 7]);
                const auto b = internal::arch::f32x4::set(coeffs[2 * k], coeffs[2 * k + 2],
                                                          coeffs[2 * k + 4], coeffs[2 * k + 6]);
                const auto c = internal::arch::f32x4::load(&tw.cos1[k]);
                const auto sn = internal::arch::f32x4::load(&tw.sin1[k]);
                const auto zr = a * c - b * sn;
                const auto zi = -(b * c + a * sn);
                const std::size_t d0 = fft.bitrev[k];
                const std::size_t d1 = fft.bitrev[k + 1];
                const std::size_t d2 = fft.bitrev[k + 2];
                const std::size_t d3 = fft.bitrev[k + 3];
                z_re[d0] = zr.lane0();
                z_im[d0] = zi.lane0();
                z_re[d1] = zr.lane1();
                z_im[d1] = zi.lane1();
                z_re[d2] = zr.lane2();
                z_im[d2] = zi.lane2();
                z_re[d3] = zr.lane3();
                z_im[d3] = zi.lane3();
            }
        } else if (internal::cpu::has_avx2()) {
            internal::avx2::imdct512_pre_twiddle(coeffs, tw.cos1, tw.sin1, fft.bitrev, z_re,
                                                 z_im);
        } else {
            constexpr std::size_t kHalfN = static_cast<std::size_t>(kN) / 2;
            for (std::size_t k = 0; k < static_cast<std::size_t>(kQuarter); k += 2) {
                const auto a = internal::arch::f64x2::set(coeffs[kHalfN - 2 * k - 1],
                                                          coeffs[kHalfN - 2 * k - 3]);
                const auto b = internal::arch::f64x2::set(coeffs[2 * k], coeffs[2 * k + 2]);
                const auto c = internal::arch::f64x2::load(&tw.cos1[k]);
                const auto sn = internal::arch::f64x2::load(&tw.sin1[k]);
                const auto zr = a * c - b * sn;
                const auto zi = -(b * c + a * sn);
                const std::size_t d0 = fft.bitrev[k];
                const std::size_t d1 = fft.bitrev[k + 1];
                z_re[d0] = zr.lane0();
                z_im[d0] = zi.lane0();
                z_re[d1] = zr.lane1();
                z_im[d1] = zi.lane1();
            }
        }
        internal::fft_forward_bitrev<static_cast<std::size_t>(kQuarter), Scalar>(fft, z_re, z_im);
        // Unit stride throughout, so this negation goes wide with nothing
        // to gather or scatter.
        if constexpr (!kWide) {
            for (std::size_t n = 0; n < static_cast<std::size_t>(kQuarter); n += 4) {
                internal::arch::f32x4::load(&z_re[n]).store(&t_re[n]);
                (-internal::arch::f32x4::load(&z_im[n])).store(&t_im[n]);
            }
        } else if (internal::cpu::has_avx2()) {
            internal::avx2::imdct512_negate_copy(z_re, z_im, t_re, t_im);
        } else {
            for (std::size_t n = 0; n < static_cast<std::size_t>(kQuarter); n += 2) {
                internal::arch::f64x2::load(&z_re[n]).store(&t_re[n]);
                (-internal::arch::f64x2::load(&z_im[n])).store(&t_im[n]);
            }
        }
    } else if constexpr (kWide) {
        // The direct form is double-only, and deliberately: it is the spec's
        // own evaluation and the oracle the fast path is measured against, so
        // narrowing it would remove the very thing it exists to be. A float32
        // caller asking for it is refused at the entry point below rather than
        // silently served something else - the same contract
        // src/core/reference_transform.hpp already states for the profile that
        // omits the tables.
        for (int k = 0; k < kQuarter; ++k) {
            const double a = coeffs[static_cast<std::size_t>(kN / 2 - 2 * k - 1)];
            const double b = coeffs[static_cast<std::size_t>(2 * k)];
            const double c = tw.cos1[static_cast<std::size_t>(k)];
            const double s = tw.sin1[static_cast<std::size_t>(k)];
            z_re[static_cast<std::size_t>(k)] = a * c - b * s;
            z_im[static_cast<std::size_t>(k)] = b * c + a * s;
        }
        internal::reference_inner_sum_128(z_re, z_im, t_re, t_im);
    }

    // Step 4: post-transform complex multiply. y[n] = z[n] * (xcos1[n] + j*xsin1[n])
    // Unit stride on every one of the six arrays, so this one vectorises
    // with nothing to gather or scatter, at every width (ROADMAP PF5).
    std::array<Scalar, kQuarter> y_re{};
    std::array<Scalar, kQuarter> y_im{};
    if constexpr (!kWide) {
        for (std::size_t n = 0; n < static_cast<std::size_t>(kQuarter); n += 4) {
            const auto c = internal::arch::f32x4::load(&tw.cos1[n]);
            const auto sn = internal::arch::f32x4::load(&tw.sin1[n]);
            const auto tr = internal::arch::f32x4::load(&t_re[n]);
            const auto ti = internal::arch::f32x4::load(&t_im[n]);
            (tr * c - ti * sn).store(&y_re[n]);
            (ti * c + tr * sn).store(&y_im[n]);
        }
    } else if (internal::cpu::has_avx2()) {
        internal::avx2::imdct512_post_twiddle(tw.cos1, tw.sin1, t_re, t_im, y_re, y_im);
    } else {
        for (std::size_t n = 0; n < static_cast<std::size_t>(kQuarter); n += 2) {
            const auto c = internal::arch::f64x2::load(&tw.cos1[n]);
            const auto sn = internal::arch::f64x2::load(&tw.sin1[n]);
            const auto tr = internal::arch::f64x2::load(&t_re[n]);
            const auto ti = internal::arch::f64x2::load(&t_im[n]);
            (tr * c - ti * sn).store(&y_re[n]);
            (ti * c + tr * sn).store(&y_im[n]);
        }
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    const auto& w = analysis_window<Scalar>();
    const auto yr = [&](int i) { return y_re[static_cast<std::size_t>(i)]; };
    const auto yi = [&](int i) { return y_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -yi(kEighth + n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            yr(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -yr(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            yi(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -yr(kEighth + n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            yi(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            yi(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -yr(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

// ROADMAP PF5's batch-axis follow-on (docs/building.md's own section):
// four independent imdct512_windowed(..., /*fast=*/true) calls, one object
// per SIMD lane, instead of four separate ones - see mdct.hpp's own doc
// comment for the shape and mdct_avx2.hpp's imdct512_windowed_batch4 for
// the AVX2 body. tw/fft are exactly the tables imdct512_windowed's own
// fast branch already looks up (twiddles(), fast_mdct_tables<512>().fft) -
// this function is the one place outside that fast branch that needs
// them, which is why the lookup is duplicated here rather than factored
// out: the AVX2 body lives in a separate translation unit (mdct_avx2.cpp)
// that cannot see this file's anonymous-namespace tables at all, so they
// have to be resolved here and passed down as plain spans/a plain-old-data
// reference, the same pattern every other AVX2 kernel call site in this
// file already uses.
void imdct512_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x,
                       bool fast) {
    imdct512_windowed_impl<double>(coeffs, x, fast);
}

// The float32 form (roadmap PF7). No `fast` parameter: the direct-form
// evaluation is the spec's own and is the oracle the fast path is measured
// against, so it stays double; narrowing it would defeat its purpose. The
// parameter is omitted rather than accepted and ignored, so the absence of a
// choice is visible at the call site.
void imdct512_windowed(std::span<const float, 256> coeffs, std::span<float, 512> x) {
    imdct512_windowed_impl<float>(coeffs, x, true);
}

void imdct512_windowed_batch4(std::span<const double, 256> coeffs0,
                              std::span<const double, 256> coeffs1,
                              std::span<const double, 256> coeffs2,
                              std::span<const double, 256> coeffs3, std::span<double, 512> x0,
                              std::span<double, 512> x1, std::span<double, 512> x2,
                              std::span<double, 512> x3) {
    if (internal::cpu::has_avx2()) {
        const auto& tw = twiddles();
        const auto& fft = fast_mdct_tables<512>().fft;
        internal::avx2::imdct512_windowed_batch4(coeffs0, coeffs1, coeffs2, coeffs3, tw.cos1,
                                                 tw.sin1, fft, x0, x1, x2, x3);
        return;
    }
    imdct512_windowed(coeffs0, x0, /*fast=*/true);
    imdct512_windowed(coeffs1, x1, /*fast=*/true);
    imdct512_windowed(coeffs2, x2, /*fast=*/true);
    imdct512_windowed(coeffs3, x3, /*fast=*/true);
}

void mdct512_forward_batch4(std::span<const double, 512> w0, std::span<const double, 512> w1,
                            std::span<const double, 512> w2, std::span<const double, 512> w3,
                            std::span<double, 256> c0, std::span<double, 256> c1,
                            std::span<double, 256> c2, std::span<double, 256> c3) {
    if (internal::cpu::has_avx2()) {
        // Same table-resolution job imdct512_windowed_batch4 does above:
        // FastMdctTables<512> lives in this file's anonymous namespace, so
        // the AVX2 body cannot look it up and takes its four twiddle
        // arrays, the FFT table and dct4_scaled's own -2/NLen scale as
        // plain arguments instead.
        const auto& t = fast_mdct_tables<512>();
        internal::avx2::mdct512_forward_batch4(w0, w1, w2, w3, t.pre_re, t.pre_im, t.post_re,
                                               t.post_im, t.fft, -2.0 / 512.0, c0, c1, c2, c3);
        return;
    }
    mdct512_forward(w0, c0, /*fast=*/true);
    mdct512_forward(w1, c1, /*fast=*/true);
    mdct512_forward(w2, c2, /*fast=*/true);
    mdct512_forward(w3, c3, /*fast=*/true);
}

// See imdct512_windowed_impl above for how the three branches of each
// vectorised section are chosen. This transform has only one such section -
// its pre-twiddle is a scalar loop for every instantiation, because the two
// half-block sets scatter to the same bitrev[k] and neither end is unit
// stride.
template <typename Scalar>
void imdct256_pair_windowed_impl(std::span<const Scalar, 256> coeffs, std::span<Scalar, 512> x,
                                 bool fast) {
    constexpr bool kWide = std::is_same_v<Scalar, double>;
    assert(kWide || fast);
    const auto& tw = twiddles2<Scalar>();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Step 1: de-interleave the 256 coefficients into the two half-block sets.
    std::array<Scalar, kQuarter> x1{};
    std::array<Scalar, kQuarter> x2{};
    for (int k = 0; k < kQuarter; ++k) {
        x1[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k)];
        x2[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k + 1)];
    }

    // Steps 2 and 3: the pre-IFFT complex multiply
    // Z1[k] = (X1[N/4-2k-1] + j*X1[2k]) * (xcos2[k] + j*xsin2[k]) (likewise
    // Z2), then two independent N/8-point complex "IFFT" sums, unscaled.
    // Same inverse-DFT identity as the long transform's step 3 (see
    // imdct512_windowed): the fast path runs conj(FFT(conj(Z))) through the
    // P = 64 kernel tables the short forward folds already own
    // (fast_mdct_tables<256>().fft), once per half-block set, and - as
    // there - writes step 2 already conjugated and already digit-reversed
    // so neither costs a pass of its own. The direct branch keeps the
    // spec's own sum, behind the same src/core/reference_transform.hpp seam
    // as the long form's.
    std::array<Scalar, kEighth> z1_re{};
    std::array<Scalar, kEighth> z1_im{};
    std::array<Scalar, kEighth> z2_re{};
    std::array<Scalar, kEighth> z2_im{};
    std::array<Scalar, kEighth> t1_re{};
    std::array<Scalar, kEighth> t1_im{};
    std::array<Scalar, kEighth> t2_re{};
    std::array<Scalar, kEighth> t2_im{};
    if (fast) {
        const auto& fft = fast_mdct_tables<256, Scalar>().fft;
        for (int k = 0; k < kEighth; ++k) {
            const Scalar c = tw.cos2[static_cast<std::size_t>(k)];
            const Scalar s = tw.sin2[static_cast<std::size_t>(k)];
            const Scalar a1 = x1[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
            const Scalar b1 = x1[static_cast<std::size_t>(2 * k)];
            const Scalar a2 = x2[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
            const Scalar b2 = x2[static_cast<std::size_t>(2 * k)];
            const std::size_t d = fft.bitrev[static_cast<std::size_t>(k)];
            z1_re[d] = a1 * c - b1 * s;
            z1_im[d] = -(b1 * c + a1 * s);
            z2_re[d] = a2 * c - b2 * s;
            z2_im[d] = -(b2 * c + a2 * s);
        }
        internal::fft_forward_bitrev<static_cast<std::size_t>(kEighth), Scalar>(fft, z1_re, z1_im);
        internal::fft_forward_bitrev<static_cast<std::size_t>(kEighth), Scalar>(fft, z2_re, z2_im);
        for (int n = 0; n < kEighth; ++n) {
            t1_re[static_cast<std::size_t>(n)] = z1_re[static_cast<std::size_t>(n)];
            t1_im[static_cast<std::size_t>(n)] = -z1_im[static_cast<std::size_t>(n)];
            t2_re[static_cast<std::size_t>(n)] = z2_re[static_cast<std::size_t>(n)];
            t2_im[static_cast<std::size_t>(n)] = -z2_im[static_cast<std::size_t>(n)];
        }
    } else if constexpr (kWide) {
        // Direct form: double-only, as in the long transform. See there.
        for (int k = 0; k < kEighth; ++k) {
            const double c = tw.cos2[static_cast<std::size_t>(k)];
            const double s = tw.sin2[static_cast<std::size_t>(k)];
            const double a1 = x1[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
            const double b1 = x1[static_cast<std::size_t>(2 * k)];
            z1_re[static_cast<std::size_t>(k)] = a1 * c - b1 * s;
            z1_im[static_cast<std::size_t>(k)] = b1 * c + a1 * s;
            const double a2 = x2[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
            const double b2 = x2[static_cast<std::size_t>(2 * k)];
            z2_re[static_cast<std::size_t>(k)] = a2 * c - b2 * s;
            z2_im[static_cast<std::size_t>(k)] = b2 * c + a2 * s;
        }
        // Two passes over the one table rather than the interleaved loop
        // this used to run: the two sums are independent and each output's
        // accumulation order is unchanged, so every bit is.
        internal::reference_inner_sum_64(z1_re, z1_im, t1_re, t1_im);
        internal::reference_inner_sum_64(z2_re, z2_im, t2_re, t2_im);
    }

    // Step 4: post-IFFT complex multiply. y1[n] = z1[n] * (xcos2[n] + j*xsin2[n]).
    // Both half-block sets: two n at a time through f64x2, four through
    // f32x4 or the AVX2 tier, all unit stride (ROADMAP PF5,
    // ac3::internal::cpu::has_avx2()).
    std::array<Scalar, kEighth> y1_re{};
    std::array<Scalar, kEighth> y1_im{};
    std::array<Scalar, kEighth> y2_re{};
    std::array<Scalar, kEighth> y2_im{};
    if constexpr (!kWide) {
        for (std::size_t n = 0; n < static_cast<std::size_t>(kEighth); n += 4) {
            const auto c = internal::arch::f32x4::load(&tw.cos2[n]);
            const auto sn = internal::arch::f32x4::load(&tw.sin2[n]);
            const auto t1r = internal::arch::f32x4::load(&t1_re[n]);
            const auto t1i = internal::arch::f32x4::load(&t1_im[n]);
            const auto t2r = internal::arch::f32x4::load(&t2_re[n]);
            const auto t2i = internal::arch::f32x4::load(&t2_im[n]);
            (t1r * c - t1i * sn).store(&y1_re[n]);
            (t1i * c + t1r * sn).store(&y1_im[n]);
            (t2r * c - t2i * sn).store(&y2_re[n]);
            (t2i * c + t2r * sn).store(&y2_im[n]);
        }
    } else if (internal::cpu::has_avx2()) {
        internal::avx2::imdct256_post_twiddle(tw.cos2, tw.sin2, t1_re, t1_im, t2_re, t2_im, y1_re,
                                              y1_im, y2_re, y2_im);
    } else {
        for (std::size_t n = 0; n < static_cast<std::size_t>(kEighth); n += 2) {
            const auto c = internal::arch::f64x2::load(&tw.cos2[n]);
            const auto sn = internal::arch::f64x2::load(&tw.sin2[n]);
            const auto t1r = internal::arch::f64x2::load(&t1_re[n]);
            const auto t1i = internal::arch::f64x2::load(&t1_im[n]);
            const auto t2r = internal::arch::f64x2::load(&t2_re[n]);
            const auto t2i = internal::arch::f64x2::load(&t2_im[n]);
            (t1r * c - t1i * sn).store(&y1_re[n]);
            (t1i * c + t1r * sn).store(&y1_im[n]);
            (t2r * c - t2i * sn).store(&y2_re[n]);
            (t2i * c + t2r * sn).store(&y2_im[n]);
        }
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    // N is 512 throughout (the spec's own note), so this reaches the same
    // full x[0..511] the long path's step 5 does.
    const auto& w = analysis_window<Scalar>();
    const auto y1r = [&](int i) { return y1_re[static_cast<std::size_t>(i)]; };
    const auto y1i = [&](int i) { return y1_im[static_cast<std::size_t>(i)]; };
    const auto y2r = [&](int i) { return y2_re[static_cast<std::size_t>(i)]; };
    const auto y2i = [&](int i) { return y2_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -y1i(n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            y1r(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -y1r(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            y1i(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -y2r(n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            y2i(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            y2i(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -y2r(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

void imdct256_pair_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x,
                            bool fast) {
    imdct256_pair_windowed_impl<double>(coeffs, x, fast);
}

// The float32 form. No `fast` parameter, for the reason imdct512_windowed's
// float overload gives.
void imdct256_pair_windowed(std::span<const float, 256> coeffs, std::span<float, 512> x) {
    imdct256_pair_windowed_impl<float>(coeffs, x, true);
}

// The float32 batch forms. Four independent calls, not four lanes.
//
// The double batch kernels exist because four transforms in lockstep keep an
// AVX2 register full where four separate ones do not (ROADMAP PF5's batch
// axis). There is no float32 AVX2 kernel to fill, so batching would buy the
// float path nothing today and would mean a second body to keep agreeing with
// the scalar one. These are here so a caller can be written once against the
// batch shape - joc.cpp's bed analysis and object synthesis both are - and
// pick up a real float32 batch later without changing.
void mdct512_forward_batch4(std::span<const float, 512> w0, std::span<const float, 512> w1,
                            std::span<const float, 512> w2, std::span<const float, 512> w3,
                            std::span<float, 256> c0, std::span<float, 256> c1,
                            std::span<float, 256> c2, std::span<float, 256> c3) {
    mdct512_forward(w0, c0);
    mdct512_forward(w1, c1);
    mdct512_forward(w2, c2);
    mdct512_forward(w3, c3);
}

void imdct512_windowed_batch4(std::span<const float, 256> coeffs0,
                              std::span<const float, 256> coeffs1,
                              std::span<const float, 256> coeffs2,
                              std::span<const float, 256> coeffs3, std::span<float, 512> x0,
                              std::span<float, 512> x1, std::span<float, 512> x2,
                              std::span<float, 512> x3) {
    imdct512_windowed(coeffs0, x0);
    imdct512_windowed(coeffs1, x1);
    imdct512_windowed(coeffs2, x2);
    imdct512_windowed(coeffs3, x3);
}

}  // namespace ac3
