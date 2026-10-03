#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/fft_kernels.hpp"

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
// with +i. The passes are dsp/fft_kernels.hpp's, one function for each radix
// and direction.
//
// Written against a scalar type (planning/ac4.md, "Arithmetic"); only double
// is instantiated until the float and fixed-point tiers arrive.

namespace iclforge::ac4::detail::dsp {

template <typename Real>
class Fft {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    // A length with a prime factor above 5, or 0, gives a plan that is not
    // valid() and transforms nothing.
    explicit Fft(std::size_t length);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    // In place. `data` must hold length() values. The first two forms work in a
    // buffer of the plan's own, made by the first call; the others work in
    // `scratch`, which must hold length() values and which the caller can share
    // among plans that never run at once, so the plan holds none.
    void forward(std::span<Complex> data) { run(data, own_work(), false); }
    void inverse(std::span<Complex> data) { run(data, own_work(), true); }
    void forward(std::span<Complex> data, std::span<Complex> scratch) { run(data, scratch, false); }
    void inverse(std::span<Complex> data, std::span<Complex> scratch) { run(data, scratch, true); }

    // The plan's passes and factors, for a caller that supplies the first pass's input
    // itself (Imdct's fused pre-twiddle, dsp/mdct.hpp) and runs fft_kernels::run_stages.
    struct View {
        const fft_kernels::Stage* stages = nullptr;
        std::size_t count = 0;
        const Complex* twiddles = nullptr;
        const Complex* roots3 = nullptr;
        const Complex* roots5 = nullptr;
    };
    [[nodiscard]] View view() const noexcept {
        return View{stages_.data(), stages_.size(), twiddles(), roots3_.data(), roots5_.data()};
    }

    // Every pass's roots, in the plan's order, as the constructor computes them where no table
    // is built in (dsp/transform_tables.hpp), which tests/ac4core/test_ac4core_transform_tables.cpp
    // holds the built-in ones to.
    [[nodiscard]] static std::vector<Complex> computed_roots(std::size_t length);

    // The inverse transform at a scalar that is not floating (Fixed32): before each pass, if
    // the largest real or imaginary part is 16 or more, every value is shifted down together
    // until it is not, so that no pass, which grows a value at most by its radix, reaches the
    // format's 128. Returns the bits shed: the result is the transform times 2^-shift.
    [[nodiscard]] int inverse_scaled(std::span<Complex> data, std::span<Complex> scratch);

   private:
    using Stage = fft_kernels::Stage;

    [[nodiscard]] std::span<Complex> own_work() {
        if (work_.size() != length_) {
            work_.resize(length_);
        }
        return work_;
    }
    void run(std::span<Complex> data, std::span<Complex> work, bool inverse);
    [[nodiscard]] const Complex* twiddles() const noexcept {
        return built_in_ != nullptr ? built_in_ : twiddles_.data();
    }

    std::size_t length_ = 0;
    bool valid_ = false;
    std::vector<Stage> stages_;
    // For each pass, w^(p*k) for p < n/radix and k < radix, with w = e^(-2 pi i/n): the
    // built-in table's where there is one, else computed into twiddles_.
    const Complex* built_in_ = nullptr;
    std::vector<Complex> twiddles_;
    // e^(-2 pi i j/3) and e^(-2 pi i j/5), the radix-3 and radix-5 butterflies' roots.
    std::array<Complex, 5> roots3_{};
    std::array<Complex, 5> roots5_{};
    std::vector<Complex> work_;
};

extern template class Fft<Real>;

}  // namespace iclforge::ac4::detail::dsp
