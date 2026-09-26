// Multichannel processing (src/ac4dec/src/pcm/multichannel.*) against the
// entries ETSI TS 103 190-1 V1.4.1 prints for it, and the routing of a
// channel element's tracks to channels (pcm/routing.*) against Tables 180,
// 182, 183, 212 and 213, and the immersive element's against ETSI TS 103
// 190-2 V1.3.1 Tables 19, 20 and 8.
//
// Tables 178 and 179 are held as printed, entry by entry, in
// ac4dec_printed_matrices.hpp, a transcription separate from the
// implementation's: Table 178 is written there entry by entry too, and Table
// 179 as the cascade its entries share.

#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_printed_matrices.hpp"
#include "pcm/multichannel.hpp"
#include "pcm/routing.hpp"

namespace {

using ac4::Speaker;
using ac4::detail::Abcd;
using ac4::detail::ChannelElement;
using ac4::detail::ElementKind;
using ac4::detail::ElementRoute;
using ac4::detail::SubstreamContext;
using ac4dec_test::entry;
using ac4dec_test::kFourChannel;
using ac4dec_test::kTable178;
using ac4dec_test::kTable179;
using ac4dec_test::rows_of;

std::vector<Abcd> random_parameters(std::mt19937& rng, std::size_t count) {
    std::uniform_real_distribution<double> value(-2.0, 2.0);
    std::vector<Abcd> p(count);
    for (Abcd& set : p) {
        for (double& x : set) {
            x = value(rng);
        }
    }
    return p;
}

template <std::size_t N>
void check_printed(const ac4::detail::Matrix<N>& m, std::string_view printed, std::span<const Abcd> p) {
    const auto rows = rows_of(printed);
    REQUIRE(rows.size() == N);
    for (std::size_t o = 0; o < N; ++o) {
        REQUIRE(rows[o].size() == N);
        for (std::size_t i = 0; i < N; ++i) {
            CAPTURE(o, i, rows[o][i]);
            const double expected = entry(rows[o][i], p);
            CHECK(std::abs(m[o][i] - expected) <= 1e-12 * (1.0 + std::abs(expected)));
        }
    }
}

// A channel element as parse_audio_data_chan() leaves it, with `tracks`
// tracks each of its own sf_info() and nothing else: enough for the route.
ChannelElement element_of(ElementKind kind, int tracks, bool lfe) {
    ChannelElement element;
    element.kind = kind;
    for (int t = 0; t < tracks; ++t) {
        ac4::detail::Track track;
        track.info = t;
        track.lfe = lfe && t == 0;
        element.tracks.push_back(track);
        element.infos.emplace_back();
    }
    return element;
}

// The channel each track's output reaches, in track order.
std::vector<Speaker> destinations(const ElementRoute& route) {
    std::vector<Speaker> out;
    for (const auto& part : route.data) {
        for (int k = 0; k < part.count; ++k) {
            out.push_back(part.outputs[static_cast<std::size_t>(k)]);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Table 178's matrices equal the table's printed entries", "[ac4dec][multichannel]") {
    std::mt19937 rng(178);
    for (int trial = 0; trial < 20; ++trial) {
        const std::vector<Abcd> p = random_parameters(rng, 2);
        for (int matsel = 0; matsel < 12; ++matsel) {
            CAPTURE(trial, matsel);
            const auto m = ac4::detail::three_channel_matrix(matsel, p[0], p[1]);
            REQUIRE(m.has_value());
            check_printed<3>(*m, kTable178[static_cast<std::size_t>(matsel)], p);
        }
    }
}

TEST_CASE("Table 179's matrices equal the table's printed entries", "[ac4dec][multichannel]") {
    std::mt19937 rng(179);
    for (int trial = 0; trial < 20; ++trial) {
        const std::vector<Abcd> p = random_parameters(rng, 5);
        for (int matsel = 0; matsel < 12; ++matsel) {
            CAPTURE(trial, matsel);
            const auto m = ac4::detail::five_channel_matrix(matsel, std::span<const Abcd, 5>(p.data(), 5));
            REQUIRE(m.has_value());
            check_printed<5>(*m, kTable179[static_cast<std::size_t>(matsel)], p);
        }
    }
}

TEST_CASE("clause 5.3.3.4's matrix equals its printed entries", "[ac4dec][multichannel]") {
    std::mt19937 rng(334);
    for (int trial = 0; trial < 20; ++trial) {
        const std::vector<Abcd> p = random_parameters(rng, 4);
        check_printed<4>(ac4::detail::four_channel_matrix(std::span<const Abcd, 4>(p.data(), 4)), kFourChannel, p);
    }
}

TEST_CASE("every chel_matsel's matrix is the identity when its parameters are", "[ac4dec][multichannel]") {
    // sap_mode 0 sets a = d = 1 and b = c = 0 (Pseudocode 59): the tracks are
    // the channels, whatever chel_matsel says.
    const Abcd one = {1.0, 0.0, 0.0, 1.0};
    const std::array<Abcd, 5> ones = {one, one, one, one, one};
    for (int matsel = 0; matsel < 12; ++matsel) {
        CAPTURE(matsel);
        const auto three = ac4::detail::three_channel_matrix(matsel, one, one);
        const auto five = ac4::detail::five_channel_matrix(matsel, ones);
        REQUIRE(three.has_value());
        REQUIRE(five.has_value());
        for (std::size_t o = 0; o < 5; ++o) {
            for (std::size_t i = 0; i < 5; ++i) {
                const double expected = o == i ? 1.0 : 0.0;
                if (o < 3 && i < 3) {
                    CHECK((*three)[o][i] == expected);
                }
                CHECK((*five)[o][i] == expected);
            }
        }
    }
}

TEST_CASE("three_channel_data() takes its two parameter sets and reads no third", "[ac4dec][multichannel]") {
    // One long group of two bands, M/S in both sets, and the sets in a vector
    // of exactly two: a read of a third set leaves it, which a sanitised or
    // bounds-checked build stops at.
    ac4::detail::SfInfo info;
    info.psy.num_window_groups = 1;
    ac4::detail::SfData layout;
    layout.max_sfb[0] = 2;
    layout.sect_sfb_offset[0][1] = 4;
    layout.sect_sfb_offset[0][2] = 8;
    std::vector<ac4::detail::StereoParameters> sets(2);
    for (ac4::detail::StereoParameters& set : sets) {
        set.abcd[0][0] = {1.0, 1.0, 1.0, -1.0};
        set.abcd[0][1] = {1.0, 1.0, 1.0, -1.0};
    }
    std::vector<std::vector<double>> lines(3, std::vector<double>(8));
    for (std::size_t t = 0; t < 3; ++t) {
        for (std::size_t k = 0; k < 8; ++k) {
            lines[t][k] = static_cast<double>(10 * t + k);
        }
    }
    const std::vector<std::vector<double>> tracks_in = lines;
    const std::array<std::vector<double>*, 3> tracks = {&lines[0], &lines[1], &lines[2]};
    REQUIRE(static_cast<bool>(ac4::detail::apply_channel_data(info, layout, 0, sets, tracks)));
    // chel_matsel 0 with M/S in both sets: O0 = I0 + I1 + I2, O1 = I0 - I1,
    // O2 = I0 + I1 - I2.
    for (std::size_t k = 0; k < 8; ++k) {
        CAPTURE(k);
        const double i0 = tracks_in[0][k];
        const double i1 = tracks_in[1][k];
        const double i2 = tracks_in[2][k];
        CHECK(lines[0][k] == i0 + i1 + i2);
        CHECK(lines[1][k] == i0 - i1);
        CHECK(lines[2][k] == i0 + i1 - i2);
    }
}

TEST_CASE("chel_matsel 12 to 15, which the tables leave out, make no matrix", "[ac4dec][multichannel]") {
    const Abcd one = {1.0, 0.0, 0.0, 1.0};
    const std::array<Abcd, 5> ones = {one, one, one, one, one};
    for (int matsel = 12; matsel < 16; ++matsel) {
        CHECK_FALSE(ac4::detail::three_channel_matrix(matsel, one, one).has_value());
        CHECK_FALSE(ac4::detail::five_channel_matrix(matsel, ones).has_value());
    }
}

TEST_CASE("the channel modes' speakers, in the order decode() writes them", "[ac4dec][multichannel]") {
    using ac4::detail::speakers_of;
    namespace mode = ac4::detail::ch_mode;
    const auto list = [](int ch_mode) {
        const auto s = speakers_of(ch_mode);
        return std::vector<Speaker>(s.begin(), s.end());
    };
    CHECK(list(mode::kMono) == std::vector{Speaker::kCentre});
    CHECK(list(mode::kStereo) == std::vector{Speaker::kLeft, Speaker::kRight});
    CHECK(list(mode::k3_0) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre});
    CHECK(list(mode::k5_1) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLfe,
                                          Speaker::kLeftSurround, Speaker::kRightSurround});
    // Table 88: 3/4/0 adds Lb and Rb, 5/2/0 Lw and Rw, 3/2/2 Tfl and Tfr.
    CHECK(list(mode::k7_1_340).back() == Speaker::kRightBack);
    CHECK(list(mode::k7_0_520).back() == Speaker::kRightWide);
    CHECK(list(mode::k7_1_322) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLfe,
                                              Speaker::kLeftSurround, Speaker::kRightSurround,
                                              Speaker::kTopFrontLeft, Speaker::kTopFrontRight});
    // Part 2: 7.X.4's twelve in full decoding, its 5.X.2 core in core
    // decoding; the Part 1 modes alike in both.
    CHECK(list(mode::k7_1_4) ==
          std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLfe,
                      Speaker::kLeftSurround, Speaker::kRightSurround, Speaker::kLeftBack,
                      Speaker::kRightBack, Speaker::kTopFrontLeft, Speaker::kTopFrontRight,
                      Speaker::kTopBackLeft, Speaker::kTopBackRight});
    CHECK(list(mode::k7_0_4).size() == 11);
    const auto core = [](int ch_mode) {
        const auto s = speakers_of(ch_mode, ac4::DecodingMode::kCore);
        return std::vector<Speaker>(s.begin(), s.end());
    };
    CHECK(core(mode::k7_0_4) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                            Speaker::kLeftSurround, Speaker::kRightSurround,
                                            Speaker::kTopSideLeft, Speaker::kTopSideRight});
    CHECK(core(mode::k7_1_4).size() == 8);
    CHECK(core(mode::k7_1_322) == list(mode::k7_1_322));
    CHECK(list(mode::k9_1_4).empty());
    CHECK(list(mode::k22_2).empty());
}

TEST_CASE("Table 180 routes the 5.X element's tracks", "[ac4dec][multichannel]") {
    SubstreamContext ctx;
    ctx.ch_mode = ac4::detail::ch_mode::k5_1;
    ElementRoute route;
    SECTION("coding_config 0: two pairs and C, the pairs by 2ch_mode") {
        ChannelElement element = element_of(ElementKind::k5X, 6, true);
        element.coding_config = 0;
        element.b_enable_mdct_stereo_proc = {true, false};
        element.chparams.resize(1);
        element.two_ch_mode = false;
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLfe, Speaker::kLeft, Speaker::kRight,
                                                 Speaker::kLeftSurround, Speaker::kRightSurround,
                                                 Speaker::kCentre});
        CHECK(route.data[1].processed);
        CHECK_FALSE(route.data[2].processed);
        element.two_ch_mode = true;
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLfe, Speaker::kLeft, Speaker::kLeftSurround,
                                                 Speaker::kRight, Speaker::kRightSurround, Speaker::kCentre});
    }
    SECTION("coding_config 1: three tracks, then the surround pair") {
        ChannelElement element = element_of(ElementKind::k5X, 6, true);
        element.coding_config = 1;
        element.chel_matsel = {7};
        element.b_enable_mdct_stereo_proc = {true};
        element.chparams.resize(3);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLfe, Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                                 Speaker::kLeftSurround, Speaker::kRightSurround});
        CHECK(route.data[1].chel_matsel == 7);
        CHECK(route.data[2].first_chparam == 2);
    }
    SECTION("coding_config 2: four tracks, then C") {
        ChannelElement element = element_of(ElementKind::k5X, 6, true);
        element.coding_config = 2;
        element.chparams.resize(4);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLfe, Speaker::kLeft, Speaker::kRight,
                                                 Speaker::kLeftSurround, Speaker::kRightSurround,
                                                 Speaker::kCentre});
    }
    SECTION("coding_config 3: five tracks") {
        ctx.ch_mode = ac4::detail::ch_mode::k5_0;
        ChannelElement element = element_of(ElementKind::k5X, 5, false);
        element.coding_config = 3;
        element.chel_matsel = {11};
        element.chparams.resize(5);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                                 Speaker::kLeftSurround, Speaker::kRightSurround});
    }
    SECTION("an element whose parts do not add up is refused") {
        ChannelElement element = element_of(ElementKind::k5X, 6, true);
        element.coding_config = 2;
        element.chparams.resize(3);  // four_channel_data() holds four
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
        element.chparams.resize(4);
        ctx.ch_mode = ac4::detail::ch_mode::k5_0;  // an LFE track where the mode has none
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
    }
}

TEST_CASE("Table 182 routes the 7.X element's tracks, and Table 183 pairs its last two", "[ac4dec][multichannel]") {
    SubstreamContext ctx;
    ElementRoute route;
    SECTION("3/4/0.1, coding_config 0, 2ch_mode 1") {
        ctx.ch_mode = ac4::detail::ch_mode::k7_1_340;
        ChannelElement element = element_of(ElementKind::k7X, 8, true);
        element.coding_config = 0;
        element.two_ch_mode = true;
        element.b_enable_mdct_stereo_proc = {false, false, false};
        element.b_use_sap_add_ch = true;
        element.chparams.resize(2);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        // Tracks 0 to 3 are A, D, B and E, 4 and 5 F and G, 6 C.
        CHECK(destinations(route) == std::vector{Speaker::kLfe, Speaker::kLeft, Speaker::kLeftSurround,
                                                 Speaker::kRight, Speaker::kRightSurround, Speaker::kLeftBack,
                                                 Speaker::kRightBack, Speaker::kCentre});
        REQUIRE(route.steps.size() == 2);
        CHECK(route.steps[0].first == Speaker::kLeftSurround);
        CHECK(route.steps[0].second == Speaker::kLeftBack);
        CHECK(route.steps[0].chparam == 0);
        CHECK(route.steps[1].first == Speaker::kRightSurround);
        CHECK(route.steps[1].second == Speaker::kRightBack);
    }
    SECTION("5/2/0, coding_config 3: five tracks, then the wide pair") {
        ctx.ch_mode = ac4::detail::ch_mode::k7_0_520;
        ChannelElement element = element_of(ElementKind::k7X, 7, false);
        element.coding_config = 3;
        element.chel_matsel = {0};
        element.b_enable_mdct_stereo_proc = {true};
        element.b_use_sap_add_ch = true;
        element.chparams.resize(5 + 2 + 1);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                                 Speaker::kLeftSurround, Speaker::kRightSurround,
                                                 Speaker::kLeftWide, Speaker::kRightWide});
        REQUIRE(route.steps.size() == 2);
        CHECK(route.steps[0].first == Speaker::kLeft);
        CHECK(route.steps[0].second == Speaker::kLeftWide);
        CHECK(route.steps[0].chparam == 5);
        CHECK(route.data.back().first_chparam == 7);
    }
    SECTION("3/2/2, coding_config 2, b_use_sap_add_ch unset") {
        ctx.ch_mode = ac4::detail::ch_mode::k7_0_322;
        ChannelElement element = element_of(ElementKind::k7X, 7, false);
        element.coding_config = 2;
        element.b_enable_mdct_stereo_proc = {false};
        element.b_use_sap_add_ch = false;
        element.chparams.resize(4);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kLeftSurround,
                                                 Speaker::kRightSurround, Speaker::kTopFrontLeft,
                                                 Speaker::kTopFrontRight, Speaker::kCentre});
        CHECK(route.steps.empty());
    }
}

TEST_CASE("Part 2 Table 19 routes the immersive element's tracks, with step 4 and Table 20",
          "[ac4dec][multichannel]") {
    namespace immersive = ac4::detail::immersive_mode;
    using S = Speaker;
    SubstreamContext ctx;
    ctx.ch_mode = ac4::detail::ch_mode::k7_1_4;
    ElementRoute route;
    // An element of `tracks` tracks and `pairs` two_channel_data() without
    // stereo processing, in `mode`.
    const auto immersive_element = [](int mode, int tracks, int pairs, bool lfe) {
        ChannelElement element = element_of(ElementKind::kImmersive, tracks, lfe);
        element.codec_mode = mode;
        element.b_enable_mdct_stereo_proc.assign(static_cast<std::size_t>(pairs), false);
        return element;
    };
    SECTION("SCPL, core_5ch_grouping 0 and 2ch_mode 0, with step 4's parameters") {
        // LFE, [A,B], [D,E], C, [F,G], [H,I], [J,K]; two chparam_info() for
        // step 4 and four for Table 20.
        ChannelElement element = immersive_element(immersive::kScpl, 12, 5, true);
        element.core_5ch_grouping = 0;
        element.two_ch_mode = false;
        element.b_use_sap_add_ch = true;
        element.chparams.resize(6);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        CHECK(destinations(route) == std::vector{S::kLfe, S::kLeft, S::kRight, S::kLeftSurround,
                                                 S::kRightSurround, S::kCentre, S::kTopFrontLeft,
                                                 S::kTopFrontRight, S::kLeftBack, S::kRightBack,
                                                 S::kTopBackLeft, S::kTopBackRight});
        CHECK(route.silent.empty());
        REQUIRE(route.steps.size() == 6);
        // Step 4: (D, F) and (E, G), framed as D and E.
        CHECK((route.steps[0].first == S::kLeftSurround &&
               route.steps[0].second == S::kTopFrontLeft));
        CHECK((route.steps[1].first == S::kRightSurround &&
               route.steps[1].second == S::kTopFrontRight));
        CHECK((route.steps[0].chparam == 0 && route.steps[1].chparam == 1));
        CHECK_FALSE(route.steps[0].prediction);
        CHECK(route.steps[1].framing == S::kRightSurround);
        // Table 20: H, I, J and K from D, E, F and G, after the tracks.
        const std::array<std::array<S, 2>, 4> predicted = {{{S::kLeftSurround, S::kLeftBack},
                                                            {S::kRightSurround, S::kRightBack},
                                                            {S::kTopFrontLeft, S::kTopBackLeft},
                                                            {S::kTopFrontRight, S::kTopBackRight}}};
        for (std::size_t j = 0; j < 4; ++j) {
            CAPTURE(j);
            const auto& step = route.steps[2 + j];
            CHECK(step.prediction);
            CHECK(step.first == predicted[j][0]);
            CHECK(step.second == predicted[j][1]);
            CHECK(step.framing == predicted[j][0]);
            CHECK(step.chparam == 2 + static_cast<int>(j));
        }

        // Core decoding: F and G are the core's Tsl and Tsr, H to K are read
        // and not decoded, and Table 20 is left out.
        REQUIRE(ac4::detail::route_element(ctx, element, route, ac4::DecodingMode::kCore));
        REQUIRE(route.data.size() == 7);
        CHECK(route.data[4].outputs[0] == S::kTopSideLeft);
        CHECK(route.data[4].outputs[1] == S::kTopSideRight);
        CHECK_FALSE(route.data[4].discarded);
        CHECK(route.data[5].discarded);
        CHECK(route.data[6].discarded);
        REQUIRE(route.steps.size() == 2);
        CHECK(route.steps[0].second == S::kTopSideLeft);
        CHECK(route.steps[1].second == S::kTopSideRight);
    }
    SECTION("ASPX_SCPL, core_5ch_grouping 0 and 2ch_mode 1") {
        ChannelElement element = immersive_element(immersive::kAspxScpl, 12, 5, true);
        element.core_5ch_grouping = 0;
        element.two_ch_mode = true;
        element.b_use_sap_add_ch = false;
        element.b_enable_mdct_stereo_proc[2] = true;  // [F,G]'s chparam_info() after the core's
        element.chparams.resize(1 + 4);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        // [A,D] and [B,E].
        CHECK(destinations(route) == std::vector{S::kLfe, S::kLeft, S::kLeftSurround, S::kRight,
                                                 S::kRightSurround, S::kCentre, S::kTopFrontLeft,
                                                 S::kTopFrontRight, S::kLeftBack, S::kRightBack,
                                                 S::kTopBackLeft, S::kTopBackRight});
        CHECK((route.data[4].processed && route.data[4].first_chparam == 0));
        REQUIRE(route.steps.size() == 4);
        CHECK((route.steps[0].prediction && route.steps[0].chparam == 1));
    }
    SECTION("core_5ch_grouping 1, 2 and 3") {
        ctx.ch_mode = ac4::detail::ch_mode::k7_0_4;
        ChannelElement three = immersive_element(immersive::kAspxAcpl1, 11, 4, false);
        three.core_5ch_grouping = 1;
        three.chel_matsel = {3};
        three.b_use_sap_add_ch = false;
        three.chparams.resize(2 + 4);
        REQUIRE(ac4::detail::route_element(ctx, three, route));
        CHECK(destinations(route) == std::vector{S::kLeft, S::kRight, S::kCentre, S::kLeftSurround,
                                                 S::kRightSurround, S::kTopFrontLeft,
                                                 S::kTopFrontRight, S::kLeftBack, S::kRightBack,
                                                 S::kTopBackLeft, S::kTopBackRight});
        CHECK(route.data[0].chel_matsel == 3);
        CHECK(route.steps.front().chparam == 2);

        ChannelElement four = immersive_element(immersive::kScpl, 11, 3, false);
        four.core_5ch_grouping = 2;
        four.b_use_sap_add_ch = false;
        four.chparams.resize(4 + 4);
        REQUIRE(ac4::detail::route_element(ctx, four, route));
        CHECK(destinations(route) == std::vector{S::kLeft, S::kRight, S::kLeftSurround,
                                                 S::kRightSurround, S::kCentre, S::kTopFrontLeft,
                                                 S::kTopFrontRight, S::kLeftBack, S::kRightBack,
                                                 S::kTopBackLeft, S::kTopBackRight});

        ChannelElement five = immersive_element(immersive::kScpl, 11, 3, false);
        five.core_5ch_grouping = 3;
        five.chel_matsel = {0};
        five.b_use_sap_add_ch = true;
        five.chparams.resize(5 + 2 + 4);
        REQUIRE(ac4::detail::route_element(ctx, five, route));
        CHECK(destinations(route) == std::vector{S::kLeft, S::kRight, S::kCentre, S::kLeftSurround,
                                                 S::kRightSurround, S::kTopFrontLeft,
                                                 S::kTopFrontRight, S::kLeftBack, S::kRightBack,
                                                 S::kTopBackLeft, S::kTopBackRight});
        CHECK(route.steps[0].chparam == 5);
        CHECK(route.steps[2].chparam == 7);
    }
    SECTION("ASPX_ACPL_2 and ASPX_AJCC leave silent what A-CPL and A-JCC make") {
        ChannelElement acpl = immersive_element(immersive::kAspxAcpl2, 8, 3, true);
        acpl.core_5ch_grouping = 0;
        acpl.two_ch_mode = false;
        acpl.b_use_sap_add_ch = true;
        acpl.chparams.resize(2);
        REQUIRE(ac4::detail::route_element(ctx, acpl, route));
        CHECK(route.silent ==
              std::vector{S::kLeftBack, S::kRightBack, S::kTopBackLeft, S::kTopBackRight});
        // Step 4 in ASPX_ACPL_2 too (ERRATA.md, "ASPX_ACPL_2 and step 4").
        CHECK(route.steps.size() == 2);
        REQUIRE(ac4::detail::route_element(ctx, acpl, route, ac4::DecodingMode::kCore));
        CHECK(route.silent.empty());

        ChannelElement ajcc = immersive_element(immersive::kAspxAjcc, 6, 2, true);
        ajcc.core_5ch_grouping = 0;
        ajcc.two_ch_mode = false;
        REQUIRE(ac4::detail::route_element(ctx, ajcc, route));
        CHECK(destinations(route) == std::vector{S::kLfe, S::kLeft, S::kRight, S::kLeftSurround,
                                                 S::kRightSurround, S::kCentre});
        CHECK(route.silent == std::vector{S::kLeftBack, S::kRightBack, S::kTopFrontLeft,
                                          S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight});
        REQUIRE(ac4::detail::route_element(ctx, ajcc, route, ac4::DecodingMode::kCore));
        CHECK(route.silent == std::vector{S::kTopSideLeft, S::kTopSideRight});
        CHECK(route.steps.empty());
    }
    SECTION("an element whose parts do not add up is refused") {
        ChannelElement element = immersive_element(immersive::kScpl, 12, 5, true);
        element.core_5ch_grouping = 0;
        element.two_ch_mode = false;
        element.b_use_sap_add_ch = false;
        element.chparams.resize(4);
        REQUIRE(ac4::detail::route_element(ctx, element, route));
        element.chparams.resize(3);  // Table 20's four
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
        element.chparams.resize(4);
        element.two_ch_mode.reset();  // grouping 0 reads 2ch_mode
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
        element.two_ch_mode = false;
        element.b_use_sap_add_ch.reset();  // 7CH_STATIC reads b_use_sap_add_ch
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
        element.b_use_sap_add_ch = false;
        ctx.ch_mode = ac4::detail::ch_mode::k7_0_4;  // an LFE track where the mode has none
        CHECK_FALSE(ac4::detail::route_element(ctx, element, route));
    }
}

TEST_CASE("Tables 212 and 213 name the channels companding and A-SPX process", "[ac4dec][multichannel]") {
    namespace mode = ac4::detail::ch_mode;
    using ac4::detail::aspx_units;
    using ac4::detail::companded_speakers;
    namespace codec = ac4::detail::codec_mode;
    CHECK(companded_speakers(mode::k5_1, codec::kAspx) ==
          std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLeftSurround,
                      Speaker::kRightSurround});
    CHECK(companded_speakers(mode::k3_0, codec::kAspx) ==
          std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre});
    CHECK(companded_speakers(mode::k7_1_340, codec::kAspx).empty());  // no companding_control() in 7.X ASPX
    CHECK(companded_speakers(mode::k5_1, codec::kSimple).empty());
    CHECK(aspx_units(mode::k5_1, codec::kSimple).empty());

    const auto five = aspx_units(mode::k5_0, codec::kAspx);
    REQUIRE(five.size() == 3);
    CHECK((five[0].pair && five[0].index == 0 && five[0].speakers[1] == Speaker::kRight));
    CHECK((five[1].pair && five[1].index == 1 && five[1].speakers[0] == Speaker::kLeftSurround));
    CHECK((!five[2].pair && five[2].index == 0 && five[2].speakers[0] == Speaker::kCentre));

    // 7.X: (L, R), then (Ls, Rs) or 5/2/0's (Lw, Rw), C, then the last pair.
    const auto back = aspx_units(mode::k7_1_340, codec::kAspx);
    const auto wide = aspx_units(mode::k7_0_520, codec::kAspx);
    const auto top = aspx_units(mode::k7_0_322, codec::kAspx);
    REQUIRE(back.size() == 4);
    REQUIRE(wide.size() == 4);
    REQUIRE(top.size() == 4);
    CHECK(back[1].speakers[0] == Speaker::kLeftSurround);
    CHECK(back[3].speakers[0] == Speaker::kLeftBack);
    CHECK(wide[1].speakers[0] == Speaker::kLeftWide);
    CHECK(wide[3].speakers[0] == Speaker::kLeftSurround);
    CHECK(top[3].speakers[1] == Speaker::kTopFrontRight);
    CHECK(back[3].index == 2);

    // The A-CPL modes (codec modes 2 to 4): a pair's L alone; the 5.X
    // element's L, R and C, or L and R in ASPX_ACPL_3; the 7.X element's five
    // channels of the 5.X core, companded in its A-CPL modes only.
    CHECK(companded_speakers(mode::kStereo, codec::kAspxAcpl2) == std::vector{Speaker::kLeft});
    const auto pair = aspx_units(mode::kStereo, codec::kAspxAcpl1);
    REQUIRE(pair.size() == 1);
    CHECK((!pair[0].pair && pair[0].speakers[0] == Speaker::kLeft));
    CHECK(companded_speakers(mode::k5_1, codec::kAspxAcpl2) ==
          std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre});
    const auto five_acpl = aspx_units(mode::k5_1, codec::kAspxAcpl1);
    REQUIRE(five_acpl.size() == 2);
    CHECK((five_acpl[0].pair && !five_acpl[1].pair && five_acpl[1].speakers[0] == Speaker::kCentre));
    CHECK(companded_speakers(mode::k5_0, codec::kAspxAcpl3) == std::vector{Speaker::kLeft, Speaker::kRight});
    CHECK(aspx_units(mode::k5_0, codec::kAspxAcpl3).size() == 1);
    CHECK(companded_speakers(mode::k7_0_520, codec::kAspxAcpl2) ==
          std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLeftSurround,
                      Speaker::kRightSurround});
    const auto seven_acpl = aspx_units(mode::k7_0_520, codec::kAspxAcpl1);
    REQUIRE(seven_acpl.size() == 3);
    CHECK(seven_acpl[1].speakers[0] == Speaker::kLeftSurround);
    CHECK(seven_acpl[2].speakers[0] == Speaker::kCentre);
}

TEST_CASE("Part 2 Table 8 names the channels A-SPX processes in the immersive element",
          "[ac4dec][multichannel]") {
    namespace mode = ac4::detail::ch_mode;
    namespace immersive = ac4::detail::immersive_mode;
    using ac4::detail::aspx_units;
    using ac4::detail::companded_speakers;
    using S = Speaker;
    constexpr auto kCore = ac4::DecodingMode::kCore;
    CHECK(aspx_units(mode::k7_1_4, immersive::kScpl).empty());
    CHECK(companded_speakers(mode::k7_1_4, immersive::kAspxScpl).empty());
    // Only ASPX_AJCC sends companding_control(), for L, R, C, Ls and Rs.
    CHECK(companded_speakers(mode::k7_0_4, immersive::kAspxAjcc) ==
          std::vector{S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround});

    // ASPX_SCPL: (Ls, Lb), (Rs, Rb), C, (L, R), (Tfl, Tbl), (Tfr, Tbr); in
    // core decoding [Ls], [Rs], C, (L, R), [Tsl] and [Tsr].
    const auto full = aspx_units(mode::k7_1_4, immersive::kAspxScpl);
    REQUIRE(full.size() == 6);
    const std::array<std::array<S, 2>, 6> kFull = {{{S::kLeftSurround, S::kLeftBack},
                                                    {S::kRightSurround, S::kRightBack},
                                                    {S::kCentre, S::kCentre},
                                                    {S::kLeft, S::kRight},
                                                    {S::kTopFrontLeft, S::kTopBackLeft},
                                                    {S::kTopFrontRight, S::kTopBackRight}}};
    const std::array<int, 6> kIndex = {0, 1, 0, 2, 3, 4};
    for (std::size_t u = 0; u < full.size(); ++u) {
        CAPTURE(u);
        CHECK(full[u].speakers[0] == kFull[u][0]);
        CHECK(full[u].pair == (u != 2));
        if (full[u].pair) {
            CHECK(full[u].speakers[1] == kFull[u][1]);
        }
        CHECK(full[u].index == kIndex[u]);
        CHECK_FALSE(full[u].first_only);
    }
    const auto core = aspx_units(mode::k7_1_4, immersive::kAspxScpl, kCore);
    REQUIRE(core.size() == 6);
    CHECK((core[0].first_only && core[1].first_only && !core[2].first_only && !core[3].first_only &&
           core[4].first_only && core[5].first_only));
    CHECK(core[4].speakers[0] == S::kTopSideLeft);
    CHECK(core[5].speakers[0] == S::kTopSideRight);

    // ASPX_ACPL_1 and 2: (A'', B''), (D'', E''), (F'', G''), C'', in both.
    for (const int codec : {immersive::kAspxAcpl1, immersive::kAspxAcpl2}) {
        CAPTURE(codec);
        const auto acpl = aspx_units(mode::k7_0_4, codec);
        REQUIRE(acpl.size() == 4);
        CHECK(acpl[0].speakers == std::array{S::kLeft, S::kRight});
        CHECK(acpl[1].speakers == std::array{S::kLeftSurround, S::kRightSurround});
        CHECK(acpl[2].speakers == std::array{S::kTopFrontLeft, S::kTopFrontRight});
        CHECK((!acpl[3].pair && acpl[3].speakers[0] == S::kCentre));
        CHECK(aspx_units(mode::k7_0_4, codec, kCore)[2].speakers ==
              std::array{S::kTopSideLeft, S::kTopSideRight});
    }
    // ASPX_AJCC: (A'', B''), (D'', E''), C''.
    const auto ajcc = aspx_units(mode::k7_1_4, immersive::kAspxAjcc);
    REQUIRE(ajcc.size() == 3);
    CHECK((!ajcc[2].pair && ajcc[2].speakers[0] == S::kCentre));
}
