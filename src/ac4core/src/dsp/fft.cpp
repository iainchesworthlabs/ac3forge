#include "iclforge/ac4core/dsp/fft.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <complex>
#include <numbers>
#include <utility>

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

template <typename Complex>
Complex times_minus_i(Complex z) {
    return {z.imag(), -z.real()};
}

// One radix-r DFT, b[k] = sum_i a[i] e^(-/+2 pi i ik/r).
template <typename Complex>
void butterfly(int radix, const std::array<Complex, 5>& a, std::array<Complex, 5>& b, bool inverse,
               const std::array<Complex, 5>& roots3, const std::array<Complex, 5>& roots5) {
    switch (radix) {
        case 2:
            b[0] = a[0] + a[1];
            b[1] = a[0] - a[1];
            return;
        case 4: {
            const Complex s02 = a[0] + a[2];
            const Complex d02 = a[0] - a[2];
            const Complex s13 = a[1] + a[3];
            // Forward: (a1 - a3) times -i for b[1]; the inverse takes +i.
            const Complex d13 = inverse ? -times_minus_i(a[1] - a[3]) : times_minus_i(a[1] - a[3]);
            b[0] = s02 + s13;
            b[1] = d02 + d13;
            b[2] = s02 - s13;
            b[3] = d02 - d13;
            return;
        }
        default: {
            const auto& roots = radix == 3 ? roots3 : roots5;
            for (int k = 0; k < radix; ++k) {
                Complex sum = a[0];
                for (int i = 1; i < radix; ++i) {
                    const Complex w = roots[static_cast<std::size_t>((i * k) % radix)];
                    sum += a[static_cast<std::size_t>(i)] * (inverse ? conj(w) : w);
                }
                b[static_cast<std::size_t>(k)] = sum;
            }
            return;
        }
    }
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
    Complex* x = data.data();
    Complex* y = work.data();
    for (const Stage& stage : stages_) {
        const auto r = static_cast<std::size_t>(stage.radix);
        const std::size_t m = stage.n / r;
        const std::size_t s = stage.stride;
        const Complex* tw = twiddles_.data() + stage.twiddle;
        std::array<Complex, 5> a{};
        std::array<Complex, 5> b{};
        for (std::size_t p = 0; p < m; ++p) {
            for (std::size_t q = 0; q < s; ++q) {
                for (std::size_t i = 0; i < r; ++i) {
                    a[i] = x[q + s * (p + i * m)];
                }
                butterfly(stage.radix, a, b, inverse, roots3_, roots5_);
                for (std::size_t k = 0; k < r; ++k) {
                    const Complex w = tw[p * r + k];
                    y[q + s * (r * p + k)] = b[k] * (inverse ? conj(w) : w);
                }
            }
        }
        std::swap(x, y);
    }
    if (x != data.data()) {
        std::copy(x, x + static_cast<std::ptrdiff_t>(length_), data.data());
    }
}

template <typename Real>
int Fft<Real>::inverse_scaled(std::span<Complex> data, std::span<Complex> scratch) {
    if constexpr (std::is_floating_point_v<Real>) {
        run(data, scratch, true);
        return 0;
    } else {
        if (!valid_ || data.size() != length_ || scratch.size() < length_ || stages_.empty()) {
            return 0;
        }
        // 16 in Q7.24: a radix-5 pass takes 16 sqrt(2) to under 114.
        constexpr int kLimitBits = 28;
        int shed = 0;
        Complex* x = data.data();
        Complex* y = scratch.data();
        for (const Stage& stage : stages_) {
            std::uint32_t largest = 0;
            for (std::size_t i = 0; i < length_; ++i) {
                for (const std::int32_t part : {x[i].re.raw, x[i].im.raw}) {
                    const std::uint32_t magnitude =
                        part < 0 ? 0U - static_cast<std::uint32_t>(part) : static_cast<std::uint32_t>(part);
                    largest = magnitude > largest ? magnitude : largest;
                }
            }
            const int bits = 32 - std::countl_zero(largest);
            if (bits > kLimitBits) {
                const int down = bits - kLimitBits;
                for (std::size_t i = 0; i < length_; ++i) {
                    x[i] = Complex{x[i].re.scaled_by_pow2(-down), x[i].im.scaled_by_pow2(-down)};
                }
                shed += down;
            }
            const auto r = static_cast<std::size_t>(stage.radix);
            const std::size_t m = stage.n / r;
            const std::size_t s = stage.stride;
            const Complex* tw = twiddles_.data() + stage.twiddle;
            std::array<Complex, 5> a{};
            std::array<Complex, 5> b{};
            for (std::size_t p = 0; p < m; ++p) {
                for (std::size_t q = 0; q < s; ++q) {
                    for (std::size_t i = 0; i < r; ++i) {
                        a[i] = x[q + s * (p + i * m)];
                    }
                    butterfly(stage.radix, a, b, true, roots3_, roots5_);
                    for (std::size_t k = 0; k < r; ++k) {
                        y[q + s * (r * p + k)] = b[k] * conj(tw[p * r + k]);
                    }
                }
            }
            std::swap(x, y);
        }
        if (x != data.data()) {
            std::copy(x, x + static_cast<std::ptrdiff_t>(length_), data.data());
        }
        return shed;
    }
}

template class Fft<Real>;
// ac4core's own tests (tests/ac4core/test_ac4core_dsp.cpp) exercise Fft at
// double directly, alongside Real (see this target's CMakeLists.txt,
// AC4CORE_ALSO_AT_DOUBLE); Mdct<double>'s own Fft member needs it too.
AC4CORE_ALSO_AT_DOUBLE(template class Fft<double>;)

}  // namespace iclforge::ac4::detail::dsp
