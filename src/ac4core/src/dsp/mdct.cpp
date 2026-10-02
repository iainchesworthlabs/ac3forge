#include "iclforge/ac4core/dsp/mdct.hpp"

#include <cmath>
#include <numbers>

namespace iclforge::ac4::detail::dsp {
namespace {

// xcos1[k] + j xsin1[k] = -cos(2 pi (8k + 1) / 16N) - j sin(2 pi (8k + 1) / 16N),
// Pseudocode 60, for k < N/2.
template <typename Complex>
std::vector<Complex> pre_twiddles(std::size_t length) {
    // Named Scalar, not Real: AC4CORE_ALSO_AT_DOUBLE (this target's CMakeLists.txt)
    // explicitly instantiates Mdct/Imdct<double> alongside <Real> in a float
    // build, and MSVC's /W4 flags a local alias named Real that resolves to a
    // different type than the enclosing iclforge::ac4::detail::Real as hiding it
    // (C4459), which -WX then makes an error.
    using Scalar = typename Complex::value_type;
    std::vector<Complex> twiddle(length / 2);
    const double n16 = 16.0 * static_cast<double>(length);
    for (std::size_t k = 0; k < twiddle.size(); ++k) {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(8 * k + 1) / n16;
        twiddle[k] =
            Complex(static_cast<Scalar>(-std::cos(angle)), static_cast<Scalar>(-std::sin(angle)));
    }
    return twiddle;
}

}  // namespace

template <typename Real>
Imdct<Real>::Imdct(std::size_t length)
    : length_(length), fft_(length / 2), twiddle_(pre_twiddles<Complex>(length)) {}

template <typename Real>
void Imdct<Real>::inverse(std::span<const Real> spectrum, std::span<Real> out) {
    if (scratch_.size() != length_) {
        scratch_.resize(length_);
    }
    inverse(spectrum, out, scratch_);
}

template <typename Real>
typename Imdct<Real>::Complex* Imdct<Real>::transform(std::span<const Real> spectrum,
                                                      std::span<Complex> scratch) const {
    const std::size_t n = length_;
    const std::size_t half = n / 2;
    const auto view = fft_.view();
    if (view.count == 0) {
        return nullptr;
    }
    // Pseudocode 60: Z[k] = (X[N-2k-1] + j X[2k]) (xcos1[k] + j xsin1[k]), the first pass's
    // input at index k.
    const Real* const x = spectrum.data();
    const Complex* const tw = twiddle_.data();
    const auto pretwiddled = [x, tw, n](std::size_t k) noexcept {
        return Complex(x[n - 2 * k - 1], x[2 * k]) * tw[k];
    };
    // Pseudocode 61: the unscaled N/2-point inverse transform. The first pass writes the second
    // half of the scratch and the next the first, and so on.
    return fft_kernels::run_stages<Real, true>(view.stages, view.count, view.twiddles, view.roots3, view.roots5,
                                               pretwiddled, scratch.data(), scratch.data() + half);
}

template <typename Real>
void Imdct<Real>::inverse(std::span<const Real> spectrum, std::span<Real> out,
                          std::span<Complex> scratch) {
    const std::size_t n = length_;
    if (!valid() || spectrum.size() != n || out.size() != 2 * n || scratch.size() < n) {
        return;
    }
    const std::size_t half = n / 2;
    const std::size_t quarter = n / 4;
    const Complex* const z = transform(spectrum, scratch);
    if (z == nullptr) {
        return;
    }
    const Complex* const tw = twiddle_.data();
    // Pseudocode 62: y[n] = z[n] (xcos1[n] + j xsin1[n]) / N.
    const Real scale = Real(1) / static_cast<Real>(n);
    const auto post = [z, tw, scale](std::size_t k) noexcept { return z[k] * tw[k] * scale; };
    // Pseudocode 63 without w[n].
    for (std::size_t m = 0; m < quarter; ++m) {
        const Complex a = post(quarter + m);
        const Complex b = post(quarter - m - 1);
        const Complex c = post(m);
        const Complex d = post(half - m - 1);
        out[2 * m] = a.imag();
        out[2 * m + 1] = -b.real();
        out[half + 2 * m] = c.real();
        out[half + 2 * m + 1] = -d.imag();
        out[n + 2 * m] = a.real();
        out[n + 2 * m + 1] = -b.imag();
        out[n + half + 2 * m] = -c.imag();
        out[n + half + 2 * m + 1] = d.real();
    }
}

template <typename Real>
void Imdct<Real>::inverse_overlap(std::span<const Real> spectrum, std::span<const Real> kbd,
                                  std::span<Real> overlap, std::span<Real> pcm, std::span<Complex> scratch) {
    const std::size_t n = length_;
    if (!valid() || spectrum.size() != n || kbd.size() != n || overlap.size() < n || pcm.size() < n ||
        scratch.size() < n) {
        return;
    }
    const std::size_t half = n / 2;
    const std::size_t quarter = n / 4;
    const Complex* const z = transform(spectrum, scratch);
    if (z == nullptr) {
        return;
    }
    const Complex* const tw = twiddle_.data();
    const Real scale = Real(1) / static_cast<Real>(n);
    const auto post = [z, tw, scale](std::size_t k) noexcept { return z[k] * tw[k] * scale; };
    const Real* const window = kbd.data();
    Real* const lap = overlap.data();
    Real* const out = pcm.data();
    // A sample of the first half of the block, windowed, added to the previous block's second half,
    // windowed in its turn, and the sample of this block's second half that waits for the next.
    const auto lapped = [window, lap, out, n](std::size_t i, Real first, Real second) noexcept {
        out[i] = lap[i] * window[n - 1 - i] + first * window[i];
        lap[i] = second;
    };
    for (std::size_t m = 0; m < quarter; ++m) {
        const Complex a = post(quarter + m);
        const Complex b = post(quarter - m - 1);
        const Complex c = post(m);
        const Complex d = post(half - m - 1);
        lapped(2 * m, a.imag(), a.real());
        lapped(2 * m + 1, -b.real(), -b.imag());
        lapped(half + 2 * m, c.real(), -c.imag());
        lapped(half + 2 * m + 1, -d.imag(), d.real());
    }
}

template <typename Real>
Mdct<Real>::Mdct(std::size_t length)
    : length_(length), fft_(length / 2), twiddle_(pre_twiddles<Complex>(length)), z_(length / 2) {}

template <typename Real>
void Mdct<Real>::forward(std::span<const Real> in, std::span<Real> spectrum) {
    const std::size_t n = length_;
    if (!valid() || in.size() != 2 * n || spectrum.size() != n) {
        return;
    }
    const std::size_t half = n / 2;
    const std::size_t quarter = n / 4;

    // The transpose of Pseudocode 63's unfolding: each y[m] gathers the two
    // samples, one from each half of the block, that the inverse writes from it.
    for (std::size_t k = 0; k < half; ++k) {
        z_[k] = Complex(Real(0), Real(0));
    }
    for (std::size_t m = 0; m < quarter; ++m) {
        z_[quarter + m] += Complex(in[n + 2 * m], in[2 * m]);
        z_[quarter - m - 1] -= Complex(in[2 * m + 1], in[n + 2 * m + 1]);
        z_[m] += Complex(in[half + 2 * m], -in[n + half + 2 * m]);
        z_[half - m - 1] += Complex(in[n + half + 2 * m + 1], -in[half + 2 * m + 1]);
    }
    // The transposes, in reverse order, of Pseudocode 62's twiddle (its
    // conjugate), Pseudocode 61's inverse transform (the forward one) and
    // Pseudocode 60's twiddle and packing.
    for (std::size_t k = 0; k < half; ++k) {
        z_[k] *= conj(twiddle_[k]);
    }
    fft_.forward(z_);
    for (std::size_t k = 0; k < half; ++k) {
        const Complex u = z_[k] * conj(twiddle_[k]);
        spectrum[n - 2 * k - 1] = u.real();
        spectrum[2 * k] = u.imag();
    }
}

template class Imdct<Real>;
template class Mdct<Real>;
// The encoder's forward transform (src/ac4enc/src/frame/analysis.cpp,
// psycho.cpp) calls Mdct at double regardless of the decoder's scalar, and
// ac4core's own tests (tests/ac4core/test_ac4core_dsp.cpp) exercise both
// Mdct and Imdct at double directly, alongside Real, to check the pseudocode
// at the scalar the double build's own reference always uses (see this
// target's CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE).
AC4CORE_ALSO_AT_DOUBLE(
    template class Imdct<double>;
    template class Mdct<double>;)

}  // namespace iclforge::ac4::detail::dsp
