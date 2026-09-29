// Part 2's channel renderer for the immersive element (src/ac4dec/src/pcm/
// renderer.hpp, ETSI TS 103 190-2 V1.3.1 clause 5.10.2) and the downmix stage
// that runs it (pcm/downmix.hpp): Tables 38 to 43 and 45 and 46 transcribed
// here a second time, as printed, and held against the renderer's matrices
// for every input and output configuration, with a distinct value for each
// custom downmix gain so that one in the wrong place shows; Table 130's
// defaults and clause 6.3.10.3.10's exception; the layouts decode() gives
// each target; which loudness correction each output takes (4.8.5.3); and the
// stage's matrices, the persistence of what a frame sends, and the Part 1
// steps to two channels and one that follow the renderer.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "pcm/downmix.hpp"
#include "pcm/renderer.hpp"
#include "syntax/metadata.hpp"
#include "syntax/presentation.hpp"

namespace {

namespace detail = ac4::detail;
using S = ac4::Speaker;

// The generalized rendering matrix's indices (5.10.2.2).
constexpr int kIndices = 14;
constexpr std::array<S, kIndices> kIndexSpeaker = {
    S::kLeft,         S::kRight,     S::kCentre,       S::kLeftSurround,  S::kRightSurround,
    S::kLeftBack,     S::kRightBack, S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft,
    S::kTopBackRight, S::kLfe,       S::kTopSideLeft,  S::kTopSideRight};

int index_of(S speaker) {
    const auto it = std::ranges::find(kIndexSpeaker, speaker);
    REQUIRE(it != kIndexSpeaker.end());
    return static_cast<int>(it - kIndexSpeaker.begin());
}

double db(double value) {
    return std::pow(10.0, value / 20.0);
}

// A value for each gain the tables name, each distinct from the others and
// from -3 dB.
constexpr double kGainB = 0.11;
constexpr double kGainT1 = 0.22;
constexpr std::array<double, 6> kGainT2 = {0.31, 0.32, 0.33, 0.34, 0.35, 0.36};

enum class G { k0, kM3, kB, kT1, kT2a, kT2b, kT2c, kT2d, kT2e, kT2f };

double value_of(G g) {
    switch (g) {
        case G::k0:
            return 1.0;
        case G::kM3:
            return db(-3.0);
        case G::kB:
            return kGainB;
        case G::kT1:
            return kGainT1;
        case G::kT2a:
            return kGainT2[0];
        case G::kT2b:
            return kGainT2[1];
        case G::kT2c:
            return kGainT2[2];
        case G::kT2d:
            return kGainT2[3];
        case G::kT2e:
            return kGainT2[4];
        case G::kT2f:
            return kGainT2[5];
    }
    return 0.0;
}

struct Entry {
    int out;
    int in;
    G gain;
};

using Entries = std::vector<Entry>;

// ri,i = 0 dB for each i of `indices`.
Entries diag(std::initializer_list<int> indices) {
    Entries out;
    for (const int i : indices) {
        out.push_back({i, i, G::k0});
    }
    return out;
}

Entries operator+(Entries a, const Entries& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// The configurations of Tables 34 and 44, by name.
enum class Config { k7X4, k7X2, k7X0, k5X4, k5X2, k5X0 };
constexpr std::array<Config, 6> kConfigs = {Config::k7X4, Config::k7X2, Config::k7X0,
                                            Config::k5X4, Config::k5X2, Config::k5X0};

std::string name_of(Config c) {
    switch (c) {
        case Config::k7X4:
            return "7.X.4";
        case Config::k7X2:
            return "7.X.2";
        case Config::k7X0:
            return "7.X.0";
        case Config::k5X4:
            return "5.X.4";
        case Config::k5X2:
            return "5.X.2";
        case Config::k5X0:
            return "5.X.0";
    }
    return "";
}

// Tables 38 to 43, the rows for the configurations an immersive element's
// source can have, as printed: [output][input].
Entries printed(Config output, Config input) {
    const Entries top_m3 = {{7, 12, G::kM3}, {9, 12, G::kM3}, {8, 13, G::kM3}, {10, 13, G::kM3}};
    const Entries back_b = {{3, 3, G::kB}, {3, 5, G::kB}, {4, 4, G::kB}, {4, 6, G::kB}};
    const Entries back_m3 = {{3, 3, G::kM3}, {3, 5, G::kM3}, {4, 4, G::kM3}, {4, 6, G::kM3}};
    const Entries t1 = {{12, 7, G::kT1}, {12, 9, G::kT1}, {13, 8, G::kT1}, {13, 10, G::kT1}};
    switch (output) {
        case Config::k7X4:  // Table 38
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
                case Config::k7X2:
                    return diag({0, 1, 2, 3, 4, 5, 6}) + top_m3;
                case Config::k7X0:
                    return diag({0, 1, 2, 3, 4, 5, 6});
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4, 7, 8, 9, 10});
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4}) + top_m3;
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
        case Config::k7X2:  // Table 39
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2, 3, 4, 5, 6}) + t1;
                case Config::k7X2:
                    return diag({0, 1, 2, 3, 4, 5, 6, 12, 13});
                case Config::k7X0:
                    return diag({0, 1, 2, 3, 4, 5, 6});
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4}) +
                           Entries{
                               {12, 7, G::kM3}, {12, 9, G::kM3}, {13, 8, G::kM3}, {13, 10, G::kM3}};
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4, 12, 13});
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
        case Config::k7X0:  // Table 40
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2, 3, 4, 5, 6}) +
                           Entries{{0, 7, G::kT2a},  {1, 8, G::kT2a},  {3, 7, G::kT2b},
                                   {4, 8, G::kT2b},  {5, 7, G::kT2c},  {6, 8, G::kT2c},
                                   {0, 9, G::kT2d},  {1, 10, G::kT2d}, {3, 9, G::kT2e},
                                   {4, 10, G::kT2e}, {5, 9, G::kT2f},  {6, 10, G::kT2f}};
                case Config::k7X2:
                    return diag({0, 1, 2, 3, 4, 5, 6}) +
                           Entries{{0, 12, G::kT2a}, {1, 13, G::kT2a}, {3, 12, G::kT2b},
                                   {4, 13, G::kT2b}, {5, 12, G::kT2c}, {6, 13, G::kT2c}};
                case Config::k7X0:
                    return diag({0, 1, 2, 3, 4, 5, 6});
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4}) +
                           Entries{{3, 7, G::kM3}, {4, 8, G::kM3}, {3, 9, G::kM3}, {4, 10, G::kM3}};
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4}) + Entries{{3, 12, G::kM3}, {4, 13, G::kM3}};
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
        case Config::k5X4:  // Table 41
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2, 7, 8, 9, 10}) + back_b;
                case Config::k7X2:
                    return diag({0, 1, 2}) + back_b + top_m3;
                case Config::k7X0:
                    return diag({0, 1, 2}) + back_m3;
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4, 7, 8, 9, 10});
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4}) + top_m3;
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
        case Config::k5X2:  // Table 42
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2}) + back_b + t1;
                case Config::k7X2:
                    return diag({0, 1, 2, 12, 13}) + back_b;
                case Config::k7X0:
                    return diag({0, 1, 2}) + back_m3;
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4}) + t1;
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4, 12, 13});
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
        case Config::k5X0:  // Table 43
            switch (input) {
                case Config::k7X4:
                    return diag({0, 1, 2}) + back_b + Entries{{0, 7, G::kT2a}, {1, 8, G::kT2a},
                                                              {3, 7, G::kT2b}, {4, 8, G::kT2b},
                                                              {0, 9, G::kT2d}, {1, 10, G::kT2d},
                                                              {3, 9, G::kT2e}, {4, 10, G::kT2e}};
                case Config::k7X2:
                    return diag({0, 1, 2}) + back_b +
                           Entries{{0, 12, G::kT2a},
                                   {1, 13, G::kT2a},
                                   {3, 12, G::kT2b},
                                   {4, 13, G::kT2b}};
                case Config::k7X0:
                    return diag({0, 1, 2}) + back_m3;
                case Config::k5X4:
                    return diag({0, 1, 2, 3, 4}) + Entries{{0, 7, G::kT2a}, {1, 8, G::kT2a},
                                                           {3, 7, G::kT2b}, {4, 8, G::kT2b},
                                                           {0, 9, G::kT2d}, {1, 10, G::kT2d},
                                                           {3, 9, G::kT2e}, {4, 10, G::kT2e}};
                case Config::k5X2:
                    return diag({0, 1, 2, 3, 4}) + Entries{{0, 12, G::kT2a},
                                                           {1, 13, G::kT2a},
                                                           {3, 12, G::kT2b},
                                                           {4, 13, G::kT2b}};
                case Config::k5X0:
                    return diag({0, 1, 2, 3, 4});
            }
            break;
    }
    return {};
}

ac4::DownmixTarget target_of(Config c) {
    switch (c) {
        case Config::k7X4:
            return ac4::DownmixTarget::k7X4;
        case Config::k7X2:
            return ac4::DownmixTarget::k7X2;
        case Config::k7X0:
            return ac4::DownmixTarget::k7X0;
        case Config::k5X4:
            return ac4::DownmixTarget::k5X4;
        case Config::k5X2:
            return ac4::DownmixTarget::k5X2;
        case Config::k5X0:
            return ac4::DownmixTarget::k5X;
    }
    return ac4::DownmixTarget::kAsCoded;
}

// The decoded channels of the 7.X.4 modes, as decode() names them.
std::vector<S> decoded_714(bool lfe) {
    std::vector<S> out = {S::kLeft, S::kRight, S::kCentre};
    if (lfe) {
        out.push_back(S::kLfe);
    }
    out.insert(out.end(), {S::kLeftSurround, S::kRightSurround, S::kLeftBack, S::kRightBack,
                           S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight});
    return out;
}

// Where each decoded channel enters the generalized matrix under `layout`
// in full decoding, by Tables 57 and 59 (src/ac4dec/ERRATA.md, "The
// renderer's input channel configuration"): -1 where the input configuration
// has no such channel.
int full_input(const detail::ImmersiveLayout& layout, S speaker) {
    const int i = index_of(speaker);
    if ((speaker == S::kLeftBack || speaker == S::kRightBack) && !layout.backs) {
        return -1;
    }
    if (i >= 7 && i <= 10) {
        if (layout.tops == 3) {
            return i;
        }
        if (layout.tops == 1) {  // Tsl and Tsr carried in Tfl and Tfr
            return speaker == S::kTopFrontLeft ? 12 : speaker == S::kTopFrontRight ? 13 : -1;
        }
        if (layout.tops == 2) {  // carried in Tbl and Tbr
            return speaker == S::kTopBackLeft ? 12 : speaker == S::kTopBackRight ? 13 : -1;
        }
        return -1;
    }
    return i;
}

// The layout of the source with `input`'s configuration, the top pair of a .2
// source carried as top_channels_present `two` says.
detail::ImmersiveLayout layout_of(Config input, int two, bool lfe) {
    detail::ImmersiveLayout layout;
    layout.backs = input == Config::k7X4 || input == Config::k7X2 || input == Config::k7X0;
    layout.tops = (input == Config::k7X4 || input == Config::k5X4)   ? 3
                  : (input == Config::k7X2 || input == Config::k5X2) ? two
                                                                     : 0;
    layout.lfe = lfe;
    layout.decoding = ac4::DecodingMode::kFull;
    return layout;
}

detail::RenderGains test_gains() {
    return {.gain_b = kGainB, .gain_t1 = kGainT1, .gain_t2 = kGainT2};
}

}  // namespace

TEST_CASE("the renderer's full decoding matrices are Tables 38 to 43 as printed",
          "[ac4dec][renderer]") {
    for (const bool lfe : {true, false}) {
        const std::vector<S> decoded = decoded_714(lfe);
        for (const Config input : kConfigs) {
            for (const int two : {1, 2}) {
                const detail::ImmersiveLayout layout = layout_of(input, two, lfe);
                if (two == 2 && layout.tops != 2) {
                    continue;  // only a .2 source carries its pair two ways
                }
                for (const Config output : kConfigs) {
                    CAPTURE(lfe, name_of(input), two, name_of(output));
                    const detail::RenderPlan plan = detail::render_plan(layout, target_of(output));
                    const auto m = detail::render_matrix(layout, decoded, plan, test_gains());
                    // The table's coefficients, and NOTE 2's LFE.
                    std::array<std::array<double, kIndices>, kIndices> table{};
                    for (const Entry& e : printed(output, input)) {
                        table[static_cast<std::size_t>(e.out)][static_cast<std::size_t>(e.in)] =
                            value_of(e.gain);
                    }
                    if (lfe) {
                        table[11][11] = 1.0;
                    }
                    REQUIRE(m.size() == plan.speakers.size());
                    for (std::size_t o = 0; o < plan.speakers.size(); ++o) {
                        const int out = index_of(plan.speakers[o]);
                        REQUIRE(m[o].size() == decoded.size());
                        for (std::size_t d = 0; d < decoded.size(); ++d) {
                            const int in = full_input(layout, decoded[d]);
                            const double expected = in < 0 ? 0.0
                                                           : table[static_cast<std::size_t>(out)]
                                                                  [static_cast<std::size_t>(in)];
                            CAPTURE(ac4::describe(plan.speakers[o]), ac4::describe(decoded[d]));
                            CHECK(std::abs(m[o][d] - expected) < 1e-12);
                        }
                    }
                    // Every coefficient of the table lands on a channel that
                    // comes out and one that goes in.
                    for (const Entry& e : printed(output, input)) {
                        const bool out_present = std::ranges::any_of(
                            plan.speakers, [&](S s) { return index_of(s) == e.out; });
                        const bool in_present = std::ranges::any_of(
                            decoded, [&](S s) { return full_input(layout, s) == e.in; });
                        CAPTURE(e.out, e.in);
                        CHECK(out_present);
                        CHECK(in_present);
                    }
                }
            }
        }
    }
}

TEST_CASE("the renderer's core decoding matrices are Tables 45 and 46 as printed",
          "[ac4dec][renderer]") {
    const std::vector<S> decoded = {S::kLeft,        S::kRight,        S::kCentre,
                                    S::kLfe,         S::kLeftSurround, S::kRightSurround,
                                    S::kTopSideLeft, S::kTopSideRight};
    const double p3 = db(3.0);
    for (const bool backs : {true, false}) {
        for (const int tops : {0, 1, 2, 3}) {
            for (const bool five_x_two : {true, false}) {
                CAPTURE(backs, tops, five_x_two);
                const detail::ImmersiveLayout layout{.backs = backs,
                                                     .tops = tops,
                                                     .lfe = true,
                                                     .decoding = ac4::DecodingMode::kCore};
                const detail::RenderPlan plan = detail::render_plan(
                    layout, five_x_two ? ac4::DownmixTarget::k5X2 : ac4::DownmixTarget::k5X);
                // Table 44: 5.X.2 and 5.X.0, as asked, whatever the source.
                std::vector<S> expected_speakers = {S::kLeft, S::kRight,        S::kCentre,
                                                    S::kLfe,  S::kLeftSurround, S::kRightSurround};
                if (five_x_two) {
                    expected_speakers.insert(expected_speakers.end(),
                                             {S::kTopSideLeft, S::kTopSideRight});
                }
                REQUIRE(plan.speakers == expected_speakers);
                const auto m = detail::render_matrix(layout, decoded, plan, test_gains());
                std::array<std::array<double, kIndices>, kIndices> table{};
                for (const int i : {0, 1, 2, 11}) {
                    table[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1.0;
                }
                const double side = backs ? kGainB * p3 : p3;
                table[3][3] = side;
                table[4][4] = side;
                if (five_x_two && tops != 0) {
                    // Table 45.
                    const double top = tops == 3 ? kGainT1 * p3 : p3;
                    table[12][12] = top;
                    table[13][13] = top;
                } else if (tops != 0) {
                    // Table 46.
                    table[0][12] = table[1][13] = kGainT2[0] * p3;
                    table[3][12] = table[4][13] = kGainT2[1] * p3;
                }
                for (std::size_t o = 0; o < plan.speakers.size(); ++o) {
                    for (std::size_t d = 0; d < decoded.size(); ++d) {
                        CAPTURE(ac4::describe(plan.speakers[o]), ac4::describe(decoded[d]));
                        const double expected =
                            table[static_cast<std::size_t>(index_of(plan.speakers[o]))]
                                 [static_cast<std::size_t>(index_of(decoded[d]))];
                        CHECK(std::abs(m[o][d] - expected) < 1e-12);
                    }
                }
            }
        }
    }
}

TEST_CASE("decode()'s layouts for an immersive element, in full and core decoding",
          "[ac4dec][renderer]") {
    using T = ac4::DownmixTarget;
    const detail::ImmersiveLayout full_514{.backs = false, .tops = 3, .lfe = true};
    CHECK(detail::render_plan(full_514, T::kAsCoded).speakers ==
          std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLfe, S::kLeftSurround,
                         S::kRightSurround, S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft,
                         S::kTopBackRight});
    CHECK(detail::render_plan(full_514, T::k7X4).speakers == decoded_714(true));
    CHECK(detail::render_plan(full_514, T::k7X2).speakers ==
          std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLfe, S::kLeftSurround,
                         S::kRightSurround, S::kLeftBack, S::kRightBack, S::kTopSideLeft,
                         S::kTopSideRight});
    const detail::ImmersiveLayout full_702{.backs = true, .tops = 2, .lfe = false};
    CHECK(detail::render_plan(full_702, T::kAsCoded).speakers ==
          std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround,
                         S::kLeftBack, S::kRightBack, S::kTopSideLeft, S::kTopSideRight});
    // The two-channel and mono targets go by 5.X.0.
    for (const T target : {T::kStereo, T::kLoRo, T::kLtRt, T::kMono}) {
        const detail::RenderPlan plan = detail::render_plan(full_514, target);
        CHECK(plan.stereo);
        CHECK(plan.output == detail::ChannelConfiguration{.width = 5, .tops = 0});
    }
    // Core decoding: 5.X.2 at most, and 5.X.0 where the source has no tops.
    detail::ImmersiveLayout core = full_514;
    core.decoding = ac4::DecodingMode::kCore;
    for (const T target : {T::kAsCoded, T::k7X4, T::k7X2, T::k5X4, T::k5X2}) {
        CHECK(detail::render_plan(core, target).output ==
              detail::ChannelConfiguration{.width = 5, .tops = 2});
    }
    for (const T target : {T::k7X0, T::k5X}) {
        CHECK(detail::render_plan(core, target).output ==
              detail::ChannelConfiguration{.width = 5, .tops = 0});
    }
    core.tops = 0;
    CHECK(detail::render_plan(core, T::kAsCoded).output ==
          detail::ChannelConfiguration{.width = 5, .tops = 0});
    // Table 127.
    CHECK(detail::out_ch_config({.width = 5, .tops = 0}) == 0);
    CHECK(detail::out_ch_config({.width = 5, .tops = 2}) == 1);
    CHECK(detail::out_ch_config({.width = 5, .tops = 4}) == 2);
    CHECK(detail::out_ch_config({.width = 7, .tops = 0}) == 3);
    CHECK(detail::out_ch_config({.width = 7, .tops = 2}) == 4);
    CHECK_FALSE(detail::out_ch_config({.width = 7, .tops = 4}).has_value());
}

TEST_CASE("the custom downmix gains take Table 130's defaults and clause 6.3.10.3.10's exception",
          "[ac4dec][renderer]") {
    const double m3 = db(-3.0);
    // Table 130, where no custom downmix data came.
    const detail::RenderGains defaults = detail::render_gains(nullptr, 0);
    CHECK(defaults.gain_b == m3);
    CHECK(defaults.gain_t1 == m3);
    CHECK(defaults.gain_t2 == std::array<double, 6>{0.0, m3, 0.0, 0.0, m3, 0.0});

    // 7.X.4 (bs_ch_config 1): out_ch_config 0 with tool_t4_to_f_s() and
    // tool_b4_to_b2(), out_ch_config 1 with tool_t4_to_t2() and tool_b4_to_b2().
    detail::CustomDmxData cdmx;
    cdmx.bs_ch_config = 1;
    cdmx.b_cdmx_data_present = true;
    cdmx.n_cdmx_configs = 2;
    cdmx.cdmx[0].out_ch_config = 0;
    cdmx.cdmx[0].b_top_front_to_front = true;
    cdmx.cdmx[0].gain_t2a_code = 4;  // -6 dB
    cdmx.cdmx[0].gain_t2b_code = 7;  // the syntax's assignment: -inf
    cdmx.cdmx[0].b_top_back_to_front = false;
    cdmx.cdmx[0].gain_t2e_code = 1;  // -1.5 dB
    cdmx.cdmx[0].gain_b_code = 5;    // -9 dB
    cdmx.cdmx[1].out_ch_config = 1;
    cdmx.cdmx[1].gain_t1_code = 3;  // -4.5 dB
    cdmx.cdmx[1].gain_b_code = 0;   // 0 dB
    const detail::RenderGains to_50 = detail::render_gains(&cdmx, 0);
    CHECK(std::abs(to_50.gain_b - db(-9.0)) < 1e-12);
    CHECK(to_50.gain_t1 == m3);
    CHECK(std::abs(to_50.gain_t2[0] - db(-6.0)) < 1e-12);
    CHECK(to_50.gain_t2[1] == 0.0);
    CHECK(to_50.gain_t2[2] == 0.0);
    CHECK(to_50.gain_t2[3] == 0.0);
    CHECK(std::abs(to_50.gain_t2[4] - db(-1.5)) < 1e-12);
    CHECK(to_50.gain_t2[5] == 0.0);
    const detail::RenderGains to_52 = detail::render_gains(&cdmx, 1);
    CHECK(to_52.gain_b == 1.0);
    CHECK(std::abs(to_52.gain_t1 - db(-4.5)) < 1e-12);
    // A configuration the data does not send takes the defaults: 5.X.4.
    CHECK(detail::render_gains(&cdmx, 2).gain_b == m3);
    // The exception: 7.X.4 to 7.X.2 takes out_ch_config 1's tool_t4_to_t2()
    // where out_ch_config 4 sends none...
    CHECK(std::abs(detail::render_gains(&cdmx, 4).gain_t1 - db(-4.5)) < 1e-12);
    // ...and its own where it does.
    cdmx.n_cdmx_configs = 3;
    cdmx.cdmx[2].out_ch_config = 4;
    cdmx.cdmx[2].gain_t1_code = 6;  // -12 dB
    CHECK(std::abs(detail::render_gains(&cdmx, 4).gain_t1 - db(-12.0)) < 1e-12);
    // It is 7.X.4's alone: from 7.X.2 (bs_ch_config 4), none.
    cdmx.bs_ch_config = 4;
    cdmx.n_cdmx_configs = 2;
    CHECK(detail::render_gains(&cdmx, 4).gain_t1 == m3);
    // An entry past n_cdmx_configs is not read.
    cdmx.n_cdmx_configs = 1;
    CHECK(detail::render_gains(&cdmx, 1).gain_t1 == m3);
}

TEST_CASE("each output takes the loudness correction clause 4.8.5.3 gives it",
          "[ac4dec][renderer]") {
    using L = detail::LoudCorrOutput;
    const detail::ImmersiveLayout full_714{.backs = true, .tops = 3, .lfe = true};
    CHECK(detail::loud_corr_output(full_714, {.width = 7, .tops = 4}) == L::kNone);
    CHECK(detail::loud_corr_output(full_714, {.width = 7, .tops = 2}) == L::k7X2);
    CHECK(detail::loud_corr_output(full_714, {.width = 7, .tops = 0}) == L::k7X);
    CHECK(detail::loud_corr_output(full_714, {.width = 5, .tops = 4}) == L::k5X4);
    CHECK(detail::loud_corr_output(full_714, {.width = 5, .tops = 2}) == L::k5X2);
    CHECK(detail::loud_corr_output(full_714, {.width = 5, .tops = 0}) == L::k5X);
    // Nothing downmixed, nothing corrected: a 5.X.2 source to 7.X.4 or 5.X.2.
    const detail::ImmersiveLayout full_512{.backs = false, .tops = 1, .lfe = true};
    CHECK(detail::loud_corr_output(full_512, {.width = 7, .tops = 4}) == L::kNone);
    CHECK(detail::loud_corr_output(full_512, {.width = 5, .tops = 2}) == L::kNone);
    CHECK(detail::loud_corr_output(full_512, {.width = 7, .tops = 0}) == L::k7X);
    // Core decoding's own corrections.
    detail::ImmersiveLayout core = full_714;
    core.decoding = ac4::DecodingMode::kCore;
    CHECK(detail::loud_corr_output(core, {.width = 5, .tops = 2}) == L::kCore5X2);
    CHECK(detail::loud_corr_output(core, {.width = 5, .tops = 0}) == L::kCore5X);
}

namespace {

// `rows` [output][input] times `channels` [input], one QMF value each.
std::vector<double> apply_rows(const std::vector<std::vector<double>>& rows,
                          const std::vector<double>& in) {
    std::vector<double> out(rows.size(), 0.0);
    for (std::size_t o = 0; o < rows.size(); ++o) {
        for (std::size_t c = 0; c < in.size(); ++c) {
            out[o] += rows[o][c] * in[c];
        }
    }
    return out;
}

// One QMF value per channel through the stage, `values` sent with it.
std::vector<double> through(detail::DownmixStage& stage, const detail::DownmixValues& values,
                            const std::vector<double>& in) {
    std::vector<std::vector<detail::QmfValue>> matrices(in.size());
    std::vector<std::vector<detail::QmfValue>*> pointers;
    for (std::size_t c = 0; c < in.size(); ++c) {
        matrices[c] = {detail::QmfValue{in[c], 0.0}};
        pointers.push_back(&matrices[c]);
    }
    std::vector<std::vector<detail::QmfValue>> out;
    if (stage.passes_through()) {
        std::vector<double> same;
        for (const auto& m : matrices) {
            same.push_back(m[0].real());
        }
        return same;
    }
    stage.process(values, pointers, out);
    std::vector<double> result;
    for (const auto& o : out) {
        result.push_back(o.at(0).real());
    }
    return result;
}

}  // namespace

TEST_CASE(
    "the downmix stage renders an immersive element with what the stream sent, until it sends more",
    "[ac4dec][renderer]") {
    const std::vector<S> decoded = decoded_714(true);
    // Each decoded channel a value of its own.
    std::vector<double> in(decoded.size());
    for (std::size_t c = 0; c < in.size(); ++c) {
        in[c] = 1.0 + static_cast<double>(c);
    }
    const detail::ImmersiveLayout layout{.backs = true, .tops = 3, .lfe = true};
    detail::DownmixStage stage;

    // As coded, 7.X.4 to itself: through.
    stage.configure(decoded, false, ac4::DownmixTarget::kAsCoded, true, layout);
    CHECK(stage.passes_through());
    CHECK(std::vector<S>(stage.speakers().begin(), stage.speakers().end()) == decoded);

    // To 5.X.0: Table 43 with Table 130's defaults, then what the stream sends.
    stage.configure(decoded, false, ac4::DownmixTarget::k5X, true, layout);
    REQUIRE_FALSE(stage.passes_through());
    const detail::RenderPlan plan = detail::render_plan(layout, ac4::DownmixTarget::k5X);
    const auto defaults =
        detail::render_matrix(layout, decoded, plan, detail::render_gains(nullptr, 0));
    CHECK(through(stage, {}, in) == apply_rows(defaults, in));

    detail::DownmixValues sent;
    sent.cdmx.emplace();
    sent.cdmx->bs_ch_config = 1;
    sent.cdmx->b_cdmx_data_present = true;
    sent.cdmx->n_cdmx_configs = 1;
    sent.cdmx->cdmx[0].out_ch_config = 0;
    sent.cdmx->cdmx[0].gain_t2a_code = 2;  // -3 dB
    sent.cdmx->cdmx[0].gain_t2b_code = 7;
    sent.cdmx->cdmx[0].gain_t2e_code = 4;  // -6 dB
    sent.cdmx->cdmx[0].gain_b_code = 1;    // -1.5 dB
    sent.loud_corr_5x = 21;                // (15 - 21) / 2 = -3 dB2
    sent.loud_corr_7x = 3;                 // another output's: no effect here
    const auto custom =
        detail::render_matrix(layout, decoded, plan, detail::render_gains(&*sent.cdmx, 0));
    std::vector<double> expected = apply_rows(custom, in);
    for (double& v : expected) {
        v *= std::exp2(-3.0 / 6.0);
    }
    const auto check_near = [](const std::vector<double>& got, const std::vector<double>& want) {
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CAPTURE(i);
            CHECK(std::abs(got[i] - want[i]) < 1e-12 * std::max(1.0, std::abs(want[i])));
        }
    };
    check_near(through(stage, sent, in), expected);
    // A frame that sends nothing keeps them.
    check_near(through(stage, {}, in), expected);
    // One that sends a correction of 31 reads 0 dB, and keeps the custom data.
    detail::DownmixValues zero;
    zero.loud_corr_5x = 31;
    check_near(through(stage, zero, in), apply_rows(custom, in));
    // A reset forgets both.
    stage.reset();
    check_near(through(stage, {}, in), apply_rows(defaults, in));
}

TEST_CASE("an immersive element's two channels and one follow the renderer's 5.X.0 by Table 218",
          "[ac4dec][renderer]") {
    const std::vector<S> decoded = decoded_714(true);
    std::vector<double> in(decoded.size());
    for (std::size_t c = 0; c < in.size(); ++c) {
        in[c] = 1.0 + 0.5 * static_cast<double>(c);
    }
    for (const bool core : {false, true}) {
        CAPTURE(core);
        const detail::ImmersiveLayout layout{
            .backs = true,
            .tops = 3,
            .lfe = true,
            .decoding = core ? ac4::DecodingMode::kCore : ac4::DecodingMode::kFull};
        const std::vector<S> channels =
            core ? std::vector<S>{S::kLeft,        S::kRight,        S::kCentre,
                                  S::kLfe,         S::kLeftSurround, S::kRightSurround,
                                  S::kTopSideLeft, S::kTopSideRight}
                 : decoded;
        const std::vector<double> x(in.begin(),
                                    in.begin() + static_cast<std::ptrdiff_t>(channels.size()));
        const detail::RenderPlan plan = detail::render_plan(layout, ac4::DownmixTarget::k5X);
        const auto rows = apply_rows(
            detail::render_matrix(layout, channels, plan, detail::render_gains(nullptr, 0)), x);
        // L R C LFE Ls Rs of the 5.X.0 render.
        REQUIRE(plan.speakers == std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLfe,
                                                S::kLeftSurround, S::kRightSurround});
        detail::DownmixValues values;
        values.coeff = detail::StereoDmxCoeff{};
        values.coeff->loro_centre_mixgain = 3;    // -1.5 dB
        values.coeff->loro_surround_mixgain = 5;  // -4.5 dB
        values.coeff->preferred_dmx_method = 0;
        values.coeff->lfe_mixgain = 11;   // 5.5 - 11 = -5.5 dB
        values.loro_loud_corr = 19;       // -2 dB2
        values.loud_corr_core_loro = 11;  // +2 dB2
        const double cmg = db(-1.5);
        const double smg = db(-4.5);
        const double lfe = db(-5.5);
        const double correction = std::exp2((core ? 2.0 : -2.0) / 6.0);
        for (const bool mix_lfe : {false, true}) {
            CAPTURE(mix_lfe);
            detail::DownmixStage stage;
            stage.configure(channels, false, ac4::DownmixTarget::kLoRo, mix_lfe, layout);
            const std::vector<double> got = through(stage, values, x);
            REQUIRE(got.size() == 2);
            const double l_lfe = mix_lfe ? lfe * rows[3] : 0.0;
            const double lo = correction * (rows[0] + cmg * rows[2] + smg * rows[4] + l_lfe);
            const double ro = correction * (rows[1] + cmg * rows[2] + smg * rows[5] + l_lfe);
            CHECK(std::abs(got[0] - lo) < 1e-12 * std::abs(lo));
            CHECK(std::abs(got[1] - ro) < 1e-12 * std::abs(ro));
            detail::DownmixStage mono;
            mono.configure(channels, false, ac4::DownmixTarget::kMono, mix_lfe, layout);
            const std::vector<double> c = through(mono, values, x);
            REQUIRE(c.size() == 1);
            CHECK(std::abs(c[0] - (lo + ro)) < 1e-12 * std::abs(lo + ro));
        }
    }
}

TEST_CASE("core decoding's renderer never passes the core through: Table 45 takes the custom gains",
          "[ac4dec][renderer]") {
    const std::vector<S> core = {S::kLeft,        S::kRight,        S::kCentre,
                                 S::kLfe,         S::kLeftSurround, S::kRightSurround,
                                 S::kTopSideLeft, S::kTopSideRight};
    const detail::ImmersiveLayout layout{
        .backs = true, .tops = 3, .lfe = true, .decoding = ac4::DecodingMode::kCore};
    detail::DownmixStage stage;
    stage.configure(core, false, ac4::DownmixTarget::kAsCoded, true, layout);
    CHECK_FALSE(stage.passes_through());
    CHECK(std::vector<S>(stage.speakers().begin(), stage.speakers().end()) == core);
    const std::vector<double> in = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    // Table 130's gain_b and gain_t1 and the +3 dB: the core as it came.
    const std::vector<double> same = through(stage, {}, in);
    for (std::size_t c = 0; c < in.size(); ++c) {
        CHECK(std::abs(same[c] - in[c]) < 1e-12 * in[c]);
    }
    // gain_t1 -9 dB for 5.X.2 (out_ch_config 1), and the core's correction.
    detail::DownmixValues values;
    values.cdmx.emplace();
    values.cdmx->bs_ch_config = 1;
    values.cdmx->b_cdmx_data_present = true;
    values.cdmx->n_cdmx_configs = 1;
    values.cdmx->cdmx[0].out_ch_config = 1;
    values.cdmx->cdmx[0].gain_t1_code = 5;  // -9 dB
    values.cdmx->cdmx[0].gain_b_code = 0;   // 0 dB
    values.loud_corr_core_5x2 = 17;         // -1 dB2
    values.loud_corr_5x2 = 3;               // full decoding's: no effect
    const std::vector<double> got = through(stage, values, in);
    const double k = std::exp2(-1.0 / 6.0);
    const std::vector<double> gains = {1.0, 1.0, 1.0, 1.0, db(3.0), db(3.0), db(-6.0), db(-6.0)};
    for (std::size_t c = 0; c < in.size(); ++c) {
        CAPTURE(c);
        CHECK(std::abs(got[c] - k * gains[c] * in[c]) < 1e-12 * in[c]);
    }
}

TEST_CASE(
    "downmix_values takes the immersive corrections and custom downmix data a presentation sends",
    "[ac4dec][renderer]") {
    detail::PresentationSubstream p;
    p.loud_corr.loud_corr_5_X = 1;
    p.loud_corr.loud_corr_5_X_2 = 2;
    p.loud_corr.loud_corr_5_X_4 = 3;
    p.loud_corr.loud_corr_7_X = 4;
    p.loud_corr.loud_corr_7_X_2 = 5;
    p.loud_corr.loud_corr_7_X_4 = 6;
    p.loud_corr.loud_corr_core_5_X_2 = 7;
    p.loud_corr.loud_corr_core_5_X = 8;
    p.loud_corr.loud_corr_core_loro = 9;
    p.loud_corr.loud_corr_core_ltrt = 10;
    const detail::Metadata metadata{};
    detail::DownmixValues v = detail::downmix_values(&p, metadata);
    CHECK(v.loud_corr_5x == 1);
    CHECK(v.loud_corr_5x2 == 2);
    CHECK(v.loud_corr_5x4 == 3);
    CHECK(v.loud_corr_7x == 4);
    CHECK(v.loud_corr_7x2 == 5);
    CHECK(v.loud_corr_7x4 == 6);
    CHECK(v.loud_corr_core_5x2 == 7);
    CHECK(v.loud_corr_core_5x == 8);
    CHECK(v.loud_corr_core_loro == 9);
    CHECK(v.loud_corr_core_ltrt == 10);
    // Custom downmix data where b_cdmx_data_present says it came.
    CHECK_FALSE(v.cdmx.has_value());
    p.custom_dmx_data.b_cdmx_data_present = true;
    p.custom_dmx_data.bs_ch_config = 2;
    v = detail::downmix_values(&p, metadata);
    REQUIRE(v.cdmx.has_value());
    CHECK(v.cdmx->bs_ch_config == 2);
}
