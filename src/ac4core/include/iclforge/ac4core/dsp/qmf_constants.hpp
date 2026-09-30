#pragma once

#include <array>
#include <cstddef>

#include "iclforge/ac4core/tables/qmf_twiddles.hpp"

// The constants of the QMF banks (dsp/qmf.hpp) and their 64-point transform,
// built at compile time from tables/qmf_twiddles.hpp so that they sit in the
// program's read-only data and every bank shares one copy: a bank holds only its
// delay line.
//
// Every rotation is a whole number of units of pi / 256. cos_units() and
// sin_units() reduce an integer number of units to the first quadrant, where the
// table is, so each constant is the Real nearest a single exact value: a product
// of two rounded twiddle factors is never used where one angle will do. Scale
// factors that are powers of two (the 1 / 2 and 1 / 128 below) are exact.

namespace iclforge::ac4::detail::dsp::qmf {

// cos(pi * units / 256) for any integer `units`.
[[nodiscard]] constexpr double cos_units(long long units) noexcept {
    long long j = units % 512;  // 512 units make a full turn
    if (j < 0) {
        j += 512;
    }
    if (j > 256) {  // cos(2 pi - x) = cos(x)
        j = 512 - j;
    }
    if (j > 128) {  // cos(pi - x) = -cos(x)
        return -tables::kCosQuadrant[static_cast<std::size_t>(256 - j)];
    }
    return tables::kCosQuadrant[static_cast<std::size_t>(j)];
}

// sin(pi * units / 256) = cos(pi * (units - 128) / 256).
[[nodiscard]] constexpr double sin_units(long long units) noexcept {
    return cos_units(units - 128);
}

// The analysis of Pseudocode 65 and the synthesis of Pseudocode 66, each as one
// 64-point complex transform between a rotation that packs the real samples in
// pairs and one that unpacks them (dsp/qmf.hpp derives both). Planes hold real
// and imaginary parts apart, in the order the kernels read them.
template <typename Real>
struct Constants {
    // The transform, e^(+2 pi i n / 64) unscaled. First pass: e^(2 pi i p k / 64)
    // for k = 1 to 3 and p = 0 to 15, at [(k - 1) * 16 + p]; second pass:
    // e^(2 pi i p k / 16) for k = 1 to 3 and p = 0 to 3, at [(k - 1) * 4 + p].
    // The third pass has no factors.
    std::array<Real, 48> fft1_re{};
    std::array<Real, 48> fft1_im{};
    std::array<Real, 12> fft2_re{};
    std::array<Real, 12> fft2_im{};
    // e^(i pi m / 64), m = 0 to 63: the analysis's rotation before the transform
    // and the synthesis's after it.
    std::array<Real, 64> rot_re{};
    std::array<Real, 64> rot_im{};
    // The analysis's unpacking: cos and sin of pi (2k + 1) / 256, halved, k = 0 to 31.
    std::array<Real, 32> post_cos{};
    std::array<Real, 32> post_sin{};
    // The synthesis's packing: e^(-i pi (2k + 1) 255 / 256) and
    // e^(-i pi (2k + 1) 253 / 256), each over 128 (the 1 / 64 of Pseudocode 66 and
    // the 1 / 2 of the unpacking), k = 0 to 31.
    std::array<Real, 32> pre_re{};
    std::array<Real, 32> pre_im{};
    std::array<Real, 32> prho_re{};
    std::array<Real, 32> prho_im{};
};

template <typename Real>
[[nodiscard]] consteval Constants<Real> make_constants() {
    Constants<Real> c{};
    for (std::size_t p = 0; p < 16; ++p) {
        for (std::size_t k = 1; k <= 3; ++k) {
            const auto units = static_cast<long long>(8 * p * k);
            c.fft1_re[(k - 1) * 16 + p] = static_cast<Real>(cos_units(units));
            c.fft1_im[(k - 1) * 16 + p] = static_cast<Real>(sin_units(units));
        }
    }
    for (std::size_t p = 0; p < 4; ++p) {
        for (std::size_t k = 1; k <= 3; ++k) {
            const auto units = static_cast<long long>(32 * p * k);
            c.fft2_re[(k - 1) * 4 + p] = static_cast<Real>(cos_units(units));
            c.fft2_im[(k - 1) * 4 + p] = static_cast<Real>(sin_units(units));
        }
    }
    for (std::size_t m = 0; m < 64; ++m) {
        const auto units = static_cast<long long>(4 * m);
        c.rot_re[m] = static_cast<Real>(cos_units(units));
        c.rot_im[m] = static_cast<Real>(sin_units(units));
    }
    for (std::size_t k = 0; k < 32; ++k) {
        const auto odd = static_cast<long long>(2 * k + 1);
        c.post_cos[k] = static_cast<Real>(cos_units(odd) / 2.0);
        c.post_sin[k] = static_cast<Real>(sin_units(odd) / 2.0);
        c.pre_re[k] = static_cast<Real>(cos_units(-255 * odd) / 128.0);
        c.pre_im[k] = static_cast<Real>(sin_units(-255 * odd) / 128.0);
        c.prho_re[k] = static_cast<Real>(cos_units(-253 * odd) / 128.0);
        c.prho_im[k] = static_cast<Real>(sin_units(-253 * odd) / 128.0);
    }
    return c;
}

template <typename Real>
inline constexpr Constants<Real> kConstants = make_constants<Real>();

}  // namespace iclforge::ac4::detail::dsp::qmf
