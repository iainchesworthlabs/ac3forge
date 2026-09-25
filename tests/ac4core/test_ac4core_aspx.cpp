// A-SPX in the AC-4 shared core (src/ac4core/src/aspx): the subband group
// tables of ETSI TS 103 190-1 V1.4.1 Pseudocodes 67 to 74, worked by hand for
// the two configurations DEE's 2.0 streams use and checked for their
// invariants over every configuration a stream can select; and the high
// frequency generator's parts, Pseudocodes 85 to 89, against signals whose
// answer is known.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "aspx/frequency_tables.hpp"
#include "aspx/hf_generator.hpp"

namespace {

namespace aspx = ac4::detail::aspx;
using Complex = std::complex<double>;

template <std::size_t N>
std::vector<int> first(const std::array<std::uint8_t, N>& table, int count) {
    return {table.begin(), table.begin() + count};
}

aspx::SubbandGroups groups_for(const aspx::FrequencyConfig& config) {
    aspx::SubbandGroups groups;
    REQUIRE(aspx::derive_subband_groups(config, groups) == aspx::GroupsError::kNone);
    return groups;
}

bool increasing(std::span<const int> values) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] <= values[i - 1]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("the subband groups, patches and limiter of DEE's 128 kbps stereo configuration",
          "[ac4core][aspx]") {
    // aspx_master_freq_scale 1, aspx_start_freq 6, aspx_stop_freq 1,
    // aspx_noise_sbg 3, aspx_xover_subband_offset 0, worked through
    // Pseudocodes 67 to 74 by hand.
    const aspx::SubbandGroups g = groups_for({.master_freq_scale = 1,
                                              .start_freq = 6,
                                              .stop_freq = 1,
                                              .noise_sbg = 3,
                                              .xover_subband_offset = 0});
    CHECK(first(g.sbg_master, g.num_sbg_master + 1) ==
          std::vector<int>{36, 38, 40, 42, 44, 47, 50, 53, 56});
    CHECK(g.sba == 36);
    CHECK(g.sbz == 56);
    CHECK(g.sbx == 36);
    CHECK(g.num_sb_aspx == 20);
    CHECK(first(g.sbg_sig_highres, g.num_sbg_sig_highres + 1) ==
          first(g.sbg_master, g.num_sbg_master + 1));
    CHECK(first(g.sbg_sig_lowres, g.num_sbg_sig_lowres + 1) ==
          std::vector<int>{36, 40, 44, 50, 56});
    // max(1, floor(3 log2(56/36) + 0.5)) = 2.
    CHECK(first(g.sbg_noise, g.num_sbg_noise + 1) == std::vector<int>{36, 44, 56});

    aspx::PatchTables at48;
    REQUIRE(aspx::derive_patch_tables(g, 1, true, at48));
    CHECK(first(at48.sbg_patch_num_sb, at48.num_sbg_patches) == std::vector<int>{8, 12});
    CHECK(first(at48.sbg_patch_start_sb, at48.num_sbg_patches) == std::vector<int>{28, 24});
    CHECK(first(at48.sbg_patches, at48.num_sbg_patches + 1) == std::vector<int>{36, 44, 56});
    CHECK(first(at48.sbg_lim, at48.num_sbg_lim + 1) == std::vector<int>{36, 44, 56});

    // At 44.1 kHz goal_sb is 46, so the first patch reaches up to 47.
    aspx::PatchTables at44;
    REQUIRE(aspx::derive_patch_tables(g, 1, false, at44));
    CHECK(first(at44.sbg_patch_num_sb, at44.num_sbg_patches) == std::vector<int>{11, 9});
    CHECK(first(at44.sbg_patch_start_sb, at44.num_sbg_patches) == std::vector<int>{24, 27});
    CHECK(first(at44.sbg_patches, at44.num_sbg_patches + 1) == std::vector<int>{36, 47, 56});
    CHECK(first(at44.sbg_lim, at44.num_sbg_lim + 1) == std::vector<int>{36, 47, 56});
}

TEST_CASE("the subband groups, patches and limiter of DEE's 48 kbps stereo configuration",
          "[ac4core][aspx]") {
    const aspx::SubbandGroups g = groups_for({.master_freq_scale = 0,
                                              .start_freq = 5,
                                              .stop_freq = 0,
                                              .noise_sbg = 3,
                                              .xover_subband_offset = 0});
    CHECK(first(g.sbg_master, g.num_sbg_master + 1) ==
          std::vector<int>{20, 22, 24, 26, 28, 30, 32, 35, 38, 42, 46});
    CHECK(first(g.sbg_sig_lowres, g.num_sbg_sig_lowres + 1) ==
          std::vector<int>{20, 24, 28, 32, 38, 46});
    // max(1, floor(3 log2(46/20) + 0.5)) = 4.
    CHECK(first(g.sbg_noise, g.num_sbg_noise + 1) == std::vector<int>{20, 24, 28, 32, 46});
    for (const bool base_48k : {true, false}) {
        aspx::PatchTables p;
        REQUIRE(aspx::derive_patch_tables(g, 0, base_48k, p));
        CHECK(first(p.sbg_patch_num_sb, p.num_sbg_patches) == std::vector<int>{18, 8});
        CHECK(first(p.sbg_patch_start_sb, p.num_sbg_patches) == std::vector<int>{2, 12});
        CHECK(first(p.sbg_patches, p.num_sbg_patches + 1) == std::vector<int>{20, 38, 46});
        // 28 is removed (under 0.245 octave above 24, and no patch border), and
        // the patch border 38 that lands on a group border once.
        CHECK(first(p.sbg_lim, p.num_sbg_lim + 1) == std::vector<int>{20, 24, 32, 38, 46});
    }
}

TEST_CASE("every configuration a stream can select gives tables that hold together",
          "[ac4core][aspx]") {
    int configurations = 0;
    int noise_refusals = 0;
    int patches_short = 0;
    int limiter_short = 0;
    for (const int scale : {0, 1}) {
        for (int start = 0; start < 8; ++start) {
            for (int stop = 0; stop < 4; ++stop) {
                for (int noise = 0; noise < 4; ++noise) {
                    for (int xover = 0; xover < 32; ++xover) {
                        aspx::SubbandGroups g;
                        const aspx::GroupsError error =
                            aspx::derive_subband_groups({.master_freq_scale = scale,
                                                         .start_freq = start,
                                                         .stop_freq = stop,
                                                         .noise_sbg = noise,
                                                         .xover_subband_offset = xover},
                                                        g);
                        if (error == aspx::GroupsError::kXoverOffset) {
                            continue;
                        }
                        if (error == aspx::GroupsError::kNoiseGroups) {
                            ++noise_refusals;
                            continue;
                        }
                        ++configurations;
                        CAPTURE(scale, start, stop, noise, xover);
                        const std::vector<int> master = first(g.sbg_master, g.num_sbg_master + 1);
                        const std::vector<int> high =
                            first(g.sbg_sig_highres, g.num_sbg_sig_highres + 1);
                        const std::vector<int> low =
                            first(g.sbg_sig_lowres, g.num_sbg_sig_lowres + 1);
                        const std::vector<int> noise_groups =
                            first(g.sbg_noise, g.num_sbg_noise + 1);
                        CHECK(increasing(master));
                        CHECK(increasing(high));
                        CHECK(increasing(low));
                        CHECK(increasing(noise_groups));
                        CHECK(low.front() == g.sbx);
                        CHECK(low.back() == g.sbz);
                        CHECK(noise_groups.back() == g.sbz);
                        for (const int border : low) {
                            CHECK(std::ranges::find(high, border) != high.end());
                        }
                        for (const bool base_48k : {true, false}) {
                            aspx::PatchTables p;
                            REQUIRE(aspx::derive_patch_tables(g, scale, base_48k, p));
                            CHECK(p.num_sbg_patches >= 1);
                            CHECK(p.num_sbg_patches <= aspx::kMaxPatches);
                            for (int i = 0; i < p.num_sbg_patches; ++i) {
                                CHECK(p.sbg_patch_num_sb[static_cast<std::size_t>(i)] > 0);
                                CHECK(p.sbg_patch_start_sb[static_cast<std::size_t>(i)] +
                                          p.sbg_patch_num_sb[static_cast<std::size_t>(i)] <=
                                      g.sba);
                            }
                            const std::vector<int> borders =
                                first(p.sbg_patches, p.num_sbg_patches + 1);
                            CHECK(borders.front() == g.sbx);
                            CHECK(borders.back() <= g.sbz);
                            patches_short += borders.back() < g.sbz ? 1 : 0;
                            const std::vector<int> lim = first(p.sbg_lim, p.num_sbg_lim + 1);
                            CHECK(increasing(lim));
                            CHECK(lim.front() == g.sbx);
                            CHECK(lim.back() <= g.sbz);
                            limiter_short += lim.back() < g.sbz ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    // The counts src/ac4dec/ERRATA.md cites ("The limiter's last group").
    CHECK(configurations == 2811);
    CHECK(noise_refusals == 5);
    CHECK(patches_short == 232);
    CHECK(limiter_short == 168);
}

TEST_CASE("the linear prediction of Pseudocodes 86 and 87 finds a two-slot recursion",
          "[ac4core][aspx]") {
    // One subband following x[n] = -a0 x[n-2] - a1 x[n-4] exactly: the
    // whitening filter 1 + a0 z^-2 + a1 z^-4 leaves nothing, so the
    // covariance method returns (a0, a1), up to Pseudocode 87's 2^-20, which
    // this recursion's conditioning multiplies about tenfold.
    const Complex a0(0.3, -0.4);
    const Complex a1(-0.2, 0.1);
    constexpr int kSlots = 42;  // 32 + ts_offset_hfgen 6 + ts_offset_hfadj 4
    std::vector<Complex> ext(static_cast<std::size_t>(kSlots) * 64);
    const auto x = [&](int n) -> Complex& { return ext[static_cast<std::size_t>(n) * 64 + 5]; };
    x(0) = Complex(1.0, 0.0);
    x(1) = Complex(0.0, 1.0);
    x(2) = Complex(-0.5, 0.25);
    x(3) = Complex(0.75, -1.0);
    for (int n = 4; n < kSlots; ++n) {
        x(n) = -a0 * x(n - 2) - a1 * x(n - 4);
    }
    std::array<Complex, 64> alpha0{};
    std::array<Complex, 64> alpha1{};
    aspx::prediction_coefficients<double>(ext, kSlots, 8, alpha0, alpha1);
    CHECK(std::abs(alpha0[5] - a0) < 1e-4);
    CHECK(std::abs(alpha1[5] - a1) < 1e-4);
    // An empty subband predicts nothing.
    CHECK(alpha0[3] == Complex{});
    CHECK(alpha1[3] == Complex{});
}

TEST_CASE("pre-flattening flattens an envelope that is a cubic in dB exactly", "[ac4core][aspx]") {
    // |Q[sb]|^2 = 10^(p(sb)/10) - 1 on every slot, so Pseudocode 85's
    // 10 log10(energy + 1) is p(sb), which the cubic fit returns unchanged.
    constexpr int kSbx = 20;
    const auto p = [](double sb) {
        return 60.0 + 1.5 * sb - 0.12 * sb * sb + 0.002 * sb * sb * sb;
    };
    std::vector<Complex> q_low(38 * 64);
    for (int ts = 0; ts < 38; ++ts) {
        for (int sb = 0; sb < kSbx; ++sb) {
            q_low[static_cast<std::size_t>(ts) * 64 + static_cast<std::size_t>(sb)] =
                std::polar(std::sqrt(std::pow(10.0, p(sb) / 10.0) - 1.0), 0.1 * ts * sb);
        }
    }
    double mean = 0.0;
    for (int sb = 0; sb < kSbx; ++sb) {
        mean += p(sb) / kSbx;
    }
    std::array<double, 64> gain{};
    aspx::preflattening_gains<double>(q_low, kSbx, 0, 32, gain);
    for (int sb = 0; sb < kSbx; ++sb) {
        CAPTURE(sb);
        CHECK(std::abs(gain[static_cast<std::size_t>(sb)] / std::pow(10.0, (mean - p(sb)) / 20.0) -
                       1.0) < 1e-9);
    }
}

TEST_CASE("with no tonal adjustment the generator copies each patch's source subbands up",
          "[ac4core][aspx]") {
    const aspx::SubbandGroups g = groups_for({.master_freq_scale = 1,
                                              .start_freq = 6,
                                              .stop_freq = 1,
                                              .noise_sbg = 3,
                                              .xover_subband_offset = 0});
    aspx::PatchTables p;
    REQUIRE(aspx::derive_patch_tables(g, 1, true, p));
    std::mt19937 rng(89);
    std::normal_distribution<double> normal;
    std::vector<Complex> ext(42 * 64);
    for (Complex& v : ext) {
        v = Complex(normal(rng), normal(rng));
    }
    std::vector<Complex> q_high(38 * 64, Complex(7.0, 7.0));
    const std::array<std::uint8_t, 5> none{};
    aspx::HfGeneratorState<double> state;
    const aspx::HfGeneratorInput<double> in{
        .q_low_ext = ext,
        .num_qmf_timeslots = 32,
        .ts_offset_hfgen = 6,
        .ts_begin = 0,
        .ts_end = 32,
        .preflat = false,
        .tna_mode = std::span<const std::uint8_t>(none).first(2)};
    aspx::generate_high_band<double>(g, p, in, state, q_high);
    // Patch 0 takes subbands 28 to 35 to 36 to 43, patch 1 24 to 35 to 44 to 55.
    for (int ts = 0; ts < 32; ++ts) {
        for (int sb = 36; sb < 56; ++sb) {
            const int source = sb < 44 ? 28 + (sb - 36) : 24 + (sb - 44);
            CHECK(q_high[static_cast<std::size_t>(ts) * 64 + static_cast<std::size_t>(sb)] ==
                  ext[static_cast<std::size_t>(ts + 4) * 64 + static_cast<std::size_t>(source)]);
        }
        // Outside the A-SPX range, and past the interval, nothing is touched.
        CHECK(q_high[static_cast<std::size_t>(ts) * 64 + 35] == Complex(7.0, 7.0));
        CHECK(q_high[static_cast<std::size_t>(ts) * 64 + 56] == Complex(7.0, 7.0));
    }
    CHECK(q_high[32 * 64 + 40] == Complex(7.0, 7.0));
}

TEST_CASE("the chirp factors follow Table 195 and Pseudocode 88's smoothing", "[ac4core][aspx]") {
    const aspx::SubbandGroups g = groups_for({.master_freq_scale = 1,
                                              .start_freq = 6,
                                              .stop_freq = 1,
                                              .noise_sbg = 3,
                                              .xover_subband_offset = 0});
    aspx::PatchTables p;
    REQUIRE(aspx::derive_patch_tables(g, 1, true, p));
    std::vector<Complex> ext(42 * 64);
    std::vector<Complex> q_high(38 * 64);
    aspx::HfGeneratorState<double> state;
    const auto run = [&](std::uint8_t mode) {
        const std::array<std::uint8_t, 5> modes{mode, mode, 0, 0, 0};
        aspx::generate_high_band<double>(
            g, p,
            {.q_low_ext = ext,
             .num_qmf_timeslots = 32,
             .ts_offset_hfgen = 6,
             .ts_begin = 0,
             .ts_end = 32,
             .preflat = false,
             .tna_mode = std::span<const std::uint8_t>(modes).first(2)},
            state, q_high);
    };
    run(3);  // Heavy after None: 0.98, rising, so 0.90625 * 0.98 + 0.09375 * 0
    CHECK(std::abs(state.chirp_prev[0] - 0.888125) < 1e-12);
    CHECK(state.tna_mode_prev[0] == 3);
    run(0);  // None after Heavy: 0, falling, so 0.25 of the last
    CHECK(std::abs(state.chirp_prev[0] - 0.22203125) < 1e-12);
    run(0);  // None after None: 0.0555078125
    CHECK(std::abs(state.chirp_prev[0] - 0.0555078125) < 1e-12);
    run(0);  // 0.013876953125, under 1/64: 0
    CHECK(state.chirp_prev[0] == 0.0);
    CHECK(state.chirp_prev[2] == 0.0);  // past num_sbg_noise
}
