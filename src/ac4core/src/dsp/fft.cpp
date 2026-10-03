#include "iclforge/ac4core/dsp/fft.hpp"

#include "iclforge/ac4core/dsp/transform_tables.hpp"

#include <algorithm>
#include <type_traits>
#include <cstdint>
#include <bit>
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
std::vector<typename Fft<Real>::Complex> Fft<Real>::computed_roots(std::size_t length) {
    std::vector<Complex> roots;
    bool ok = false;
    const std::vector<int> radices = factor(length, ok);
    std::size_t n = length;
    for (const int radix : radices) {
        const auto r = static_cast<std::size_t>(radix);
        const std::size_t m = n / r;
        for (std::size_t p = 0; p < m; ++p) {
            for (std::size_t k = 0; k < r; ++k) {
                const std::complex<double> w = root(p * k, n);
                roots.emplace_back(static_cast<Real>(w.real()), static_cast<Real>(w.imag()));
            }
        }
        n = m;
    }
    return roots;
}

template <typename Real>
Fft<Real>::Fft(std::size_t length) : length_(length) {
    const std::vector<int> radices = factor(length, valid_);
    if (!valid_) {
        return;
    }
    std::size_t n = length;
    std::size_t stride = 1;
    std::size_t count = 0;
    for (const int radix : radices) {
        const auto r = static_cast<std::size_t>(radix);
        const std::size_t m = n / r;
        stages_.push_back(Stage{radix, n, stride, count});
        count += m * r;
        n = m;
        stride *= r;
    }
    // An inverse transform of 2 * length lines has its roots in flash at the float and fixed
    // tiers (dsp/transform_tables.hpp), the same values computed_roots() gives.
    const TransformTable<Real>* const table = transform_table<Real>(2 * length);
    if (table != nullptr && table->fft_roots.size() == count) {
        built_in_ = table->fft_roots.data();
    } else {
        twiddles_ = computed_roots(length);
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
            ? fft_kernels::run_stages<Real, true>(stages_.data(), stages_.size(), twiddles(),
                                                  roots3_.data(), roots5_.data(), first,
                                                  data.data(), work.data())
            : fft_kernels::run_stages<Real, false>(stages_.data(), stages_.size(), twiddles(),
                                                   roots3_.data(), roots5_.data(), first,
                                                   data.data(), work.data());
    if (result != data.data()) {
        std::copy(result, result + static_cast<std::ptrdiff_t>(length_), data.data());
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
            fft_kernels::pass_from_array<Real, true>(stage, x, y, twiddles(), roots3_.data(),
                                                     roots5_.data());
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
