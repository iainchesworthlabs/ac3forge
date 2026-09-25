// The encoder's channel data matrices undone (src/ac4enc/src/asf/multichannel.
// hpp) held to ETSI TS 103 190-1 V1.4.1 as printed: Tables 178 and 179 and
// clause 5.3.3.4's matrix, in the transcription the decoder's tests keep
// (tests/ac4dec/ac4dec_printed_matrices.hpp). A unit's output channels,
// turned into its tracks, must come back through the printed matrix of the
// parameters the encoder chose, or was given, band by band.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../ac4dec/ac4dec_printed_matrices.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/multichannel.hpp"
#include "asf/stereo.hpp"

namespace {

using ac4::detail::Channel;
using ac4::detail::StereoChoice;
using ac4dec_test::Abcd;

// A small linear congruential generator: the same draws everywhere.
struct Lcg {
    std::uint32_t state = 20260925U;
    std::uint32_t next() {
        state = state * 1664525U + 1013904223U;
        return state >> 8U;
    }
    double uniform() { return static_cast<double>(next()) / static_cast<double>(1U << 24U) * 2.0 - 1.0; }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
};

// A channel of random lines in a frame's layout, each band allowed a hundredth
// of its energy.
Channel random_channel(Lcg& rng, const ac4::detail::FrameLayout& layout, std::array<int, 2> max_sfb, double scale) {
    std::vector<double> spectrum(2048);
    for (double& x : spectrum) {
        x = scale * rng.uniform();
    }
    Channel c;
    c.grouped = ac4::detail::regroup(spectrum, layout, max_sfb);
    c.allowed.resize(c.grouped.offset.size());
    for (std::size_t g = 0; g < c.grouped.offset.size(); ++g) {
        for (std::size_t b = 0; b + 1 < c.grouped.offset[g].size(); ++b) {
            double energy = 0.0;
            for (std::size_t k = c.grouped.offset[g][b]; k < c.grouped.offset[g][b + 1]; ++k) {
                energy += c.grouped.lines[k] * c.grouped.lines[k];
            }
            c.allowed[g].push_back(energy / 100.0 + 1e-9);
        }
    }
    return c;
}

// A chparam_info() of random choices for the unit's bands.
StereoChoice random_choice(Lcg& rng, const Channel& c) {
    StereoChoice s;
    s.sap_mode = rng.below(4);
    const std::size_t groups = c.grouped.offset.size();
    s.ms_used.resize(groups);
    s.sap_used.resize(groups);
    s.alpha_q.resize(groups);
    for (std::size_t g = 0; g < groups; ++g) {
        const auto bands = static_cast<std::size_t>(c.grouped.max_sfb[g]);
        for (std::size_t b = 0; b < bands; ++b) {
            s.ms_used[g].push_back(rng.below(2) == 1);
        }
        for (std::size_t p = 0; p < (bands + 1) / 2; ++p) {
            s.sap_used[g].push_back(rng.below(3) != 0);
            s.alpha_q[g].push_back(rng.below(61) - 30);
        }
    }
    return s;
}

// Pseudocode 59's a, b, c and d for a band of a chparam_info(): M/S is
// L = X0 + X1, R = X0 - X1, and prediction L = (1 + a) X0 + X1,
// R = (1 - a) X0 - X1, with a = 0.1 alpha_q in single precision.
Abcd parameters(const StereoChoice& s, std::size_t g, std::size_t b) {
    const Abcd identity = {1.0, 0.0, 0.0, 1.0};
    const Abcd mid_side = {1.0, 1.0, 1.0, -1.0};
    switch (s.sap_mode) {
        case 1:
            return s.ms_used[g][b] ? mid_side : identity;
        case 2:
            return mid_side;
        case 3: {
            if (!s.sap_used[g][b / 2]) {
                return identity;
            }
            const auto a = static_cast<double>(static_cast<float>(s.alpha_q[g][b / 2]) * 0.1F);
            return {1.0 + a, 1.0, 1.0 - a, -1.0};
        }
        default:
            return identity;
    }
}

// Every line of `outputs`, before the unit's matrix was undone, is the printed
// matrix of the parameters `sets` chose, applied to the tracks now in `unit`.
void check_printed(std::string_view printed, std::span<const StereoChoice> sets, const std::vector<Channel>& outputs,
                   const std::vector<Channel*>& unit) {
    const ac4::detail::Grouped& first = unit.front()->grouped;
    std::size_t mismatches = 0;
    double worst = 0.0;
    for (std::size_t g = 0; g < first.offset.size(); ++g) {
        for (std::size_t b = 0; b + 1 < first.offset[g].size(); ++b) {
            std::vector<Abcd> p;
            for (const StereoChoice& set : sets) {
                p.push_back(parameters(set, g, b));
            }
            const auto m = ac4dec_test::printed_matrix(printed, p);
            for (std::size_t k = first.offset[g][b]; k < first.offset[g][b + 1]; ++k) {
                for (std::size_t o = 0; o < outputs.size(); ++o) {
                    double sum = 0.0;
                    for (std::size_t t = 0; t < unit.size(); ++t) {
                        sum += m[o][t] * unit[t]->grouped.lines[k];
                    }
                    const double want = outputs[o].grouped.lines[k];
                    const double error = std::abs(sum - want);
                    worst = std::max(worst, error);
                    if (error > 1e-9 * (1.0 + std::abs(want))) {
                        ++mismatches;
                    }
                }
            }
        }
    }
    CAPTURE(worst);
    CHECK(mismatches == 0);
}

const std::array<ac4::detail::FrameLayout, 2> kLayouts = {
    ac4::detail::long_layout(2048), ac4::detail::split_layout(2048, {0, 3}, {2, -1})};

std::array<int, 2> max_sfb_of(const ac4::detail::FrameLayout& layout) {
    return layout.long_frame ? std::array<int, 2>{40, 40} : std::array<int, 2>{12, 30};
}

}  // namespace

TEST_CASE("every chel_matsel's cascade undone comes back through Table 178 as printed", "[ac4enc][multichannel]") {
    Lcg rng;
    for (const auto& layout : kLayouts) {
        for (int matsel = 0; matsel < 12; ++matsel) {
            for (const bool chosen : {false, true}) {
                CAPTURE(matsel, chosen, layout.long_frame);
                std::vector<Channel> outputs;
                for (int c = 0; c < 3; ++c) {
                    outputs.push_back(random_channel(rng, layout, max_sfb_of(layout), 1.0 + c));
                }
                std::vector<Channel> unit = outputs;
                std::vector<StereoChoice> forced;
                if (!chosen) {
                    forced = {random_choice(rng, outputs[0]), random_choice(rng, outputs[0])};
                }
                const ac4::detail::UnitChoice choice =
                    ac4::detail::undo_three(matsel, {&unit[0], &unit[1], &unit[2]}, forced);
                REQUIRE(choice.sets.size() == 2);
                CHECK(choice.chel_matsel == matsel);
                CHECK(std::isfinite(choice.bits));
                check_printed(ac4dec_test::kTable178[static_cast<std::size_t>(matsel)], choice.sets, outputs,
                              {&unit[0], &unit[1], &unit[2]});
            }
        }
    }
}

TEST_CASE("four_channel_data()'s steps undone come back through clause 5.3.3.4's matrix", "[ac4enc][multichannel]") {
    Lcg rng;
    for (const auto& layout : kLayouts) {
        for (const bool chosen : {false, true}) {
            for (int draw = 0; draw < 4; ++draw) {
                CAPTURE(chosen, draw, layout.long_frame);
                std::vector<Channel> outputs;
                for (int c = 0; c < 4; ++c) {
                    outputs.push_back(random_channel(rng, layout, max_sfb_of(layout), 1.0 + c));
                }
                std::vector<Channel> unit = outputs;
                std::vector<StereoChoice> forced;
                if (!chosen) {
                    for (int s = 0; s < 4; ++s) {
                        forced.push_back(random_choice(rng, outputs[0]));
                    }
                }
                const ac4::detail::UnitChoice choice =
                    ac4::detail::undo_four({&unit[0], &unit[1], &unit[2], &unit[3]}, forced);
                REQUIRE(choice.sets.size() == 4);
                check_printed(ac4dec_test::kFourChannel, choice.sets, outputs, {&unit[0], &unit[1], &unit[2], &unit[3]});
            }
        }
    }
}

TEST_CASE("every chel_matsel's five channel matrix undone comes back through Table 179 as printed",
          "[ac4enc][multichannel]") {
    Lcg rng;
    for (const auto& layout : kLayouts) {
        for (int matsel = 0; matsel < 12; ++matsel) {
            for (const bool chosen : {false, true}) {
                CAPTURE(matsel, chosen, layout.long_frame);
                std::vector<Channel> outputs;
                for (int c = 0; c < 5; ++c) {
                    outputs.push_back(random_channel(rng, layout, max_sfb_of(layout), 1.0 + c));
                }
                std::vector<Channel> unit = outputs;
                std::vector<StereoChoice> forced;
                if (!chosen) {
                    for (int s = 0; s < 5; ++s) {
                        forced.push_back(random_choice(rng, outputs[0]));
                    }
                }
                const ac4::detail::UnitChoice choice =
                    ac4::detail::undo_five(matsel, {&unit[0], &unit[1], &unit[2], &unit[3], &unit[4]}, forced);
                REQUIRE(choice.sets.size() == 5);
                CHECK(choice.chel_matsel == matsel);
                check_printed(ac4dec_test::kTable179[static_cast<std::size_t>(matsel)], choice.sets, outputs,
                              {&unit[0], &unit[1], &unit[2], &unit[3], &unit[4]});
            }
        }
    }
}

TEST_CASE("a pair undone comes back through its chparam_info()'s matrix", "[ac4enc][multichannel]") {
    Lcg rng;
    for (const auto& layout : kLayouts) {
        for (const bool chosen : {false, true}) {
            for (int draw = 0; draw < 8; ++draw) {
                CAPTURE(chosen, draw, layout.long_frame);
                std::vector<Channel> outputs = {random_channel(rng, layout, max_sfb_of(layout), 1.0),
                                                random_channel(rng, layout, max_sfb_of(layout), 0.5)};
                // Half the draws share most of their content, so that the
                // chosen stereo processing is not always left and right.
                if (draw % 2 == 1) {
                    for (std::size_t k = 0; k < outputs[1].grouped.lines.size(); ++k) {
                        outputs[1].grouped.lines[k] += 0.8 * outputs[0].grouped.lines[k];
                    }
                }
                std::vector<Channel> unit = outputs;
                std::vector<StereoChoice> forced;
                if (!chosen) {
                    forced = {random_choice(rng, outputs[0])};
                }
                const ac4::detail::UnitChoice choice = ac4::detail::undo_pair({&unit[0], &unit[1]}, forced);
                REQUIRE(choice.sets.size() == 1);
                check_printed("a0 b0 | c0 d0", choice.sets, outputs, {&unit[0], &unit[1]});
            }
        }
    }
}

TEST_CASE("perceptual entropy prefers the matrix that takes out what the channels share", "[ac4enc][multichannel]") {
    // Three channels of one content at 1, 0.9 and 0.8: some chel_matsel must
    // cost fewer bits than every step left and right, which is what the
    // experimental coding configurations weigh.
    Lcg rng;
    const auto layout = ac4::detail::long_layout(2048);
    const Channel base = random_channel(rng, layout, {40, 40}, 1.0);
    std::vector<Channel> outputs(3, base);
    for (std::size_t c = 1; c < 3; ++c) {
        for (double& x : outputs[c].grouped.lines) {
            x *= 1.0 - 0.1 * static_cast<double>(c);
        }
    }
    std::vector<Channel> unit = outputs;
    StereoChoice identity;
    const std::vector<StereoChoice> left_right = {identity, identity};
    const double apart = ac4::detail::undo_three(0, {&unit[0], &unit[1], &unit[2]}, left_right).bits;
    double best = apart;
    for (int matsel = 0; matsel < 12; ++matsel) {
        unit = outputs;
        best = std::min(best, ac4::detail::undo_three(matsel, {&unit[0], &unit[1], &unit[2]}).bits);
    }
    CHECK(best < 0.6 * apart);
}
