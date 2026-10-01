#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <numbers>

// The mathematical functions the sample rate converter's filter is designed with
// (dsp/resampler_design.hpp), written for the compiler's constant evaluator and for every platform.
//
// The standard's <cmath> functions are not constexpr before C++26, and a compiler that evaluates
// them at compile time does it with an extension of its own, and with the C library of the machine
// it runs on. These are the same functions in plain IEEE 754 double arithmetic, a series or an
// iteration each and no library call: the compiler's evaluator, an x86-64 host, a Cortex-M3 and an
// ESP32's soft-float double run the same additions, subtractions, multiplications and divisions in
// the same order and give the same bits (the build pins -ffp-contract=off, so no multiply and add
// is fused). tests/ac4core/test_ac4core_portable_math.cpp checks each against its definition and
// against the C library's, and a function's value at compile time against its value at run time.
//
// ceil and sqrt give what the standard's give, the sqrt the double nearest the root as IEEE 754
// asks of the library's. sin and cos keep to the library's values within three units in the last
// place, which is far finer than the float table the converter keeps (a rounding of 2^-24): the
// table is the one thing that has to be the same everywhere, and a float coefficient has the same
// bits however the last bit of its double falls, except where that value lies within it of a
// rounding boundary of float, about one in 2^29.

namespace iclforge::ac4::detail::dsp::portable {

// The smallest whole number not below x, for a finite x of magnitude below 2^62.
[[nodiscard]] constexpr double ceil(double x) noexcept {
    const auto whole = static_cast<double>(static_cast<std::int64_t>(x));  // toward zero
    return whole < x ? whole + 1.0 : whole;
}

// The square root of x, for x of 2^-1000 up to 2^1000; 0 for any x that is not positive.
//
// The bits of a double are close to its logarithm, so halving them, with half the exponent's bias
// put back, estimates the root to six per cent. Heron's iteration y = (y + x / y) / 2 squares the
// relative error at each step, and three leave 1e-12. A last step of Newton's method on the
// residual x - y * y, which is computed exactly by splitting y into two halves of 26 bits (Dekker's
// product: the partial products of the halves fit a double), squares it again and gives the double
// nearest the root. The arguments one unit below a power of four are the ones this cannot place:
// the double below a power of two is half as far away as the one above it, so their roots lie
// within 2^-56 of a unit in the last place of a midpoint between two doubles, and the sign of the
// residual is what says on which side.
[[nodiscard]] constexpr double sqrt(double x) noexcept {
    if (!(x > 0.0)) {
        return 0.0;
    }
    double y =
        std::bit_cast<double>((std::bit_cast<std::uint64_t>(x) >> 1U) + 0x1FF8000000000000ULL);
    for (int step = 0; step < 3; ++step) {
        y = 0.5 * (y + x / y);
    }
    constexpr double kSplit = 134217729.0;  // 2^27 + 1
    const double scaled = kSplit * y;
    const double high = scaled - (scaled - y);
    const double low = y - high;
    const double product = y * y;
    const double product_error = ((high * high - product) + 2.0 * high * low) + low * low;
    const double residual = (x - product) - product_error;
    const double root = y + residual / (2.0 * y);
    // A power of two above the root: the root of any x below its square lies below the midpoint
    // between it and the double under it, which is the nearest.
    const auto bits = std::bit_cast<std::uint64_t>(root);
    if ((bits << 12U) == 0U && x < root * root) {
        return std::bit_cast<double>(bits - 1U);
    }
    return root;
}

namespace detail {

// pi / 2 in three pieces, for the reduction of an argument by multiples of it. The first holds the
// 33 leading bits of the double nearest pi / 2, the second the 20 bits that follow, the third is
// pi / 2 less the double nearest it, 6.1232339957367659e-17 (from the digits of pi). A multiple n
// of the first two is exact for n below 2^20, so an argument has them taken off without a rounding;
// the third, a product that rounds, and the third's own rounding leave an error of 1e-26 at n =
// 2^20 and of 1e-30 at the n of this converter's design, where an argument is at most 140.
inline constexpr double kHalfPi = std::numbers::pi / 2.0;
inline constexpr double kHalfPiFirst = std::bit_cast<double>(std::bit_cast<std::uint64_t>(kHalfPi) &
                                                             ~((std::uint64_t{1} << 20U) - 1U));
inline constexpr double kHalfPiSecond = kHalfPi - kHalfPiFirst;
inline constexpr double kHalfPiThird = 6.123233995736765886130329661375e-17;
inline constexpr double kTwoOverPi = 0.63661977236758134307553505349006;

// sin(r) and cos(r) for |r| up to a little over pi / 4, as their Taylor series to r^19 and r^18, in
// Horner's form in r^2. The terms left out are below 2^-70 of the sum there, and what is kept is
// added from the smallest to the largest so that the leading 1 and r are the last additions.
[[nodiscard]] constexpr double sin_kernel(double r) noexcept {
    const double z = r * r;
    double p = -1.0 / 121645100408832000.0;  // -1 / 19!
    p = p * z + 1.0 / 355687428096000.0;     // 1 / 17!
    p = p * z - 1.0 / 1307674368000.0;       // 1 / 15!
    p = p * z + 1.0 / 6227020800.0;          // 1 / 13!
    p = p * z - 1.0 / 39916800.0;            // 1 / 11!
    p = p * z + 1.0 / 362880.0;              // 1 / 9!
    p = p * z - 1.0 / 5040.0;                // 1 / 7!
    p = p * z + 1.0 / 120.0;                 // 1 / 5!
    p = p * z - 1.0 / 6.0;                   // 1 / 3!
    return r + (r * z) * p;
}

[[nodiscard]] constexpr double cos_kernel(double r) noexcept {
    const double z = r * r;
    double p = -1.0 / 6402373705728000.0;  // -1 / 18!
    p = p * z + 1.0 / 20922789888000.0;    // 1 / 16!
    p = p * z - 1.0 / 87178291200.0;       // 1 / 14!
    p = p * z + 1.0 / 479001600.0;         // 1 / 12!
    p = p * z - 1.0 / 3628800.0;           // 1 / 10!
    p = p * z + 1.0 / 40320.0;             // 1 / 8!
    p = p * z - 1.0 / 720.0;               // 1 / 6!
    p = p * z + 1.0 / 24.0;                // 1 / 4!
    p = p * z - 0.5;                       // 1 / 2!
    return 1.0 + z * p;
}

// x = n pi / 2 + r with n whole and |r| at most a little over pi / 4. Out of range where an
// argument that large has fewer bits left than the reduction is exact to.
inline constexpr double kReduceLimit = 1048576.0;  // 2^20

struct Reduced {
    std::int64_t quadrant;  // n modulo 4
    double r;
};

[[nodiscard]] constexpr Reduced reduce(double x) noexcept {
    const double scaled = x * kTwoOverPi;
    const auto n = static_cast<std::int64_t>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
    const auto whole = static_cast<double>(n);
    const double r = ((x - whole * kHalfPiFirst) - whole * kHalfPiSecond) - whole * kHalfPiThird;
    return {n & 3, r};
}

}  // namespace detail

// sin(x) for |x| up to 2^20; NaN beyond that and for a NaN.
//
// The argument is taken to the nearest multiple n of pi / 2 and what is left, r, by a Cody-Waite
// reduction with pi / 2 in three pieces (above), where the first two multiply exactly; sin(x) is
// then +-sin(r) or +-cos(r) as n modulo 4 says, each a short series.
[[nodiscard]] constexpr double sin(double x) noexcept {
    if (!(x >= -detail::kReduceLimit && x <= detail::kReduceLimit)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const detail::Reduced reduced = detail::reduce(x);
    switch (reduced.quadrant) {
        case 0:
            return detail::sin_kernel(reduced.r);
        case 1:
            return detail::cos_kernel(reduced.r);
        case 2:
            return -detail::sin_kernel(reduced.r);
        default:
            return -detail::cos_kernel(reduced.r);
    }
}

// cos(x), over the same range, from the same reduction.
[[nodiscard]] constexpr double cos(double x) noexcept {
    if (!(x >= -detail::kReduceLimit && x <= detail::kReduceLimit)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const detail::Reduced reduced = detail::reduce(x);
    switch (reduced.quadrant) {
        case 0:
            return detail::cos_kernel(reduced.r);
        case 1:
            return -detail::sin_kernel(reduced.r);
        case 2:
            return -detail::cos_kernel(reduced.r);
        default:
            return detail::sin_kernel(reduced.r);
    }
}

}  // namespace iclforge::ac4::detail::dsp::portable
