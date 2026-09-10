#pragma once

#include <bit>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>

// The decoder's third scalar (planning/arithmetic-tiers.md): a signed 32-bit
// integer read as Q7.24 - seven bits of headroom above unity, twenty-four
// below. It is what `decode_scalar_t` names in the fixed-point build
// (src/forge/src/internal/scalar/fixed32/), for parts with no floating-point
// unit at all: an ESP32-C3 or a Cortex-M3, where even `float` is a compiled
// subroutine and a 5.1 E-AC-3 frame is 12.9 M soft-float instructions.
//
// What the format is for. A/52's coefficients are a mantissa in [-1, 1)
// times 2^-exponent, so they are below unity; the transform's intermediate
// growth and a downmix sum are what the headroom absorbs; and the
// twenty-four fractional bits put the quantisation floor at -144 dB of full
// scale, which is what leaves a decode above 100 dB of the double one after
// a transform whose stages each cost a fraction of a bit. Constants - the
// twiddles, the windows, the downmix coefficients - are the same format: a
// value of exactly 1.0 is 2^24 and fits, and twenty-four bits of a twiddle
// are more than the arithmetic around it keeps.
//
// What the type deliberately is not. It is not a general fixed-point library:
// only the operations the decode path asks of a scalar exist, each with one
// rounding rule, so the result is defined by this file and not by a
// template's cleverness. Products round half up on the shift back and
// saturate; sums wrap (two's complement, defined, never undefined); the
// conversions from a wider type saturate. It is not `std::floating_point`
// either: the templates the decode path already has are written against a
// scalar that behaves like a number, and where one of them needs more than
// arithmetic - a square root, a noise draw from a 32-bit state, a power of two
// - the header it lives in asks for that through a member or an overload set
// rather than through <cmath>.
//
// Why integer arithmetic earns the strongest of the three tiers' guarantees:
// it is the same on every machine. No rounding mode, no fused multiply-add,
// no C library's last bit. A fixed-point decode of a stream produces the same
// PCM on x86, on the Cortex-M3 leg and on a RISC-V part, bit for bit, by
// construction - which is what lets the probe gate it with an exact hash.

namespace ac3::internal {

struct Fixed32 {
    static constexpr int kFractionBits = 24;
    static constexpr std::int32_t kOne = std::int32_t{1} << kFractionBits;
    static constexpr double kScale = 16777216.0;  // 2^24

    std::int32_t raw = 0;

    constexpr Fixed32() = default;

    // From an integer: `Scalar{0}`, `Scalar{2}`, `static_cast<Scalar>(n)`.
    // Saturates: an integer past +-127 is not a value this format has, and
    // the callers that pass one (a sample count, say) are the ones that have
    // to be written differently for this tier - see output.cpp's ramp.
    template <std::integral I>
    explicit constexpr Fixed32(I value) : raw(saturate(static_cast<std::int64_t>(value) * kOne)) {}

    // From a floating value: rounded half away from zero, saturated, on the
    // value's own bits. Not `value * 2^24 + 0.5` and a cast: that is a
    // floating expression, and a compiler is free to evaluate one
    // differently on two targets - the x86 host and the Cortex-M3 leg
    // disagreed by a raw unit on a handful of samples when this was written
    // that way, and the tier's whole promise is that they do not. On the
    // bits it is integer arithmetic, the same everywhere, and on a part with
    // no FPU it is also a fraction of the cost of the software multiply, add
    // and conversion the expression compiled to. A NaN or an infinity
    // saturates in its sign's direction rather than being undefined.
    explicit constexpr Fixed32(double value) : raw(from_double_bits(value)) {}
    explicit constexpr Fixed32(float value) : raw(from_float_bits(value)) {}

    [[nodiscard]] static constexpr Fixed32 from_raw(std::int32_t r) {
        Fixed32 f;
        f.raw = r;
        return f;
    }

    // An integer times 2^power, exactly: what a dequantiser wants for a code
    // of up to sixteen bits that the format could not hold before the scale
    // (mantissas.hpp's asymmetric quantisers). Saturates past the format.
    [[nodiscard]] static constexpr Fixed32 from_integer_scaled(std::int32_t value, int power) {
        const int shift = kFractionBits + power;
        if (shift >= 0) {
            if (shift >= 32) {
                return from_raw(value == 0 ? 0
                                : value < 0 ? std::numeric_limits<std::int32_t>::min()
                                            : std::numeric_limits<std::int32_t>::max());
            }
            return from_raw(saturate(static_cast<std::int64_t>(value) << shift));
        }
        if (shift <= -32) {
            return from_raw(value < 0 ? -1 : 0);
        }
        return from_raw(static_cast<std::int32_t>(value >> -shift));
    }

    // A ratio of two integers, truncated: for a fraction whose parts the
    // format could not hold (a band centre over a bin count).
    [[nodiscard]] static constexpr Fixed32 from_integer_ratio(std::int64_t numerator,
                                                              std::int64_t denominator) {
        if (denominator == 0) {
            return from_raw(numerator < 0 ? std::numeric_limits<std::int32_t>::min()
                                          : std::numeric_limits<std::int32_t>::max());
        }
        return from_raw(saturate((numerator << kFractionBits) / denominator));
    }

    // A noise generator's 32-bit state as a value in [0, 1): the top
    // twenty-four bits, which is what the format holds. The double and float
    // decoders divide the whole state by 2^32 - 1 instead (eac3_tools.hpp,
    // mantissas.hpp); the sequences are the decoder's own to choose
    // (§7.3.4, §3.5.5.3), and this one is deterministic per instance too.
    [[nodiscard]] static constexpr Fixed32 unit_from_state(std::uint32_t state) {
        return from_raw(static_cast<std::int32_t>(state >> 8));
    }

    explicit constexpr operator double() const { return static_cast<double>(raw) / kScale; }
    // The float of the raw integer times an exact power of two: the same
    // value the double route gives (float(raw) rounds to 24 significant bits
    // once, and the scale moves the exponent only), without a double divide
    // on a part that has neither a divider nor a double.
    explicit constexpr operator float() const {
        return static_cast<float>(raw) * (1.0F / 16777216.0F);
    }
    // Truncation toward zero, like a float-to-int cast.
    explicit constexpr operator int() const { return static_cast<int>(raw / kOne); }

    // Sums wrap. The format's seven bits of headroom are what keep a legal
    // stream's arithmetic away from the edge; a hostile one gets wrong audio,
    // never undefined behaviour.
    friend constexpr Fixed32 operator+(Fixed32 a, Fixed32 b) {
        return from_raw(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.raw) +
                                                  static_cast<std::uint32_t>(b.raw)));
    }
    friend constexpr Fixed32 operator-(Fixed32 a, Fixed32 b) {
        return from_raw(static_cast<std::int32_t>(static_cast<std::uint32_t>(a.raw) -
                                                  static_cast<std::uint32_t>(b.raw)));
    }
    friend constexpr Fixed32 operator-(Fixed32 a) {
        return from_raw(static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(a.raw)));
    }
    // The product through 64 bits, rounded half up on the way back, saturated.
    // One rounding rule, everywhere.
    friend constexpr Fixed32 operator*(Fixed32 a, Fixed32 b) {
        const std::int64_t product = static_cast<std::int64_t>(a.raw) * b.raw;
        return from_raw(saturate((product + (std::int64_t{1} << (kFractionBits - 1))) >>
                                 kFractionBits));
    }
    // Truncating division; a zero divisor saturates in the dividend's
    // direction rather than trapping. Rare on the decode path (a band's RMS,
    // a table built at start-up), never in a per-sample loop.
    friend constexpr Fixed32 operator/(Fixed32 a, Fixed32 b) {
        if (b.raw == 0) {
            return from_raw(a.raw < 0 ? std::numeric_limits<std::int32_t>::min()
                                      : std::numeric_limits<std::int32_t>::max());
        }
        return from_raw(saturate((static_cast<std::int64_t>(a.raw) << kFractionBits) / b.raw));
    }
    constexpr Fixed32& operator+=(Fixed32 o) { return *this = *this + o; }
    constexpr Fixed32& operator-=(Fixed32 o) { return *this = *this - o; }
    constexpr Fixed32& operator*=(Fixed32 o) { return *this = *this * o; }
    constexpr Fixed32& operator/=(Fixed32 o) { return *this = *this / o; }

    friend constexpr auto operator<=>(Fixed32 a, Fixed32 b) = default;

    // Scaling by 2^n: a shift. Left is exact until the value leaves the
    // format and saturates there; right rounds half up, the product's rule.
    [[nodiscard]] constexpr Fixed32 scaled_by_pow2(int n) const {
        if (n >= 0) {
            if (n >= 31) {
                return from_raw(raw == 0 ? 0
                                : raw < 0 ? std::numeric_limits<std::int32_t>::min()
                                          : std::numeric_limits<std::int32_t>::max());
            }
            return from_raw(saturate(static_cast<std::int64_t>(raw) << n));
        }
        if (n <= -32) {
            return from_raw(0);
        }
        const std::int64_t half = std::int64_t{1} << (-n - 1);
        return from_raw(static_cast<std::int32_t>((static_cast<std::int64_t>(raw) + half) >> -n));
    }

   private:
    // sign, biased exponent, mantissa with the implicit bit restored, and
    // the shift from the mantissa's own scale (2^(exponent - mantissa bits))
    // to raw units (2^-24): raw = mantissa x 2^(exponent - mantissa_bits + 24).
    [[nodiscard]] static constexpr std::int32_t from_bits(bool negative, int biased_exponent,
                                                          int exponent_bias, int mantissa_bits,
                                                          std::uint64_t mantissa,
                                                          int max_biased_exponent) {
        if (biased_exponent == 0) {
            return 0;  // zero or a denormal, below the format's floor either way
        }
        if (biased_exponent == max_biased_exponent) {
            return negative ? std::numeric_limits<std::int32_t>::min()
                            : std::numeric_limits<std::int32_t>::max();
        }
        const int shift = biased_exponent - exponent_bias - mantissa_bits + kFractionBits;
        std::uint64_t magnitude = 0;
        if (shift >= 0) {
            if (shift >= 64 - mantissa_bits - 1) {
                return negative ? std::numeric_limits<std::int32_t>::min()
                                : std::numeric_limits<std::int32_t>::max();
            }
            magnitude = mantissa << shift;
        } else if (shift > -64) {
            magnitude = (mantissa + (std::uint64_t{1} << (-shift - 1))) >> -shift;
        }
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1) {
            return negative ? std::numeric_limits<std::int32_t>::min()
                            : std::numeric_limits<std::int32_t>::max();
        }
        const auto signed_magnitude = static_cast<std::int64_t>(magnitude);
        return saturate(negative ? -signed_magnitude : signed_magnitude);
    }

    [[nodiscard]] static constexpr std::int32_t from_double_bits(double value) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        return from_bits((bits >> 63) != 0, static_cast<int>((bits >> 52) & 0x7FFU), 1023, 52,
                         (bits & ((std::uint64_t{1} << 52) - 1)) | (std::uint64_t{1} << 52),
                         0x7FF);
    }

    [[nodiscard]] static constexpr std::int32_t from_float_bits(float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        return from_bits((bits >> 31) != 0, static_cast<int>((bits >> 23) & 0xFFU), 127, 23,
                         (bits & 0x7FFFFFU) | 0x800000U, 0xFF);
    }

    [[nodiscard]] static constexpr std::int32_t saturate(std::int64_t v) {
        if (v > std::numeric_limits<std::int32_t>::max()) {
            return std::numeric_limits<std::int32_t>::max();
        }
        if (v < std::numeric_limits<std::int32_t>::min()) {
            return std::numeric_limits<std::int32_t>::min();
        }
        return static_cast<std::int32_t>(v);
    }
};

[[nodiscard]] constexpr Fixed32 abs(Fixed32 x) {
    return x.raw < 0 ? -x : x;
}

// --- The scalar overload sets the decode path calls instead of <cmath> ------
//
// The double and float overloads are what those paths called before this tier
// existed, so nothing about them changes; the Fixed32 ones are integer.

[[nodiscard]] inline double scalar_sqrt(double x) { return std::sqrt(x); }
[[nodiscard]] inline float scalar_sqrt(float x) { return std::sqrt(x); }
// floor(sqrt(n)) for a 64-bit n below 2^62 (which is every value the tier
// forms: a raw value times 2^24, or a band's summed squares). Newton from a
// power-of-two start, which converges in a handful of steps at these widths.
[[nodiscard]] constexpr std::uint64_t isqrt64(std::uint64_t n) {
    if (n == 0) {
        return 0;
    }
    // Start at 2^ceil(bits/2), never below the root.
    const int bits = 64 - std::countl_zero(n);
    std::uint64_t r = std::uint64_t{1} << ((bits + 1) / 2);
    while (true) {
        const std::uint64_t next = (r + n / r) / 2;
        if (next >= r) {
            break;
        }
        r = next;
    }
    // r is floor(sqrt(n)) or one above; settle it.
    while (r * r > n) {
        --r;
    }
    while ((r + 1) * (r + 1) <= n) {
        ++r;
    }
    return r;
}

// Square root of raw * 2^24 (the value times 2^48 in raw units), rounded
// down: sqrt(v) in Q7.24 = isqrt(raw << 24).
[[nodiscard]] constexpr Fixed32 scalar_sqrt(Fixed32 x) {
    if (x.raw <= 0) {
        return Fixed32{};
    }
    return Fixed32::from_raw(static_cast<std::int32_t>(
        isqrt64(static_cast<std::uint64_t>(x.raw) << Fixed32::kFractionBits)));
}

[[nodiscard]] inline double scalar_abs(double x) { return std::abs(x); }
[[nodiscard]] inline float scalar_abs(float x) { return std::abs(x); }
[[nodiscard]] constexpr Fixed32 scalar_abs(Fixed32 x) { return abs(x); }

// A value scaled by 2^n: std::ldexp for the floating types, a shift here.
[[nodiscard]] inline double scalar_ldexp(double x, int n) { return std::ldexp(x, n); }
[[nodiscard]] inline float scalar_ldexp(float x, int n) { return std::ldexp(x, n); }
[[nodiscard]] constexpr Fixed32 scalar_ldexp(Fixed32 x, int n) { return x.scaled_by_pow2(n); }

}  // namespace ac3::internal
