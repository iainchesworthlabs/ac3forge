#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Arithmetic modulo p = 2^255 - 19, for CPace's Elligator 2 map, which needs
// field operations the PSA Crypto API does not expose. Sixteen signed 64-bit
// limbs of 16 bits each: portable C++ with no 128-bit type, which MSVC lacks,
// and products that fit comfortably (a limb product is under 2^34 and a column
// of sixteen under 2^39).
//
// The operations run in time independent of the values, except that encode()
// and select() take their branches on a mask rather than a comparison, so the
// map from a pairing code to a CPace generator leaks nothing through timing.

namespace ac3::sendspin::field25519 {

using Element = std::array<std::int64_t, 16>;
using Bytes32 = std::array<std::uint8_t, 32>;

inline constexpr Element kZero{};
inline constexpr Element kOne{1};

// Brings every limb back to 16 bits, folding the carry out of the top limb into
// the bottom one as 38 (2^256 = 38 mod p).
constexpr void carry(Element& e) {
    for (std::size_t i = 0; i < e.size(); ++i) {
        e[i] += std::int64_t{1} << 16U;
        const std::int64_t c = e[i] >> 16U;
        if (i + 1 < e.size()) {
            e[i + 1] += c - 1;
        } else {
            e[0] += 38 * (c - 1);
        }
        e[i] -= c * (std::int64_t{1} << 16U);
    }
}

constexpr Element add(const Element& a, const Element& b) {
    Element r{};
    for (std::size_t i = 0; i < r.size(); ++i) {
        r[i] = a[i] + b[i];
    }
    return r;
}

constexpr Element sub(const Element& a, const Element& b) {
    Element r{};
    for (std::size_t i = 0; i < r.size(); ++i) {
        r[i] = a[i] - b[i];
    }
    return r;
}

constexpr Element mul(const Element& a, const Element& b) {
    std::array<std::int64_t, 31> t{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (std::size_t i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    Element r{};
    for (std::size_t i = 0; i < 16; ++i) {
        r[i] = t[i];
    }
    carry(r);
    carry(r);
    return r;
}

constexpr Element square(const Element& a) { return mul(a, a); }

// a = b when `condition` is 1, unchanged when it is 0, by mask.
constexpr void select(Element& a, const Element& b, std::int64_t condition) {
    const std::int64_t mask = -condition;
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] ^= mask & (a[i] ^ b[i]);
    }
}

// x^(p-2), the inverse (and 0 for 0). p - 2 = 2^255 - 21: bits 254 to 0 all set
// except 4 and 2.
constexpr Element invert(const Element& x) {
    Element c = x;
    for (int bit = 253; bit >= 0; --bit) {
        c = square(c);
        if (bit != 4 && bit != 2) {
            c = mul(c, x);
        }
    }
    return c;
}

// x^((p-1)/2): 1 for a non-zero square, p - 1 for a non-square. (p-1)/2 =
// 2^254 - 10: bits 253 to 0 all set except 3 and 0.
constexpr Element legendre(const Element& x) {
    Element c = x;
    for (int bit = 252; bit >= 0; --bit) {
        c = square(c);
        if (bit != 3 && bit != 0) {
            c = mul(c, x);
        }
    }
    return c;
}

// RFC 7748 decodeUCoordinate: little-endian, bit 255 ignored.
constexpr Element decode(const Bytes32& in) {
    Element e{};
    for (std::size_t i = 0; i < 16; ++i) {
        e[i] = std::int64_t{in[2 * i]} + (std::int64_t{in[(2 * i) + 1]} << 8U);
    }
    e[15] &= 0x7FFF;
    return e;
}

// The canonical little-endian encoding, fully reduced below p.
constexpr Bytes32 encode(const Element& in) {
    Element t = in;
    carry(t);
    carry(t);
    carry(t);
    for (int pass = 0; pass < 2; ++pass) {
        Element m{};
        m[0] = t[0] - 0xFFED;
        for (std::size_t i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16U) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16U) & 1);
        const std::int64_t borrow = (m[15] >> 16U) & 1;
        m[14] &= 0xFFFF;
        select(t, m, 1 - borrow);
    }
    Bytes32 out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(t[i] & 0xFF);
        out[(2 * i) + 1] = static_cast<std::uint8_t>((t[i] >> 8U) & 0xFF);
    }
    return out;
}

}  // namespace ac3::sendspin::field25519
