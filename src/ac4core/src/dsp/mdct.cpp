#include "dsp/mdct.hpp"

#include <cmath>
#include <numbers>

namespace ac4::detail::dsp {
namespace {

// xcos1[k] + j xsin1[k] = -cos(2 pi (8k + 1) / 16N) - j sin(2 pi (8k + 1) / 16N),
// Pseudocode 60, for k < N/2.
template <typename Complex>
std::vector<Complex> pre_twiddles(std::size_t length) {
    using Real = typename Complex::value_type;
    std::vector<Complex> twiddle(length / 2);
    const double n16 = 16.0 * static_cast<double>(length);
    for (std::size_t k = 0; k < twiddle.size(); ++k) {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(8 * k + 1) / n16;
        twiddle[k] = Complex(static_cast<Real>(-std::cos(angle)), static_cast<Real>(-std::sin(angle)));
    }
    return twiddle;
}

}  // namespace

template <typename Real>
Imdct<Real>::Imdct(std::size_t length)
    : length_(length), fft_(length / 2), twiddle_(pre_twiddles<Complex>(length)), z_(length / 2) {}

template <typename Real>
void Imdct<Real>::inverse(std::span<const Real> spectrum, std::span<Real> out) {
    const std::size_t n = length_;
    if (!valid() || spectrum.size() != n || out.size() != 2 * n) {
        return;
    }
    const std::size_t half = n / 2;
    const std::size_t quarter = n / 4;

    // Pseudocode 60: Z[k] = (X[N-2k-1] + j X[2k]) (xcos1[k] + j xsin1[k]).
    for (std::size_t k = 0; k < half; ++k) {
        z_[k] = Complex(spectrum[n - 2 * k - 1], spectrum[2 * k]) * twiddle_[k];
    }
    // Pseudocode 61: the unscaled N/2-point inverse transform.
    fft_.inverse(z_);
    // Pseudocode 62: y[n] = z[n] (xcos1[n] + j xsin1[n]) / N.
    const Real scale = Real(1) / static_cast<Real>(n);
    for (std::size_t k = 0; k < half; ++k) {
        z_[k] = z_[k] * twiddle_[k] * scale;
    }
    // Pseudocode 63 without w[n].
    const Complex* y = z_.data();
    for (std::size_t m = 0; m < quarter; ++m) {
        out[2 * m] = y[quarter + m].imag();
        out[2 * m + 1] = -y[quarter - m - 1].real();
        out[half + 2 * m] = y[m].real();
        out[half + 2 * m + 1] = -y[half - m - 1].imag();
        out[n + 2 * m] = y[quarter + m].real();
        out[n + 2 * m + 1] = -y[quarter - m - 1].imag();
        out[n + half + 2 * m] = -y[m].imag();
        out[n + half + 2 * m + 1] = y[half - m - 1].real();
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
        z_[k] *= std::conj(twiddle_[k]);
    }
    fft_.forward(z_);
    for (std::size_t k = 0; k < half; ++k) {
        const Complex u = z_[k] * std::conj(twiddle_[k]);
        spectrum[n - 2 * k - 1] = u.real();
        spectrum[2 * k] = u.imag();
    }
}

template class Imdct<double>;
template class Mdct<double>;

}  // namespace ac4::detail::dsp
