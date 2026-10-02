#pragma once

#include <bit>
#include <compare>
#include <cstdint>
#include <limits>

#include "iclforge/arithmetic/fixed32.hpp"

// A value as a signed 32-bit mantissa and a power of two, for the AC-4 decoder's fixed-point
// tier (planning/ac4.md, D14d): the energies, gains and scale factors of A-SPX, the transient
// ducker's and the companding's levels, a prediction's covariances, which span a range no
// absolute fixed-point format holds. A-SPX's signal scale factors alone run from 2^-96 to
// 2^96 in the double decoder, and a quiet subband's energy is below Fixed32's 2^-24 where a
// loud one is above its 2^7.
//
// value = m * 2^(e - 30), with |m| in [2^29, 2^30) or m = 0 and e = kZeroExponent, so each
// value has one representation and two compare by sign, exponent and mantissa in that order.
// Thirty bits of mantissa: more than float's twenty-four, fewer than double's fifty-three.
//
// Every operation is integer arithmetic with one rounding rule, half away from zero on the
// magnitude when a result is brought back to thirty bits, so a value is the same on every
// machine, as Fixed32's are. log2 and exp2 are series in integers, not calls into a C library.
// Overflow and underflow are of the exponent only, which is 32 bits wide: no audio reaches
// either.
//
// It is not a general software float: no infinities, no NaN, no subnormals and no rounding
// modes. A zero divisor gives the largest value of the dividend's sign.

namespace iclforge::internal {

struct MantExp {
    static constexpr int kMantissaBits = 30;
    static constexpr std::int32_t kZeroExponent = -(1 << 24);
    static constexpr std::int32_t kMaxExponent = 1 << 24;

    std::int32_t m = 0;
    std::int32_t e = kZeroExponent;

    constexpr MantExp() = default;

    // An integer, exactly.
    template <std::integral I>
    explicit constexpr MantExp(I value) : MantExp(make(static_cast<std::int64_t>(value), 0)) {}

    // A floating value on its bits, rounded to thirty bits: for constants, built at compile
    // time. Zero, a subnormal, an infinity or a NaN gives zero, which none of the constants is.
    explicit constexpr MantExp(double value) : MantExp(from_double_bits(value)) {}

    // A Fixed32's value, exactly.
    explicit constexpr MantExp(Fixed32 value) : MantExp(make(value.raw, -Fixed32::kFractionBits)) {}

    // v * 2^power, rounded to thirty bits.
    [[nodiscard]] static constexpr MantExp make(std::int64_t v, int power) {
        if (v == 0) {
            return {};
        }
        const bool negative = v < 0;
        std::uint64_t magnitude =
            negative ? 0U - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
        const int bits = 64 - std::countl_zero(magnitude);
        int shift = bits - kMantissaBits;
        if (shift > 0) {
            magnitude = (magnitude + (std::uint64_t{1} << static_cast<unsigned>(shift - 1))) >>
                        static_cast<unsigned>(shift);
            if (magnitude == (std::uint64_t{1} << kMantissaBits)) {
                magnitude >>= 1U;
                ++shift;
            }
        } else if (shift < 0) {
            magnitude <<= static_cast<unsigned>(-shift);
        }
        const auto mantissa = static_cast<std::int32_t>(magnitude);
        return from_parts(negative ? -mantissa : mantissa, power + shift + kMantissaBits);
    }

    [[nodiscard]] static constexpr MantExp from_parts(std::int32_t mantissa, std::int64_t exponent) {
        MantExp out;
        if (mantissa == 0 || exponent < kZeroExponent) {
            return out;
        }
        out.m = mantissa;
        out.e = static_cast<std::int32_t>(exponent > kMaxExponent ? kMaxExponent : exponent);
        return out;
    }

    // The largest value of a sign: what a division by zero gives.
    [[nodiscard]] static constexpr MantExp largest(bool negative) {
        return from_parts(negative ? -((1 << kMantissaBits) - 1) : (1 << kMantissaBits) - 1,
                          kMaxExponent);
    }

    [[nodiscard]] constexpr bool is_zero() const { return m == 0; }
    [[nodiscard]] constexpr bool negative() const { return m < 0; }

    // Times 2^n, exactly.
    [[nodiscard]] constexpr MantExp scaled_by_pow2(int n) const {
        return m == 0 ? MantExp{} : from_parts(m, static_cast<std::int64_t>(e) + n);
    }

    // The Fixed32 nearest the value, half away from zero, saturated at the format's edges.
    [[nodiscard]] constexpr Fixed32 to_fixed() const {
        if (m == 0) {
            return Fixed32{};
        }
        // raw = m * 2^(e - 30 + 24)
        const std::int64_t shift = static_cast<std::int64_t>(e) - (kMantissaBits - Fixed32::kFractionBits);
        if (shift >= 2) {  // |m| * 4 is at least 2^31
            return Fixed32::from_raw(m < 0 ? std::numeric_limits<std::int32_t>::min()
                                           : std::numeric_limits<std::int32_t>::max());
        }
        if (shift >= 0) {
            const std::int64_t raw = static_cast<std::int64_t>(m) << static_cast<unsigned>(shift);
            if (raw > std::numeric_limits<std::int32_t>::max()) {
                return Fixed32::from_raw(std::numeric_limits<std::int32_t>::max());
            }
            if (raw < std::numeric_limits<std::int32_t>::min()) {
                return Fixed32::from_raw(std::numeric_limits<std::int32_t>::min());
            }
            return Fixed32::from_raw(static_cast<std::int32_t>(raw));
        }
        if (shift <= -32) {
            return Fixed32{};
        }
        const auto s = static_cast<unsigned>(-shift);
        const std::uint32_t magnitude = m < 0 ? 0U - static_cast<std::uint32_t>(m) : static_cast<std::uint32_t>(m);
        const std::uint32_t rounded = (magnitude + (std::uint32_t{1} << (s - 1U))) >> s;
        const auto value = static_cast<std::int32_t>(rounded);
        return Fixed32::from_raw(m < 0 ? -value : value);
    }

    // The double of the value: exact, the mantissa having thirty bits.
    [[nodiscard]] constexpr double to_double() const {
        if (m == 0) {
            return 0.0;
        }
        double value = static_cast<double>(m);
        int n = e - kMantissaBits;
        while (n > 0) {
            const int step = n > 512 ? 512 : n;
            value *= pow2_double(step);
            n -= step;
        }
        while (n < 0) {
            const int step = n < -512 ? 512 : -n;
            value /= pow2_double(step);
            n += step;
        }
        return value;
    }
    explicit constexpr operator double() const { return to_double(); }

    friend constexpr MantExp operator-(MantExp a) { return a.m == 0 ? a : from_parts(-a.m, a.e); }

    friend constexpr MantExp operator*(MantExp a, MantExp b) {
        if (a.m == 0 || b.m == 0) {
            return {};
        }
        return make(static_cast<std::int64_t>(a.m) * b.m,
                    static_cast<int>(clamp_exponent(static_cast<std::int64_t>(a.e) + b.e)) -
                        2 * kMantissaBits);
    }

    friend constexpr MantExp operator+(MantExp a, MantExp b) {
        if (a.m == 0) {
            return b;
        }
        if (b.m == 0) {
            return a;
        }
        if (a.e < b.e) {
            const MantExp t = a;
            a = b;
            b = t;
        }
        const std::int64_t d = static_cast<std::int64_t>(a.e) - b.e;
        if (d > 62) {
            return a;
        }
        // Both mantissas up by 31 bits, the smaller's then down by d: the sum is below 2^62,
        // and the bits of b that fall off are below the rounding of the result.
        const std::int64_t big = static_cast<std::int64_t>(a.m) * (std::int64_t{1} << 31U);
        const std::int64_t small =
            (static_cast<std::int64_t>(b.m) * (std::int64_t{1} << 31U)) >> static_cast<unsigned>(d);
        return make(big + small, a.e - kMantissaBits - 31);
    }
    friend constexpr MantExp operator-(MantExp a, MantExp b) { return a + (-b); }

    friend constexpr MantExp operator/(MantExp a, MantExp b) {
        if (b.m == 0) {
            return largest(a.m < 0);
        }
        if (a.m == 0) {
            return {};
        }
        // |a.m| * 2^32 / |b.m| is in (2^31, 2^33).
        const std::int64_t q = (static_cast<std::int64_t>(a.m) * (std::int64_t{1} << 32U)) / b.m;
        return make(q, static_cast<int>(clamp_exponent(static_cast<std::int64_t>(a.e) - b.e)) - 32);
    }

    constexpr MantExp& operator+=(MantExp o) { return *this = *this + o; }
    constexpr MantExp& operator-=(MantExp o) { return *this = *this - o; }
    constexpr MantExp& operator*=(MantExp o) { return *this = *this * o; }
    constexpr MantExp& operator/=(MantExp o) { return *this = *this / o; }

    friend constexpr bool operator==(MantExp a, MantExp b) = default;
    friend constexpr std::strong_ordering operator<=>(MantExp a, MantExp b) {
        const int sa = a.m > 0 ? 1 : (a.m < 0 ? -1 : 0);
        const int sb = b.m > 0 ? 1 : (b.m < 0 ? -1 : 0);
        if (sa != sb || sa == 0) {
            return sa <=> sb;
        }
        if (a.e != b.e) {
            return sa > 0 ? a.e <=> b.e : b.e <=> a.e;
        }
        return a.m <=> b.m;
    }

   private:
    [[nodiscard]] static constexpr std::int64_t clamp_exponent(std::int64_t e) {
        return e < -2 * static_cast<std::int64_t>(kMaxExponent)
                   ? -2 * static_cast<std::int64_t>(kMaxExponent)
                   : (e > 2 * static_cast<std::int64_t>(kMaxExponent) ? 2 * static_cast<std::int64_t>(kMaxExponent) : e);
    }

    [[nodiscard]] static constexpr double pow2_double(int n) {
        return std::bit_cast<double>(static_cast<std::uint64_t>(1023 + n) << 52U);
    }

    [[nodiscard]] static constexpr MantExp from_double_bits(double value) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        const auto biased = static_cast<int>((bits >> 52U) & 0x7FFU);
        if (biased == 0 || biased == 0x7FF) {
            return {};
        }
        const auto mantissa =
            static_cast<std::int64_t>((bits & ((std::uint64_t{1} << 52U) - 1U)) | (std::uint64_t{1} << 52U));
        return make((bits >> 63U) != 0 ? -mantissa : mantissa, biased - 1075);
    }
};

[[nodiscard]] constexpr MantExp abs(MantExp x) {
    return x.m < 0 ? -x : x;
}

// The root of a value that is not negative, rounded down in its last bit; 0 for the rest.
[[nodiscard]] constexpr MantExp scalar_sqrt(MantExp x) {
    if (x.m <= 0) {
        return {};
    }
    // x = M 2^E with M = m and E = e - 30, E made even.
    std::uint64_t mantissa = static_cast<std::uint64_t>(x.m);
    std::int64_t exponent = static_cast<std::int64_t>(x.e) - MantExp::kMantissaBits;
    if ((exponent & 1) != 0) {
        mantissa <<= 1U;
        exponent -= 1;
    }
    // sqrt(M 2^32) 2^((E - 32) / 2), M 2^32 below 2^63.
    const std::uint64_t root = isqrt64(mantissa << 32U);
    return MantExp::make(static_cast<std::int64_t>(root), static_cast<int>((exponent - 32) / 2));
}

namespace mant_exp_detail {

// Q30 integers, 2^30 being one.
inline constexpr std::int64_t kOne = std::int64_t{1} << 30U;

[[nodiscard]] constexpr std::int64_t mul_q30(std::int64_t a, std::int64_t b) {
    const std::int64_t p = a * b;
    return p >= 0 ? (p + (kOne >> 1U)) >> 30U : -((-p + (kOne >> 1U)) >> 30U);
}

}  // namespace mant_exp_detail

// log2 of a positive value; the largest negative value for the rest. The series is the float
// overload's (scalar_math.hpp), 2 log2(e) atanh(u) with u = (y - 1) / (y + 1) and y in
// [sqrt(1/2), sqrt(2)), to u^11, in Q30: within a few units of 2^-30 of the logarithm.
[[nodiscard]] constexpr MantExp scalar_log2(MantExp x) {
    using namespace mant_exp_detail;
    if (x.m <= 0) {
        return MantExp::largest(true);
    }
    // x = (m / 2^29) 2^(e - 1), m / 2^29 in [1, 2).
    std::int64_t y = static_cast<std::int64_t>(x.m) * 2;  // Q30, in [1, 2)
    std::int64_t integer = static_cast<std::int64_t>(x.e) - 1;
    constexpr std::int64_t kRootHalfQ30 = 759250125;  // sqrt(1/2) in Q30
    if (y >= 2 * kRootHalfQ30) {                       // y >= sqrt(2)
        y = (y + 1) / 2;
        integer += 1;
    }
    const std::int64_t u = ((y - kOne) * kOne) / (y + kOne);  // |u| <= 0.1716
    const std::int64_t u2 = mul_q30(u, u);
    constexpr std::int64_t kTwoLog2e = 3098164010LL;  // 2 / ln 2 in Q30
    std::int64_t series = kOne / 11;
    series = kOne / 9 + mul_q30(u2, series);
    series = kOne / 7 + mul_q30(u2, series);
    series = kOne / 5 + mul_q30(u2, series);
    series = kOne / 3 + mul_q30(u2, series);
    series = kOne + mul_q30(u2, series);
    const std::int64_t fraction = mul_q30(kTwoLog2e, mul_q30(u, series));
    return MantExp::make(integer * kOne + fraction, -30);
}

// 2^x. x = n + r with n the nearest integer and r in [-1/2, 1/2], 2^r = e^(r ln 2) by its
// Taylor series to the ninth power in Q30, within a few units of 2^-30. Arguments beyond
// 2^20 in magnitude are clamped there.
[[nodiscard]] constexpr MantExp scalar_exp2(MantExp x) {
    using namespace mant_exp_detail;
    if (x.m == 0) {
        return MantExp{1};
    }
    // x in Q30.
    std::int64_t q = 0;
    const std::int64_t shift = static_cast<std::int64_t>(x.e) - MantExp::kMantissaBits + 30;
    constexpr std::int64_t kLimit = std::int64_t{1} << 50U;  // 2^20 in Q30
    if (shift >= 21) {
        q = x.m < 0 ? -kLimit : kLimit;
    } else if (shift >= 0) {
        q = static_cast<std::int64_t>(x.m) * (std::int64_t{1} << static_cast<unsigned>(shift));
        q = q > kLimit ? kLimit : (q < -kLimit ? -kLimit : q);
    } else if (shift > -63) {
        const auto s = static_cast<unsigned>(-shift);
        const std::int64_t mag = x.m < 0 ? -static_cast<std::int64_t>(x.m) : static_cast<std::int64_t>(x.m);
        const std::int64_t rounded = s >= 32 ? 0 : (mag + (std::int64_t{1} << (s - 1U))) >> s;
        q = x.m < 0 ? -rounded : rounded;
    }
    const std::int64_t n = (q + (kOne >> 1U)) >> 30U;  // floor(x + 1/2)
    const std::int64_t r = q - n * kOne;               // [-1/2, 1/2)
    constexpr std::int64_t kLn2 = 744261118;           // ln 2 in Q30
    const std::int64_t t = mul_q30(r, kLn2);
    std::int64_t p = kOne / 362880;
    p = kOne / 40320 + mul_q30(t, p);
    p = kOne / 5040 + mul_q30(t, p);
    p = kOne / 720 + mul_q30(t, p);
    p = kOne / 120 + mul_q30(t, p);
    p = kOne / 24 + mul_q30(t, p);
    p = kOne / 6 + mul_q30(t, p);
    p = kOne / 2 + mul_q30(t, p);
    p = kOne + mul_q30(t, p);
    p = kOne + mul_q30(t, p);
    return MantExp::make(p, static_cast<int>(n) - 30);
}

}  // namespace iclforge::internal
