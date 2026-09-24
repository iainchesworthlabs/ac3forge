#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

// A complex FFT for every length of the form 2^a * 3^b * 5^c, which covers
// every transform AC-4 needs: an inverse MDCT of N spectral lines runs an
// N/2-point transform (ETSI TS 103 190-1 V1.4.1 clause 5.5.2, Pseudocode 61),
// and the fifteen block lengths of clause 5.5.3 (2 048 down to 96 at 44.1
// and 48 kHz, twice and four times those at 96 and 192 kHz) put N/2 between
// 48 and 8 192.
//
// Stockham autosort, decimation in frequency: one pass per radix, radix 4
// first, then 2, 3 and 5, with the twiddle factors of every pass computed
// once, in double, when the plan is built. Both directions are unscaled, as
// Pseudocode 61 is: forward is sum_n x[n] e^(-2 pi i kn/L), inverse the same
// with +i.
//
// Written against a scalar type (planning/ac4.md, "Arithmetic"); only double
// is instantiated until the float and fixed-point tiers arrive.

namespace ac4::detail::dsp {

template <typename Real>
class Fft {
   public:
    using Complex = std::complex<Real>;

    // A length with a prime factor above 5, or 0, gives a plan that is not
    // valid() and transforms nothing.
    explicit Fft(std::size_t length);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    // In place. `data` must hold length() values.
    void forward(std::span<Complex> data) { run(data, false); }
    void inverse(std::span<Complex> data) { run(data, true); }

   private:
    struct Stage {
        int radix = 0;
        std::size_t n = 0;       // the sub-transform length this pass splits
        std::size_t stride = 0;  // how many sub-transforms run side by side
        std::size_t twiddle = 0; // offset of this pass's factors in twiddles_
    };

    void run(std::span<Complex> data, bool inverse);

    std::size_t length_ = 0;
    bool valid_ = false;
    std::vector<Stage> stages_;
    // For each pass, w^(p*k) for p < n/radix and k < radix, with w = e^(-2 pi i/n).
    std::vector<Complex> twiddles_;
    // e^(-2 pi i j/3) and e^(-2 pi i j/5), the radix-3 and radix-5 butterflies' roots.
    std::array<Complex, 5> roots3_{};
    std::array<Complex, 5> roots5_{};
    std::vector<Complex> work_;
};

extern template class Fft<double>;

}  // namespace ac4::detail::dsp
