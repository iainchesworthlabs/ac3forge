#include "iclforge/ac4core/dsp/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace iclforge::ac4::detail::dsp {
namespace {

// The radices that factor `length`, radix 4 first. Empty, with `ok` false,
// when a prime above 5 divides it.
std::vector<int> factor(std::size_t length, bool& ok) {
    std::vector<int> radices;
    ok = length > 0;
    if (!ok) {
        return radices;
    }
    for (const int radix : {4, 2, 3, 5}) {
        const auto r = static_cast<std::size_t>(radix);
        while (length % r == 0) {
            radices.push_back(radix);
            length /= r;
        }
    }
    ok = length == 1;
    if (!ok) {
        radices.clear();
    }
    return radices;
}

// e^(-2 pi i num/den), with the angle reduced first so that a large product
// p*k loses no precision.
std::complex<double> root(std::size_t num, std::size_t den) {
    const double angle = -2.0 * std::numbers::pi * static_cast<double>(num % den) / static_cast<double>(den);
    return {std::cos(angle), std::sin(angle)};
}

}  // namespace

template <typename Real>
Fft<Real>::Fft(std::size_t length) : length_(length) {
    const std::vector<int> radices = factor(length, valid_);
    if (!valid_) {
        return;
    }
    std::size_t n = length;
    std::size_t stride = 1;
    for (const int radix : radices) {
        const auto r = static_cast<std::size_t>(radix);
        const std::size_t m = n / r;
        stages_.push_back(Stage{radix, n, stride, twiddles_.size()});
        for (std::size_t p = 0; p < m; ++p) {
            for (std::size_t k = 0; k < r; ++k) {
                const std::complex<double> w = root(p * k, n);
                twiddles_.emplace_back(static_cast<Real>(w.real()), static_cast<Real>(w.imag()));
            }
        }
        n = m;
        stride *= r;
    }
    for (std::size_t j = 0; j < 5; ++j) {
        const std::complex<double> w3 = root(j, 3);
        const std::complex<double> w5 = root(j, 5);
        roots3_[j] = Complex(static_cast<Real>(w3.real()), static_cast<Real>(w3.imag()));
        roots5_[j] = Complex(static_cast<Real>(w5.real()), static_cast<Real>(w5.imag()));
    }
}

template <typename Real>
void Fft<Real>::run(std::span<Complex> data, std::span<Complex> work, bool inverse) {
    if (!valid_ || data.size() != length_ || work.size() < length_ || stages_.empty()) {
        return;
    }
    const Complex* const x = data.data();
    const auto first = [x](std::size_t index) noexcept { return x[index]; };
    // The first pass reads `data` and writes `work`, the second reads `work` and writes `data`, and
    // so on.
    Complex* const result =
        inverse
            ? fft_kernels::run_stages<Real, true>(stages_.data(), stages_.size(), twiddles_.data(),
                                                  roots3_.data(), roots5_.data(), first,
                                                  data.data(), work.data())
            : fft_kernels::run_stages<Real, false>(stages_.data(), stages_.size(), twiddles_.data(),
                                                   roots3_.data(), roots5_.data(), first,
                                                   data.data(), work.data());
    if (result != data.data()) {
        std::copy(result, result + static_cast<std::ptrdiff_t>(length_), data.data());
    }
}

template class Fft<Real>;
// ac4core's own tests (tests/ac4core/test_ac4core_dsp.cpp) exercise Fft at
// double directly, alongside Real (see this target's CMakeLists.txt,
// AC4CORE_ALSO_AT_DOUBLE); Mdct<double>'s own Fft member needs it too.
AC4CORE_ALSO_AT_DOUBLE(template class Fft<double>;)

}  // namespace iclforge::ac4::detail::dsp
