#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/exponents.hpp"
#include "ac3/internal/arch/simd.hpp"

#include "fft_radix2.hpp"

// ROADMAP PF5's correctness gate.
//
// The vector kernels in the codec core are written once, against the two
// 128-bit types src/forge/src/internal/arch/<arch>/ac3/internal/arch/simd.hpp
// defines, and CMake chooses which of the three directories supplies them
// (see src/forge/CMakeLists.txt). The claim that makes that safe is not
// "close enough": it is that every seam operation is exactly one IEEE-754
// add, subtract or multiply per lane, so a kernel written against f64x2
// performs precisely the operations, in precisely the order, that the scalar
// loop it replaced performed - and therefore produces the same doubles, not
// merely nearby ones. Encoded output is a bit-exact function of those
// doubles; a last-place difference in an MDCT coefficient sitting on a power
// of two moves an exponent, and an exponent is 6.02 dB.
//
// So this file compares each vector kernel against a scalar reference IN THE
// SAME BINARY and requires bit-for-bit equality, never a tolerance. On a
// generic build much of it is a tautology that costs a few milliseconds; on
// the x86_64 and aarch64 builds - which is to say on every CI leg that is
// not deliberately forced to generic - it is the whole argument.
//
// This is separate from, and weaker than, the cross-build check: two builds
// differing only in AC3FORGE_SIMD encoding the corpus to byte-identical
// streams. That one covers restructuring this file cannot see. Both are
// described in docs/building.md.

namespace {

namespace arch = ac3::internal::arch;

// Bit-for-bit, not ==: this has to distinguish +0.0 from -0.0 (they compare
// equal) and has to fail rather than silently pass on a NaN pair (which
// compare unequal to everything, themselves included).
bool same_bits(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A deterministic spread of doubles across the ranges the kernels actually
// see, plus the ones that break naive implementations. Seeded, so a failure
// on a CI leg reproduces exactly.
std::vector<double> adversarial_doubles() {
    std::vector<double> v{
        0.0,
        -0.0,
        0.5,
        -0.5,
        1.5,
        -1.5,
        2.5,
        -2.5,
        3.5,
        -3.5,
        // Either side of a tie by one ulp: the values a round-half-up
        // implementation built on x + 0.5 gets wrong.
        0.49999999999999994,
        -0.49999999999999994,
        std::nextafter(0.5, 1.0),
        std::nextafter(2.5, 0.0),
        // The magic-number constant the SSE2 path pivots on, and its
        // neighbours.
        4503599627370496.0,   // 2^52
        4503599627370495.5,   // largest double below 2^52 with a fraction
        -4503599627370496.0,
        9007199254740992.0,  // 2^53
        // Fixed-point clamp boundaries (to_fixed25's own range).
        1.0,
        -1.0,
        0.9999999403953552,
        -0.9999999403953552,
        16777215.0 / 16777216.0,
        -1.0000001,
        1e-300,
        -1e-300,
        std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    // Every half-integer up to 2048, both signs: the whole tie ladder in the
    // range coefficients scaled by 2^24 actually reach.
    for (int i = 0; i <= 2048; ++i) {
        v.push_back(static_cast<double>(i) + 0.5);
        v.push_back(-(static_cast<double>(i) + 0.5));
    }
    std::mt19937_64 rng(0x5f5d5c5b'5a595857ULL);
    std::uniform_real_distribution<double> coefficient(-1.5, 1.5);
    std::uniform_real_distribution<double> scaled(-2.0e7, 2.0e7);
    for (int i = 0; i < 20000; ++i) {
        v.push_back(coefficient(rng));
        v.push_back(scaled(rng));
    }
    return v;
}

// Pseudorandom complex data for the FFT comparisons, in the magnitude range
// the pre-twiddle stage of a real transform hands the core.
template <std::size_t P>
void fill_random(std::array<double, P>& re, std::array<double, P>& im, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(-4.0, 4.0);
    for (std::size_t i = 0; i < P; ++i) {
        re[i] = dist(rng);
        im[i] = dist(rng);
    }
}

template <std::size_t P>
void check_fft_bit_exact(std::uint64_t seed) {
    static const ac3::internal::FftRadix2Tables<P> tables;
    std::array<double, P> re{};
    std::array<double, P> im{};
    fill_random<P>(re, im, seed);
    std::array<double, P> ref_re = re;
    std::array<double, P> ref_im = im;

    // fft_radix2_forward_vector, not fft_radix2_forward: the latter is the
    // dispatcher, and on a generic build it forwards to the reference, which
    // would make this compare a function with itself. Naming the vector form
    // directly checks it on EVERY build - including the generic one, where it
    // is not what the library runs but is still code that must be right if
    // someone forces AC3FORGE_SIMD or ports the seam to a new architecture.
    ac3::internal::fft_radix2_forward_vector<P>(tables, re, im);
    ac3::internal::fft_radix2_forward_reference<P>(tables, ref_re, ref_im);

    std::size_t mismatches = 0;
    for (std::size_t k = 0; k < P; ++k) {
        if (!same_bits(re[k], ref_re[k]) || !same_bits(im[k], ref_im[k])) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("arch seam reports which directory was compiled", "[simd]") {
    // Not an assertion about WHICH one - a generic build is legitimate
    // everywhere and is what -DAC3FORGE_SIMD=generic asks for. Printed so a
    // CI log records what the rest of this file actually exercised, which is
    // the difference between a meaningful run and a tautological one.
    std::printf("arch seam: %s\n", arch::kSimdName);
    CHECK(arch::kSimdName != nullptr);
}

TEST_CASE("f64x2 load, store, set and broadcast keep lane order", "[simd]") {
    const std::array<double, 2> src{-3.25, 7.5};
    std::array<double, 2> dst{};
    arch::f64x2::load(src.data()).store(dst.data());
    CHECK(same_bits(dst[0], src[0]));
    CHECK(same_bits(dst[1], src[1]));

    const auto pair = arch::f64x2::set(1.5, -2.5);
    CHECK(same_bits(pair.lane0(), 1.5));
    CHECK(same_bits(pair.lane1(), -2.5));

    const auto both = arch::f64x2::broadcast(-0.0);
    CHECK(same_bits(both.lane0(), -0.0));
    CHECK(same_bits(both.lane1(), -0.0));
}

TEST_CASE("f64x2 arithmetic is lane-wise IEEE-754", "[simd]") {
    const auto values = adversarial_doubles();
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
        const double a = values[i];
        const double b = values[i + 1];
        const auto va = arch::f64x2::set(a, b);
        const auto vb = arch::f64x2::set(b, a);
        const auto sum = va + vb;
        const auto diff = va - vb;
        const auto prod = va * vb;
        const auto neg = -va;
        // Skip the pairs whose scalar answer is a NaN: same_bits cannot
        // compare NaN payloads meaningfully, and nothing in the codec
        // produces one.
        if (std::isnan(a + b) || std::isnan(a - b) || std::isnan(a * b)) {
            continue;
        }
        if (!same_bits(sum.lane0(), a + b) || !same_bits(sum.lane1(), b + a) ||
            !same_bits(diff.lane0(), a - b) || !same_bits(diff.lane1(), b - a) ||
            !same_bits(prod.lane0(), a * b) || !same_bits(prod.lane1(), b * a) ||
            !same_bits(neg.lane0(), -a) || !same_bits(neg.lane1(), -b)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("round_ties_away is exactly std::round", "[simd]") {
    // The seam's one non-obvious contract, and the one whose x86_64
    // implementation is arithmetic rather than an instruction: SSE2 has no
    // rounding instruction, SSE4.1's rounds ties to EVEN, and the usual
    // trunc(x + copysign(0.5, x)) shortcut is wrong at 0.49999999999999994
    // (which is in the value set above). If this ever fails, to_fixed25_block
    // is producing different fixed-point values from to_fixed25 and every
    // encode on the leg is suspect.
    const auto values = adversarial_doubles();
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
        const double a = values[i];
        const double b = values[i + 1];
        const auto rounded = arch::round_ties_away(arch::f64x2::set(a, b));
        if (std::isnan(a) || std::isnan(b)) {
            continue;
        }
        if (!same_bits(rounded.lane0(), std::round(a)) ||
            !same_bits(rounded.lane1(), std::round(b))) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("i32x4 widening, shift and subtract match the scalar form", "[simd]") {
    // Every byte value, four at a time, through the §7.2.2.2 expression the
    // bit allocator uses it for: psd = 3072 - (exp << 7).
    std::array<std::uint8_t, 260> exps{};
    for (std::size_t i = 0; i < exps.size(); ++i) {
        exps[i] = static_cast<std::uint8_t>(i % 256);
    }
    const auto base = arch::i32x4::broadcast(3072);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i + 4 <= exps.size(); ++i) {
        std::array<std::int32_t, 4> got{};
        (base - arch::shift_left<7>(arch::i32x4::load_u8_widen(exps.data() + i)))
            .store(got.data());
        for (std::size_t lane = 0; lane < 4; ++lane) {
            if (got[lane] != 3072 - (exps[i + lane] << 7)) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("to_fixed25_block agrees with to_fixed25 element by element", "[simd]") {
    const auto values = adversarial_doubles();
    std::vector<std::int32_t> batched(values.size());
    ac3::to_fixed25_block(values, batched);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        // to_fixed25 narrows with a C++ cast, which is undefined for a NaN
        // or an out-of-range value; the clamp rules those out for everything
        // except a NaN, which nothing in the codec produces and which
        // adversarial_doubles() therefore does not include.
        if (batched[i] != ac3::to_fixed25(values[i])) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    // Odd lengths matter: every legal AC-3 mantissa count is odd (37, 61,
    // ... 253), so the batch form's scalar tail runs on every real call.
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{37},
                                std::size_t{253}}) {
        std::vector<std::int32_t> tail(n);
        ac3::to_fixed25_block(std::span{values}.first(n), tail);
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(tail[i] == ac3::to_fixed25(values[i]));
        }
    }
}

TEST_CASE("radix-2 FFT vector form is bit-identical to the scalar reference", "[simd]") {
    // The three sizes the codec instantiates: P = 64 (the block-switched
    // short transforms' DCT-IV core and their inverse), P = 128 (the long
    // transform's, and the long inverse's), P = 512 (dft512, the enhanced
    // coupling spectrum).
    check_fft_bit_exact<64>(0x1111'2222'3333'4444ULL);
    check_fft_bit_exact<128>(0x5555'6666'7777'8888ULL);
    check_fft_bit_exact<512>(0x9999'AAAA'BBBB'CCCCULL);
    // Repeat at P = 128 with several seeds: the butterfly's cancellation
    // behaviour depends on the data, and one draw is one sample.
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        check_fft_bit_exact<128>(seed * 0x9E37'79B9'7F4A'7C15ULL);
    }
}
