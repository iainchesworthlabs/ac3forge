// Multichannel processing (src/ac4dec/src/pcm/multichannel.*) against the
// entries ETSI TS 103 190-1 V1.4.1 prints for it, and the routing of a
// channel element's tracks to channels (pcm/routing.*) against Tables 180,
// 182, 183, 212 and 213.
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
    CHECK(list(mode::k7_1_4).empty());
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

TEST_CASE("Tables 212 and 213 name the channels companding and A-SPX process", "[ac4dec][multichannel]") {
    namespace mode = ac4::detail::ch_mode;
    using ac4::detail::aspx_units;
    using ac4::detail::companded_speakers;
    CHECK(companded_speakers(mode::k5_1) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                                         Speaker::kLeftSurround, Speaker::kRightSurround});
    CHECK(companded_speakers(mode::k3_0) == std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre});
    CHECK(companded_speakers(mode::k7_1_340).empty());  // no companding_control() in 7.X ASPX

    const auto five = aspx_units(mode::k5_0);
    REQUIRE(five.size() == 3);
    CHECK((five[0].pair && five[0].index == 0 && five[0].speakers[1] == Speaker::kRight));
    CHECK((five[1].pair && five[1].index == 1 && five[1].speakers[0] == Speaker::kLeftSurround));
    CHECK((!five[2].pair && five[2].index == 0 && five[2].speakers[0] == Speaker::kCentre));

    // 7.X: (L, R), then (Ls, Rs) or 5/2/0's (Lw, Rw), C, then the last pair.
    const auto back = aspx_units(mode::k7_1_340);
    const auto wide = aspx_units(mode::k7_0_520);
    const auto top = aspx_units(mode::k7_0_322);
    REQUIRE(back.size() == 4);
    REQUIRE(wide.size() == 4);
    REQUIRE(top.size() == 4);
    CHECK(back[1].speakers[0] == Speaker::kLeftSurround);
    CHECK(back[3].speakers[0] == Speaker::kLeftBack);
    CHECK(wide[1].speakers[0] == Speaker::kLeftWide);
    CHECK(wide[3].speakers[0] == Speaker::kLeftSurround);
    CHECK(top[3].speakers[1] == Speaker::kTopFrontRight);
    CHECK(back[3].index == 2);
}
