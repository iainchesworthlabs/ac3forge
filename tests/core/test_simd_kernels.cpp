#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/exponents.hpp"
#include "ac3/core/window.hpp"
#include "ac3/internal/arch/simd.hpp"
#include "cpu_features.hpp"

#ifdef AC3FORGE_HAVE_AVX2_TIER
#include "avx2_probe.hpp"
#include "mdct_avx2.hpp"
#endif

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
// This file checks the seam's PRIMITIVES (f64x2/i32x4 arithmetic,
// round_ties_away, to_fixed25_block) against a scalar reference in the same
// binary, bit-for-bit, never a tolerance - on a generic build that is a
// tautology that costs a few milliseconds, on the x86_64 and aarch64 builds
// it is the whole argument. The KERNELS built from those primitives
// (mdct.cpp's dct4_scaled pre/post-twiddle loops, the IMDCT twiddle stages,
// fft.cpp's dft512 normalisation, bitalloc.cpp's exponents_to_psd) are
// composition, not new arithmetic, so they inherit that guarantee rather
// than needing their own bit-exact unit test; their correctness is instead
// covered end to end by tests/core/test_mdct_fast.cpp's existing tolerance
// check against the direct-form oracle and by the cross-build corpus check
// below.
//
// The FFT/DCT-IV kernel itself (fft_kernel.hpp, ROADMAP PF4) is NOT part of
// this seam: its radix-4 restructuring is an algorithmic change (fewer
// operations), not a wider-lane one, and carries its own correctness
// argument in that header's comment.
//
// The cross-build check - two builds differing only in AC3FORGE_SIMD
// encoding the corpus to byte-identical streams - is separate from, and
// stronger than, anything in this file: it covers restructuring a unit test
// cannot see. Both are described in docs/building.md.

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

// ---------------------------------------------------------------------------
// Runtime CPU-feature dispatch (ROADMAP PF5's dynamic-dispatch follow-on).
//
// Unlike the tests above, this is not about a compile-time-selected tier's
// arithmetic - it is about whether the RUNTIME decision of which tier to
// use is itself correct, and whether the AVX2 tier (when compiled in)
// actually executes correctly on whatever machine happens to run this
// binary. The two cannot be conflated: has_avx2() answering correctly and
// the AVX2 code actually running correctly are two different claims, and
// the compile-everywhere/execute-if-capable split below tests each one
// where it can actually be tested.
// ---------------------------------------------------------------------------

TEST_CASE("cpu::has_avx2 reports a stable answer for this process", "[simd][avx2]") {
    // Not an assertion about which answer - a machine with no AVX2 is a
    // legitimate, common case (has_avx2() answering false there is exactly
    // correct), and even a build with AC3FORGE_AVX2=OFF or a non-x86_64
    // target must still answer unconditionally false rather than fail to
    // link or crash. Printed so a CI log records what the rest of this
    // file's [avx2] cases actually exercised.
    const bool avx2 = ac3::internal::cpu::has_avx2();
    std::printf("cpu::has_avx2(): %s\n", avx2 ? "true" : "false");
    // Resolved exactly once per process (a function-local static) - calling
    // it twice must never disagree with itself.
    CHECK(ac3::internal::cpu::has_avx2() == avx2);
}

TEST_CASE("AVX2 probe executes correctly where the CPU actually supports it",
         "[simd][avx2]") {
#ifdef AC3FORGE_HAVE_AVX2_TIER
    // Compile-everywhere, execute-if-capable (docs/building.md): the AVX2
    // TU above this test case in the same binary already proves the code
    // compiles and links on every x86_64 leg, MSVC/clang-cl/GCC/Clang/
    // AppleClang alike, with zero hardware dependency. This case proves the
    // other half - that it actually EXECUTES correctly - which can only be
    // checked on a machine that truly has AVX2. The four x86_64 CI legs
    // resolve to self-hosted-or-GitHub-hosted dynamically per run and
    // self-hosted CPU features are not documented anywhere in this repo,
    // so this must never assume the current host qualifies.
    if (!ac3::internal::cpu::has_avx2()) {
        // A loud, explicit skip - never a silent pass - unless
        // AC3FORGE_REQUIRE_AVX2=1 asks for a hard failure instead, which is
        // what turns "the AVX2 path ran and passed" into a guaranteed,
        // rather than aspirational, statement on whichever CI job sets it
        // (see tools/ci/run_codec_matrix.sh and docs/building.md).
        const char* const require = std::getenv("AC3FORGE_REQUIRE_AVX2");
        const bool required = require != nullptr && std::strcmp(require, "1") == 0;
        INFO("this CPU does not report AVX2 support - nothing to execute here");
        if (required) {
            FAIL("AC3FORGE_REQUIRE_AVX2=1 was set, but this host cannot run the AVX2 path "
                "it exists to prove - pin this job to hardware that actually has AVX2");
        }
        SKIP("AVX2 not available on this CPU");
    }
    CHECK(ac3::internal::avx2::avx2_probe_matches_expected());
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 apply_analysis_window agrees with the scalar form bit-for-bit",
         "[simd][avx2]") {
#ifdef AC3FORGE_HAVE_AVX2_TIER
    if (!ac3::internal::cpu::has_avx2()) {
        const char* const require = std::getenv("AC3FORGE_REQUIRE_AVX2");
        const bool required = require != nullptr && std::strcmp(require, "1") == 0;
        INFO("this CPU does not report AVX2 support - nothing to execute here");
        if (required) {
            FAIL("AC3FORGE_REQUIRE_AVX2=1 was set, but this host cannot run the AVX2 path "
                "it exists to prove - pin this job to hardware that actually has AVX2");
        }
        SKIP("AVX2 not available on this CPU");
    }

    // A deterministic pseudorandom 512-sample block, not the adversarial
    // corpus above: this is a single elementwise multiply against a fixed
    // table, so there is no rounding-mode edge case of its own to hunt for
    // (round_ties_away is what that corpus exists for) - the only claim to
    // check is that four lanes at a time produces the identical bits two
    // lanes at a time does, on ordinary in-range values.
    std::array<double, 512> x{};
    std::mt19937_64 rng(0x61617732'6b657273ULL);
    std::uniform_real_distribution<double> dist(-1.5, 1.5);
    for (double& v : x) {
        v = dist(rng);
    }

    std::array<double, 512> scalar{};
    for (std::size_t n = 0; n < x.size(); ++n) {
        scalar[n] = x[n] * ac3::kAnalysisWindow[n];
    }

    std::array<double, 512> avx2_result{};
    ac3::internal::avx2::apply_analysis_window(x, avx2_result);

    std::size_t mismatches = 0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        if (!same_bits(avx2_result[n], scalar[n])) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}
