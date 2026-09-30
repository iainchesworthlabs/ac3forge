#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

// |q|^(4/3), the magnitude of the dequantised spectral line of ETSI TS 103 190-1
// V1.4.1 clause 5.1.3.2, for every |q| the syntax can send: 8 191, the most the
// 21 bits of ext_code (Pseudocode 20) reach. A table in the program's read-only
// data, 64 KiB at double and 32 KiB at float, built at compile time: the values
// do not depend on the C library's pow(), so every build and every target has
// the same ones, and no start-up code fills them in RAM.
//
// |q|^(4/3) is the cube root of |q|^4, which for |q| <= 8 191 is an integer
// below 2^52, exact in a double. pow43_exact() takes that root by Newton's method
// in double and one step more with its residual in double-double arithmetic, so
// the result is the double nearest the exact value (its error before the last
// rounding is about 1e-32 relative), and an exact cube's fourth power comes out
// exactly. pow(m, 4.0 / 3.0) is not that value: 4.0 / 3.0 is 4/3 less 7.4e-17,
// which moves the result by up to 6.7e-16 relative at 8 191.

namespace iclforge::ac4::detail {

inline constexpr std::size_t kMaxQuant = 8191;
inline constexpr std::size_t kPow43Entries = kMaxQuant + 1;

namespace pow43_detail {

struct DoubleDouble {
    double hi = 0.0;
    double lo = 0.0;
};

// Veltkamp's split of a double into two halves of 26 bits, each exact.
[[nodiscard]] constexpr DoubleDouble split(double a) noexcept {
    const double t = 134217729.0 * a;  // 2^27 + 1
    const double hi = t - (t - a);
    return {hi, a - hi};
}

// a * b as the pair (the rounded product, its error), exactly (Dekker), without
// asking for a fused multiply-add.
[[nodiscard]] constexpr DoubleDouble two_prod(double a, double b) noexcept {
    const double p = a * b;
    const DoubleDouble x = split(a);
    const DoubleDouble y = split(b);
    return {p, ((x.hi * y.hi - p) + x.hi * y.lo + x.lo * y.hi) + x.lo * y.lo};
}

}  // namespace pow43_detail

// m^(4/3) for a whole m from 0 to kMaxQuant: the double nearest the exact value.
[[nodiscard]] constexpr double pow43_exact(std::uint32_t m) noexcept {
    if (m == 0) {
        return 0.0;
    }
    const auto x = static_cast<double>(m);
    const double a = x * x * x * x;  // exact: below 2^52
    // A start at or above the root, less than twice it, then Newton's steps, which
    // come down on the root from there: y = (2 y + a / y^2) / 3.
    double y = 1.0;
    while (y * y * y < a) {
        y *= 2.0;
    }
    for (int i = 0; i < 8; ++i) {
        y = (2.0 * y + a / (y * y)) / 3.0;
    }
    // y is within an ulp of the root. The residual y^3 - a, with y^3 as the sum
    // of three parts that are each exact, gives the last correction.
    const pow43_detail::DoubleDouble y2 = pow43_detail::two_prod(y, y);
    const pow43_detail::DoubleDouble y3 = pow43_detail::two_prod(y2.hi, y);
    const double residual = ((y3.hi - a) + y3.lo) + y2.lo * y;
    return y - residual / (3.0 * y * y);
}

inline constexpr std::size_t kPow43Block = 1024;

// One block of the table by itself, so that no single constant evaluation is long.
template <typename Real>
[[nodiscard]] constexpr std::array<Real, kPow43Block> pow43_block(std::size_t block) noexcept {
    std::array<Real, kPow43Block> part{};
    for (std::size_t i = 0; i < part.size(); ++i) {
        part[i] =
            static_cast<Real>(pow43_exact(static_cast<std::uint32_t>(block * kPow43Block + i)));
    }
    return part;
}

template <typename Real, std::size_t Block>
inline constexpr std::array<Real, kPow43Block> kPow43Part = pow43_block<Real>(Block);

template <typename Real, std::size_t... Blocks>
[[nodiscard]] constexpr std::array<Real, kPow43Entries> join_pow43(
    std::index_sequence<Blocks...>) noexcept {
    std::array<Real, kPow43Entries> table{};
    (std::copy(kPow43Part<Real, Blocks>.begin(), kPow43Part<Real, Blocks>.end(),
               table.begin() + static_cast<std::ptrdiff_t>(Blocks * kPow43Block)),
     ...);
    return table;
}

// kPow43<Real>[m] is m^(4/3) at the decoder's scalar, for m from 0 to kMaxQuant.
template <typename Real>
inline constexpr std::array<Real, kPow43Entries> kPow43 =
    join_pow43<Real>(std::make_index_sequence<kPow43Entries / kPow43Block>{});

}  // namespace iclforge::ac4::detail
