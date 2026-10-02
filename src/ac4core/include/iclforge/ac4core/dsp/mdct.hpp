#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/fft.hpp"

// The MDCT pair of ETSI TS 103 190-1 V1.4.1 clause 5.5.2.
//
// Imdct is the clause's steps 1 to 5 with the window left out: Pseudocode 60's
// pre-twiddle, Pseudocode 61's N/2-point inverse FFT, Pseudocode 62's
// post-twiddle and its 1/N, and Pseudocode 63's unfolding. Written out, the 2N
// samples it gives for N spectral lines X[k] are
//
//   x[n] = (1/N) sum_{k=0..N-1} X[k] cos(pi/N (n + 1/2 + N/2) (k + 1/2)),  n = 0 .. 2N-1,
//
// which the tests hold both the fast path and a verbatim transcription of the
// four pseudocodes to. Windowing and the overlap-add (steps 5 and 6) are the
// synthesis's (dsp/synthesis.hpp), since the windows depend on the lengths of
// neighbouring blocks.
//
// Mdct is the forward transform, the transpose of the same cosine matrix:
//
//   X[k] = sum_{n=0..2N-1} x[n] cos(pi/N (n + 1/2 + N/2) (k + 1/2)),  k = 0 .. N-1,
//
// computed as the adjoint of Imdct's steps in reverse, without the 1/N.
// Through Imdct and a Princen-Bradley window pair the round trip has a gain
// of 1/2. The decoder reads the pseudocode as printed, with its output's full
// scale at 2^15, and the encoder scales its own analysis to match
// (src/ac4dec/ERRATA.md, "Full scale, and the overlap-add's factor of two").
//
// Both need N to be a multiple of 4 whose half is 2^a * 3^b * 5^c, which
// every block length of clause 5.5.3 is.

namespace iclforge::ac4::detail::dsp {

template <typename Real>
class Imdct {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    explicit Imdct(std::size_t length);

    [[nodiscard]] bool valid() const noexcept { return fft_.valid() && length_ % 4 == 0; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    // `spectrum` holds length() lines and `out` 2 * length() samples. The first
    // form works in a buffer of the transform's own, made by the first call; the
    // second in `scratch`, which must hold length() values (N/2 for Pseudocode
    // 60's z[k] and N/2 for the transform's work) and which a caller with several
    // transforms that never run at once can share among them.
    void inverse(std::span<const Real> spectrum, std::span<Real> out);
    void inverse(std::span<const Real> spectrum, std::span<Real> out, std::span<Complex> scratch);

    // At Fixed32 (planning/ac4.md, D14d): `spectrum` holds the lines of the double decoder
    // times 2^-exponent, and `out` receives the samples at the fixed tier's time-domain scale
    // (dsp/scalar_traits.hpp, kTimeShift). The block takes an exponent of its own: its lines
    // are shifted together until the largest is in [4, 8), the transform sheds bits only where
    // a pass could reach the format's edge (Fft::inverse_scaled), and Pseudocode 62's 1/N is a
    // power of two and a factor in [1/2, 1) folded into the post-twiddle. At double and float
    // `exponent` is 0 and this is the form above.
    void inverse(std::span<const Real> spectrum, int exponent, std::span<Real> out,
                 std::span<Complex> scratch);

   private:
    std::size_t length_ = 0;
    Fft<Real> fft_;
    std::vector<Complex> twiddle_;  // xcos1[k] + j xsin1[k], k < N/2
    // At Fixed32 only: twiddle_ times 2^(post_shift_) / N, in [1/2, 1) in magnitude.
    std::vector<Complex> post_twiddle_;
    int post_shift_ = 0;
    std::vector<Complex> scratch_;
};

template <typename Real>
class Mdct {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    explicit Mdct(std::size_t length);

    [[nodiscard]] bool valid() const noexcept { return fft_.valid() && length_ % 4 == 0; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    // `in` holds 2 * length() samples and `spectrum` length() lines.
    void forward(std::span<const Real> in, std::span<Real> spectrum);

   private:
    std::size_t length_ = 0;
    Fft<Real> fft_;
    std::vector<Complex> twiddle_;  // as Imdct's
    std::vector<Complex> z_;
};

extern template class Imdct<Real>;
extern template class Mdct<Real>;

}  // namespace iclforge::ac4::detail::dsp
