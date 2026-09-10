#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <ranges>
#include <span>

#include "ac3/core/mdct.hpp"
#include "golden/mdct_goldens.hpp"

// Phase 4 of the performance-observability programme: the opt-in §7.9.4
// fast N/4-FFT MDCT (mdct.cpp's mdct_forward_fast_core, reached via
// mdct512_forward's `fast` parameter). This file is the correctness half -
// the fast path must agree with the existing direct-form path (already
// validated against numpy goldens in test_mdct.cpp) to within a tight
// numerical tolerance, on both synthetic and real-audio-shaped input, before
// anything is allowed to default it on.
//
// All three forward transforms have an accelerated path now, each with its
// own independently-derived fold (mdct256_forward_first/second landed theirs
// after an earlier attempt to reuse the LONG transform's fold for alpha = -1
// turned out to be a math error - see mdct.hpp), so each is checked against
// its own direct-form table here rather than against another transform's.

namespace {

// Relative error, normalized against the SPECTRUM's own peak magnitude
// rather than each bin's individual value. A per-bin denominator blows up on
// exactly the input this test cares about: a real (tone) signal concentrates
// almost all its energy into a handful of bins, leaving the rest at genuine
// spectral-leakage magnitudes (1e-8..1e-11) where a few ULPs of ordinary
// floating-point rounding - present between ANY two algorithms that do not
// perform IDENTICAL operations in IDENTICAL order, fast-vs-direct included -
// reads as a huge per-bin relative error despite being scientifically
// meaningless at that magnitude. Peak-normalizing is the standard way to
// compare two spectra and avoids that trap while staying scale-invariant.
double max_rel_error(std::span<const double> fast, std::span<const double> direct) {
    double peak = 0.0;
    for (const double v : direct) {
        peak = std::max(peak, std::abs(v));
    }
    const double denom = std::max(peak, 1e-12);
    double worst_abs = 0.0;
    for (std::size_t i = 0; i < direct.size(); ++i) {
        worst_abs = std::max(worst_abs, std::abs(fast[i] - direct[i]));
    }
    return worst_abs / denom;
}

constexpr double kFastTolerance = 1e-10;  // matches mdct.hpp's own documented bound

std::array<double, 256> forward512(const std::array<double, 512>& input, bool fast) {
    std::array<double, 512> windowed{};
    std::array<double, 256> coeffs{};
    ac3::apply_analysis_window(input, windowed);
    ac3::mdct512_forward(windowed, coeffs, fast);
    return coeffs;
}

struct ShortCoeffs {
    std::array<double, 128> first;
    std::array<double, 128> second;
};

ShortCoeffs forward256_pair(const std::array<double, 512>& input, bool fast) {
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(input, windowed);
    const std::span<const double, 512> full(windowed);
    ShortCoeffs out{};
    ac3::mdct256_forward_first(full.first<256>(), out.first, fast);
    ac3::mdct256_forward_second(full.last<256>(), out.second, fast);
    return out;
}

std::array<double, 512> random_block(std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::array<double, 512> block{};
    for (auto& s : block) {
        s = dist(rng);
    }
    return block;
}

// Three real-audio-shaped blocks (not silence, not a single frequency): a
// couple of tones summed together, at different phases per block so the
// three cover different spectral shapes the way consecutive real frames
// would.
std::array<double, 512> tone_block(int block_offset) {
    std::array<double, 512> block{};
    for (int n = 0; n < 512; ++n) {
        const double t = static_cast<double>(block_offset * 256 + n) / 48000.0;
        block[static_cast<std::size_t>(n)] =
            0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * t) +
            0.15 * std::sin(2.0 * std::numbers::pi * 2500.0 * t);
    }
    return block;
}

}  // namespace

TEST_CASE("fast mdct512_forward agrees with the direct form on numpy goldens", "[mdct][fast]") {
    const auto check = [](const std::array<double, 512>& input,
                          std::span<const double> expected) {
        const auto direct = forward512(input, false);
        const auto fast = forward512(input, true);
        CHECK(max_rel_error(direct, expected) < 1e-9);  // sanity: direct still matches goldens
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    };
    check(ac3::golden::kGoldenImpulse0Input, ac3::golden::kGoldenImpulse0Coeffs);
    check(ac3::golden::kGoldenImpulse100Input, ac3::golden::kGoldenImpulse100Coeffs);
    check(ac3::golden::kGoldenDcInput, ac3::golden::kGoldenDcCoeffs);
    check(ac3::golden::kGoldenSineInput, ac3::golden::kGoldenSineCoeffs);
    check(ac3::golden::kGoldenRandomInput, ac3::golden::kGoldenRandomCoeffs);
}

TEST_CASE("fast mdct512_forward agrees with the direct form on random data", "[mdct][fast]") {
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed);
        const auto block = random_block(0x5eed0000U + seed);
        const auto direct = forward512(block, false);
        const auto fast = forward512(block, true);
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    }
}

TEST_CASE("fast mdct512_forward agrees with the direct form on real audio", "[mdct][fast]") {
    // 3+ frames of real (tone) audio, never silence or a single sample - see
    // this project's own "silence gives false passes" lesson, which applies
    // just as much to a numerical-agreement test as to a codec-correctness
    // one: a degenerate input can trivially satisfy a tolerance check
    // without exercising the algorithm's real behaviour.
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        const auto input = tone_block(block);
        const auto direct = forward512(input, false);
        const auto fast = forward512(input, true);
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    }
}

TEST_CASE("fast mdct256_forward_first/second agree with their direct forms",
         "[mdct][fast]") {
    // Each short transform's fold is verified against ITS OWN direct-form
    // table, separately - the lesson behind this file's whole existence: an
    // earlier attempt reused the alpha = 0 fold for alpha = -1 on the
    // reasoning that "phi_k(-1) = 0 makes them the same formula", and a
    // numerical check exactly like this one is what caught the ~1.85
    // relative error before it reached a bitstream. alpha = -1's fold is
    // the DCT-IV of the antisymmetric half-fold; alpha = +1's reaches the
    // same core through the DST-IV reversal identity - two different
    // derivations, each of which must independently survive this bound.
    const auto check = [](const std::array<double, 512>& input) {
        const auto direct = forward256_pair(input, false);
        const auto fast = forward256_pair(input, true);
        CHECK(max_rel_error(fast.first, direct.first) < kFastTolerance);
        CHECK(max_rel_error(fast.second, direct.second) < kFastTolerance);
    };
    check(ac3::golden::kGoldenSineInput);
    check(ac3::golden::kGoldenRandomInput);
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed);
        check(random_block(0x5ec00000U + seed));
    }
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        check(tone_block(block));
    }
}

// --- The inverse transforms' fast paths (phase 6: IMDCT step 3 via the
// radix-2 FFT instead of the pseudocode's direct O(N^2) sum; see mdct.hpp's
// inverse doc comment). Same standard as the forward's: the fast path must
// agree with the direct evaluation - the spec's own statement of the
// transform - to the same documented bound, on transform-shaped spectra and
// on arbitrary coefficient sets alike, before anything defaults it on.

TEST_CASE("fast imdct512_windowed agrees with the direct 7.9.4.1 evaluation",
          "[mdct][fast]") {
    const auto check = [](const std::array<double, 256>& coeffs) {
        std::array<double, 512> direct{};
        std::array<double, 512> fast{};
        ac3::imdct512_windowed(coeffs, direct, false);
        ac3::imdct512_windowed(coeffs, fast, true);
        const double err = max_rel_error(fast, direct);
        CAPTURE(err);
        CHECK(err < kFastTolerance);
    };
    // Transform-shaped spectra: what a decoder actually feeds this.
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        check(forward512(tone_block(block), false));
    }
    // Arbitrary dense spectra: every bin loaded, no structure to hide in.
    for (std::uint32_t seed = 1; seed <= 5; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::array<double, 256> coeffs{};
        for (auto& c : coeffs) {
            c = dist(rng);
        }
        check(coeffs);
    }
}

TEST_CASE("fast imdct256_pair_windowed agrees with the direct 7.9.4.2 evaluation",
          "[mdct][fast]") {
    const auto check = [](const std::array<double, 256>& coeffs) {
        std::array<double, 512> direct{};
        std::array<double, 512> fast{};
        ac3::imdct256_pair_windowed(coeffs, direct, false);
        ac3::imdct256_pair_windowed(coeffs, fast, true);
        const double err = max_rel_error(fast, direct);
        CAPTURE(err);
        CHECK(err < kFastTolerance);
    };
    // A block-switched block's coefficient set as the encoder interleaves it
    // (§7.9.2: X[2k] = first[k], X[2k+1] = second[k]).
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        const auto pair = forward256_pair(tone_block(block), false);
        std::array<double, 256> coeffs{};
        for (std::size_t k = 0; k < 128; ++k) {
            coeffs[2 * k] = pair.first[k];
            coeffs[2 * k + 1] = pair.second[k];
        }
        check(coeffs);
    }
    for (std::uint32_t seed = 11; seed <= 15; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::array<double, 256> coeffs{};
        for (auto& c : coeffs) {
            c = dist(rng);
        }
        check(coeffs);
    }
}

// --- the float32 inverse (roadmap PF7's float32 gap) -------------------------
//
// imdct512_windowed has a float32 overload for the minimum-footprint profile,
// where the target's FPU is single-precision and every double is either a
// software-emulated multiply (arm-none-eabi) or a wasted one (Xtensa LX7). It
// is the SAME code - mdct.cpp's imdct512_windowed_impl templated on the scalar
// type - so what needs establishing is not that it computes the right thing but
// what it COSTS to compute it in float.
//
// The bound below is therefore a measurement, not an aspiration, and it is
// deliberately not kFastTolerance: 1e-10 is six decimal digits tighter than
// float32's own 1.19e-7 epsilon, so holding a float path to it would be asking
// for something the type cannot represent.
//
// What the right bound is, stated carefully because the obvious framing is
// wrong: float32 has a 24-bit mantissa, so its epsilon IS a 24-bit LSB - the
// two are the same number, not orders apart. A float32 transform therefore
// cannot be good to better than about one LSB at 24-bit, and the measured
// figure here is 2.75e-7, roughly 2.3 epsilons, which is the handful of
// rounding steps a 128-point FFT accumulates.
//
// That is the correct thing to compare against the CODEC's own noise floor
// rather than against the output word length. AC-3 quantises mantissas to at
// most 16 bits and usually far fewer, so 2.3 LSB at 24-bit sits well below the
// coding noise already present in any real stream - which is why this is
// acceptable for a decoder and would not be for, say, a mastering transform.
TEST_CASE("float32 inverse transform agrees with the double one", "[mdct][float32]") {
    std::mt19937 rng(20260907);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    double worst = 0.0;
    for (int trial = 0; trial < 32; ++trial) {
        std::array<double, 256> coeffs_d{};
        std::array<float, 256> coeffs_f{};
        for (std::size_t i = 0; i < coeffs_d.size(); ++i) {
            coeffs_d[i] = dist(rng);
            coeffs_f[i] = static_cast<float>(coeffs_d[i]);
        }

        std::array<double, 512> x_d{};
        std::array<float, 512> x_f{};
        ac3::imdct512_windowed(coeffs_d, x_d, /*fast=*/true);
        ac3::imdct512_windowed(coeffs_f, x_f);

        std::array<double, 512> widened{};
        for (std::size_t i = 0; i < widened.size(); ++i) {
            widened[i] = static_cast<double>(x_f[i]);
        }
        worst = std::max(worst, max_rel_error(widened, x_d));
    }

    // Peak-normalised, so this is "how far apart are the two spectra relative
    // to the signal in them" - the same question kFastTolerance answers for
    // fast-vs-direct, asked of double-vs-float.
    INFO("worst peak-normalised float32-vs-double error: " << worst);
    CHECK(worst < 1e-5);
    // And it should not be absurdly small either - a float32 path that agreed
    // to 1e-12 would mean the float overload had quietly computed in double,
    // which is exactly the trap fft_kernel.hpp's Scalar parameter exists to
    // avoid. Catching that here is cheaper than noticing it as a performance
    // mystery on the target.
    CHECK(worst > 1e-9);
}

TEST_CASE("float32 forward transform agrees with the double one", "[mdct][float32]") {
    // The forward direction had no float32 form until oba::joc needed one: it
    // analyses the bed inside a DECODE before un-mixing it (PF8), which is the
    // only forward transform a decode runs. The encoder's own forward path is
    // still double and is not on this one.
    std::mt19937 rng(20260908);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    double worst_window = 0.0;
    double worst_forward = 0.0;
    for (int trial = 0; trial < 32; ++trial) {
        std::array<double, 512> raw_d{};
        std::array<float, 512> raw_f{};
        for (std::size_t i = 0; i < raw_d.size(); ++i) {
            raw_d[i] = dist(rng);
            raw_f[i] = static_cast<float>(raw_d[i]);
        }

        std::array<double, 512> win_d{};
        std::array<float, 512> win_f{};
        ac3::apply_analysis_window(raw_d, win_d);
        ac3::apply_analysis_window(raw_f, win_f);

        std::array<double, 512> win_widened{};
        for (std::size_t i = 0; i < win_widened.size(); ++i) {
            win_widened[i] = static_cast<double>(win_f[i]);
        }
        worst_window = std::max(worst_window, max_rel_error(win_widened, win_d));

        std::array<double, 256> coeffs_d{};
        std::array<float, 256> coeffs_f{};
        ac3::mdct512_forward(win_d, coeffs_d, /*fast=*/true);
        ac3::mdct512_forward(win_f, coeffs_f);

        std::array<double, 256> widened{};
        for (std::size_t i = 0; i < widened.size(); ++i) {
            widened[i] = static_cast<double>(coeffs_f[i]);
        }
        worst_forward = std::max(worst_forward, max_rel_error(widened, coeffs_d));
    }

    INFO("worst peak-normalised float32-vs-double window error: " << worst_window);
    CHECK(worst_window < 1e-6);

    INFO("worst peak-normalised float32-vs-double forward error: " << worst_forward);
    CHECK(worst_forward < 1e-5);
    // The same trap the inverse's test guards: agreement to 1e-12 would mean
    // the float overload had quietly computed in double, which the Scalar
    // template parameter exists to prevent and which is far cheaper to catch
    // here than as a performance mystery on the target.
    CHECK(worst_forward > 1e-9);
}

TEST_CASE("float32 batch transforms agree with their scalar float forms", "[mdct][float32]") {
    // The float32 batch entry points are four independent calls rather than
    // four SIMD lanes - there is no float32 AVX2 kernel for them to fill yet.
    // So this is an EXACT check, not a tolerance one: any difference at all
    // would mean the batch form had stopped being the scalar one repeated,
    // which is the whole of what it currently promises.
    std::mt19937 rng(20260908);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    std::array<std::array<float, 512>, 4> win{};
    for (auto& block : win) {
        for (auto& sample : block) {
            sample = dist(rng);
        }
    }

    std::array<std::array<float, 256>, 4> batched{};
    ac3::mdct512_forward_batch4(win[0], win[1], win[2], win[3], batched[0], batched[1],
                                batched[2], batched[3]);
    for (std::size_t lane = 0; lane < win.size(); ++lane) {
        CAPTURE(lane);
        std::array<float, 256> one{};
        ac3::mdct512_forward(win[lane], one);
        CHECK(std::ranges::equal(one, batched[lane]));
    }

    std::array<std::array<float, 256>, 4> coeffs{};
    for (auto& block : coeffs) {
        for (auto& value : block) {
            value = dist(rng);
        }
    }

    std::array<std::array<float, 512>, 4> batched_inv{};
    ac3::imdct512_windowed_batch4(coeffs[0], coeffs[1], coeffs[2], coeffs[3], batched_inv[0],
                                  batched_inv[1], batched_inv[2], batched_inv[3]);
    for (std::size_t lane = 0; lane < coeffs.size(); ++lane) {
        CAPTURE(lane);
        std::array<float, 512> one{};
        ac3::imdct512_windowed(coeffs[lane], one);
        CHECK(std::ranges::equal(one, batched_inv[lane]));
    }
}

TEST_CASE("float32 short-block inverse agrees with the double one", "[mdct][float32]") {
    std::mt19937 rng(20260908);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    double worst = 0.0;
    for (int trial = 0; trial < 32; ++trial) {
        std::array<double, 256> coeffs_d{};
        std::array<float, 256> coeffs_f{};
        for (std::size_t i = 0; i < coeffs_d.size(); ++i) {
            coeffs_d[i] = dist(rng);
            coeffs_f[i] = static_cast<float>(coeffs_d[i]);
        }

        std::array<double, 512> x_d{};
        std::array<float, 512> x_f{};
        ac3::imdct256_pair_windowed(coeffs_d, x_d, /*fast=*/true);
        ac3::imdct256_pair_windowed(coeffs_f, x_f);

        std::array<double, 512> widened{};
        for (std::size_t i = 0; i < widened.size(); ++i) {
            widened[i] = static_cast<double>(x_f[i]);
        }
        worst = std::max(worst, max_rel_error(widened, x_d));
    }

    // Expected a shade better than the long transform's 2.75e-7: this runs two
    // 64-point FFTs rather than one 128-point, so there is one fewer radix
    // stage for rounding to accumulate through.
    INFO("worst peak-normalised float32-vs-double error: " << worst);
    CHECK(worst < 1e-5);
    CHECK(worst > 1e-9);
}

TEST_CASE("float32 short-block forward transforms agree with the double ones",
          "[mdct][float32]") {
    // The encoders' analysis front end under the minimum-footprint profile
    // runs the block-switched pair in float too (ac3/internal/encode_scalar.hpp),
    // through these forms; each is its double sibling's own fold in float.
    std::mt19937 rng(20260910);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    double worst = 0.0;
    for (int trial = 0; trial < 32; ++trial) {
        std::array<double, 512> win_d{};
        std::array<float, 512> win_f{};
        for (std::size_t i = 0; i < win_d.size(); ++i) {
            win_d[i] = dist(rng);
            win_f[i] = static_cast<float>(win_d[i]);
        }
        const std::span<const double, 512> full_d(win_d);
        const std::span<const float, 512> full_f(win_f);
        std::array<double, 128> first_d{};
        std::array<double, 128> second_d{};
        std::array<float, 128> first_f{};
        std::array<float, 128> second_f{};
        ac3::mdct256_forward_first(full_d.first<256>(), first_d, /*fast=*/true);
        ac3::mdct256_forward_second(full_d.last<256>(), second_d, /*fast=*/true);
        ac3::mdct256_forward_first(full_f.first<256>(), first_f);
        ac3::mdct256_forward_second(full_f.last<256>(), second_f);
        std::array<double, 128> widened{};
        for (std::size_t i = 0; i < widened.size(); ++i) {
            widened[i] = static_cast<double>(first_f[i]);
        }
        worst = std::max(worst, max_rel_error(widened, first_d));
        for (std::size_t i = 0; i < widened.size(); ++i) {
            widened[i] = static_cast<double>(second_f[i]);
        }
        worst = std::max(worst, max_rel_error(widened, second_d));
    }
    INFO("worst peak-normalised float32-vs-double short-block error: " << worst);
    CHECK(worst < 1e-5);
    CHECK(worst > 1e-9);
}
