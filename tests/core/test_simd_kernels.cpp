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
#include "ac3/core/mdct.hpp"
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

#ifdef AC3FORGE_HAVE_AVX2_TIER
namespace {

// dct4_scaled's / imdct512_windowed's twiddle-stage AVX2 kernels all share
// P = 128 (kQuarter, the long transform's own size) as a representative
// test size: it is a multiple of 4 (the AVX2 kernels' own requirement,
// documented on each in mdct_avx2.hpp) and matches a real call site
// exactly, unlike the short pair's P = 64. bitrev is the identity
// permutation - the kernels only use it as a scatter-target index array,
// so any permutation exercises the same read/gather + write/scatter code
// paths; identity keeps the test's own expected-value bookkeeping simple.
constexpr std::size_t kTestP = 128;

std::vector<double> deterministic_doubles(std::size_t n, std::uint64_t seed) {
    std::vector<double> v(n);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(-1.5, 1.5);
    for (double& x : v) {
        x = dist(rng);
    }
    return v;
}

std::vector<std::uint16_t> identity_bitrev(std::size_t n) {
    std::vector<std::uint16_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint16_t>(i);
    }
    return v;
}

bool all_bits_equal(std::span<const double> a, std::span<const double> b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_bits(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace
#endif

TEST_CASE("AVX2 dct4_pre_twiddle agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    const auto u = deterministic_doubles(2 * kTestP, 0x64637434'70726574ULL);
    const auto pre_re = deterministic_doubles(kTestP, 0x64637434'70725245ULL);
    const auto pre_im = deterministic_doubles(kTestP, 0x64637434'70724946ULL);
    const auto bitrev = identity_bitrev(kTestP);
    const std::size_t m_len = u.size();

    std::vector<double> scalar_re(kTestP), scalar_im(kTestP);
    for (std::size_t m = 0; m < kTestP; ++m) {
        const double a = u[2 * m];
        const double b = u[m_len - 1 - 2 * m];
        scalar_re[bitrev[m]] = a * pre_re[m] - b * pre_im[m];
        scalar_im[bitrev[m]] = a * pre_im[m] + b * pre_re[m];
    }

    std::vector<double> avx2_re(kTestP), avx2_im(kTestP);
    ac3::internal::avx2::dct4_pre_twiddle(u, pre_re, pre_im, bitrev, avx2_re, avx2_im);

    CHECK(all_bits_equal(avx2_re, scalar_re));
    CHECK(all_bits_equal(avx2_im, scalar_im));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 dct4_post_twiddle agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    const auto z_re = deterministic_doubles(kTestP, 0x706f7374'7a726530ULL);
    const auto z_im = deterministic_doubles(kTestP, 0x706f7374'7a696d30ULL);
    const auto post_re = deterministic_doubles(kTestP, 0x706f7374'52453031ULL);
    const auto post_im = deterministic_doubles(kTestP, 0x706f7374'494d3031ULL);
    constexpr double scale = -2.0 / 512.0;
    const std::size_t m_len = 2 * kTestP;

    std::vector<double> scalar_out(m_len);
    for (std::size_t k = 0; k < kTestP; ++k) {
        const double even = scale * (z_re[k] * post_re[k] - z_im[k] * post_im[k]);
        const double odd = scale * (-(z_re[k] * post_im[k] + z_im[k] * post_re[k]));
        scalar_out[2 * k] = even;
        scalar_out[m_len - 1 - 2 * k] = odd;
    }

    std::vector<double> avx2_out(m_len);
    ac3::internal::avx2::dct4_post_twiddle(z_re, z_im, post_re, post_im, scale, avx2_out);

    CHECK(all_bits_equal(avx2_out, scalar_out));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 imdct512_pre_twiddle agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    const auto coeffs = deterministic_doubles(4 * kTestP, 0x696d6463'74636f65ULL);
    const auto cos1 = deterministic_doubles(kTestP, 0x696d6463'74636f73ULL);
    const auto sin1 = deterministic_doubles(kTestP, 0x696d6463'74736e31ULL);
    const auto bitrev = identity_bitrev(kTestP);
    const std::size_t k_half_n = coeffs.size();

    std::vector<double> scalar_re(kTestP), scalar_im(kTestP);
    for (std::size_t k = 0; k < kTestP; ++k) {
        const double a = coeffs[k_half_n - 2 * k - 1];
        const double b = coeffs[2 * k];
        scalar_re[bitrev[k]] = a * cos1[k] - b * sin1[k];
        scalar_im[bitrev[k]] = -(b * cos1[k] + a * sin1[k]);
    }

    std::vector<double> avx2_re(kTestP), avx2_im(kTestP);
    ac3::internal::avx2::imdct512_pre_twiddle(coeffs, cos1, sin1, bitrev, avx2_re, avx2_im);

    CHECK(all_bits_equal(avx2_re, scalar_re));
    CHECK(all_bits_equal(avx2_im, scalar_im));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 imdct512_negate_copy agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    const auto z_re = deterministic_doubles(kTestP, 0x6e656763'6f707930ULL);
    const auto z_im = deterministic_doubles(kTestP, 0x6e656763'6f707931ULL);

    std::vector<double> scalar_re(kTestP), scalar_im(kTestP);
    for (std::size_t n = 0; n < kTestP; ++n) {
        scalar_re[n] = z_re[n];
        scalar_im[n] = -z_im[n];
    }

    std::vector<double> avx2_re(kTestP), avx2_im(kTestP);
    ac3::internal::avx2::imdct512_negate_copy(z_re, z_im, avx2_re, avx2_im);

    CHECK(all_bits_equal(avx2_re, scalar_re));
    CHECK(all_bits_equal(avx2_im, scalar_im));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 imdct512_post_twiddle agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    const auto cos1 = deterministic_doubles(kTestP, 0x706f7374'636f7331ULL);
    const auto sin1 = deterministic_doubles(kTestP, 0x706f7374'73696e31ULL);
    const auto t_re = deterministic_doubles(kTestP, 0x706f7374'74726530ULL);
    const auto t_im = deterministic_doubles(kTestP, 0x706f7374'74696d30ULL);

    std::vector<double> scalar_re(kTestP), scalar_im(kTestP);
    for (std::size_t n = 0; n < kTestP; ++n) {
        scalar_re[n] = t_re[n] * cos1[n] - t_im[n] * sin1[n];
        scalar_im[n] = t_im[n] * cos1[n] + t_re[n] * sin1[n];
    }

    std::vector<double> avx2_re(kTestP), avx2_im(kTestP);
    ac3::internal::avx2::imdct512_post_twiddle(cos1, sin1, t_re, t_im, avx2_re, avx2_im);

    CHECK(all_bits_equal(avx2_re, scalar_re));
    CHECK(all_bits_equal(avx2_im, scalar_im));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 imdct256_post_twiddle agrees with the scalar form bit-for-bit", "[simd][avx2]") {
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

    // kEighth = 64, not kTestP (128): the short pair's own real size, and a
    // different multiple-of-4 value than the other cases here exercise.
    constexpr std::size_t kEighth = 64;
    const auto cos2 = deterministic_doubles(kEighth, 0x70323536'636f7332ULL);
    const auto sin2 = deterministic_doubles(kEighth, 0x70323536'73696e32ULL);
    const auto t1_re = deterministic_doubles(kEighth, 0x70323536'74317265ULL);
    const auto t1_im = deterministic_doubles(kEighth, 0x70323536'74316d30ULL);
    const auto t2_re = deterministic_doubles(kEighth, 0x70323536'74327265ULL);
    const auto t2_im = deterministic_doubles(kEighth, 0x70323536'74326d30ULL);

    std::vector<double> scalar_y1_re(kEighth), scalar_y1_im(kEighth);
    std::vector<double> scalar_y2_re(kEighth), scalar_y2_im(kEighth);
    for (std::size_t n = 0; n < kEighth; ++n) {
        scalar_y1_re[n] = t1_re[n] * cos2[n] - t1_im[n] * sin2[n];
        scalar_y1_im[n] = t1_im[n] * cos2[n] + t1_re[n] * sin2[n];
        scalar_y2_re[n] = t2_re[n] * cos2[n] - t2_im[n] * sin2[n];
        scalar_y2_im[n] = t2_im[n] * cos2[n] + t2_re[n] * sin2[n];
    }

    std::vector<double> avx2_y1_re(kEighth), avx2_y1_im(kEighth);
    std::vector<double> avx2_y2_re(kEighth), avx2_y2_im(kEighth);
    ac3::internal::avx2::imdct256_post_twiddle(cos2, sin2, t1_re, t1_im, t2_re, t2_im, avx2_y1_re,
                                               avx2_y1_im, avx2_y2_re, avx2_y2_im);

    CHECK(all_bits_equal(avx2_y1_re, scalar_y1_re));
    CHECK(all_bits_equal(avx2_y1_im, scalar_y1_im));
    CHECK(all_bits_equal(avx2_y2_re, scalar_y2_re));
    CHECK(all_bits_equal(avx2_y2_im, scalar_y2_im));
#else
    SKIP("AC3FORGE_AVX2=OFF, or this is not an x86_64 build - no AVX2 tier was compiled");
#endif
}

TEST_CASE("AVX2 mdct512_forward_batch4 agrees with four scalar calls bit-for-bit",
         "[simd][avx2]") {
    // Same shape, and for the same reason, as the inverse case below: the
    // low-level AVX2 body needs FastMdctTables<512>, private to mdct.cpp,
    // so this goes through the PUBLIC ac3::mdct512_forward_batch4, which
    // only takes the AVX2 path when has_avx2() is true - guarded here
    // exactly like every other [avx2] case in this file.
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

    std::mt19937_64 rng(0x6d646374'6634666dULL);
    std::uniform_real_distribution<double> dist(-1.5, 1.5);
    std::array<std::array<double, 512>, 4> windowed{};
    for (auto& one : windowed) {
        for (double& v : one) {
            v = dist(rng);
        }
    }

    std::array<std::array<double, 256>, 4> scalar_c{};
    for (std::size_t i = 0; i < 4; ++i) {
        ac3::mdct512_forward(windowed[i], scalar_c[i], /*fast=*/true);
    }

    std::array<std::array<double, 256>, 4> batch_c{};
    ac3::mdct512_forward_batch4(windowed[0], windowed[1], windowed[2], windowed[3], batch_c[0],
                                batch_c[1], batch_c[2], batch_c[3]);

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!all_bits_equal(batch_c[i], scalar_c[i])) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("AVX2 imdct512_windowed_batch4 agrees with four scalar calls bit-for-bit",
         "[simd][avx2]") {
    // Unlike the kernels above, this one is tested through the PUBLIC
    // ac3::imdct512_windowed_batch4 (mdct.hpp) rather than
    // ac3::internal::avx2:: directly: the low-level AVX2 body needs the
    // twiddle/FFT tables mdct.cpp's own anonymous namespace holds
    // (twiddles(), fast_mdct_tables<512>()), which are not visible outside
    // that translation unit, so there is no way for a test to call it
    // without going through the orchestrator that resolves them. The
    // orchestrator itself checks has_avx2() internally and only takes the
    // AVX2 path when it is true, so this is still only a meaningful check
    // of the batched AVX2 kernel specifically on a machine that has one -
    // guarded exactly like every other [avx2] case in this file, not run
    // unconditionally.
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

    std::mt19937_64 rng(0x62617463'6834696dULL);
    std::uniform_real_distribution<double> dist(-2.0e7, 2.0e7);
    std::array<std::array<double, 256>, 4> coeffs{};
    for (auto& one : coeffs) {
        for (double& v : one) {
            v = dist(rng);
        }
    }

    std::array<std::array<double, 512>, 4> scalar_x{};
    for (std::size_t i = 0; i < 4; ++i) {
        ac3::imdct512_windowed(coeffs[i], scalar_x[i], /*fast=*/true);
    }

    std::array<std::array<double, 512>, 4> batch_x{};
    ac3::imdct512_windowed_batch4(coeffs[0], coeffs[1], coeffs[2], coeffs[3], batch_x[0],
                                  batch_x[1], batch_x[2], batch_x[3]);

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!all_bits_equal(batch_x[i], scalar_x[i])) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}
