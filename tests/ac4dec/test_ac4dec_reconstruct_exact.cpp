// The two speed-ups of the spectral reconstruction (src/ac4dec/src/pcm/asf_reconstruct.hpp;
// planning/ac4.md, D14e) against what they replaced, to the bit: the table of scale factor gains
// against the std::pow the reconstruction made for every band of every frame, and the in-place
// ungrouping of a frame of one long block against ungroup().

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "iclforge/ac4core/tables/sfb_tables.hpp"
#include "pcm/asf_reconstruct.hpp"

namespace {

using iclforge::ac4::detail::Real;

bool same_bits(std::span<const Real> a, std::span<const Real> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Real)) == 0;
}

}  // namespace

TEST_CASE("ungrouping a long block in place gives the lines ungroup() puts out",
          "[ac4dec][exact]") {
    namespace detail = iclforge::ac4::detail;
    detail::SubstreamContext ctx;  // a 2 048 sample frame
    detail::AsfPsyInfo psy;        // one long block, one group of one window
    psy.num_win_in_group[0] = 1;
    const std::span<const std::uint16_t> offsets = detail::tables::sfb_offsets_48(
        detail::transform_length_samples(ctx, detail::get_transf_length(ctx, psy, 0)));
    const std::vector<int> lengths = {ctx.frame_len_base};
    std::mt19937 rng(11);
    std::normal_distribution<double> normal;
    // The buffers change hands from frame to frame, as the stage's do: what `spectrum` held before
    // is not the frame's.
    std::vector<Real> spectrum(5, Real(9));
    std::vector<Real> work;
    for (int frame = 0; frame < 60; ++frame) {
        const auto bands = static_cast<std::size_t>(rng() % offsets.size());
        auto data = std::make_unique<detail::SfData>();
        data->max_sfb[0] = static_cast<int>(bands);
        std::vector<Real> scaled(offsets[bands]);
        for (Real& v : scaled) {
            v = rng() % 6 == 0 ? Real(0) : static_cast<Real>(normal(rng) * 100.0);
        }
        std::vector<Real> expected(3, Real(7));
        detail::ungroup(ctx, psy, *data, lengths, scaled, expected);
        work = scaled;
        CAPTURE(frame, bands);
        REQUIRE(detail::ungroup_in_place(ctx, psy, *data, lengths, work, spectrum));
        REQUIRE(spectrum.size() == expected.size());
        CHECK(same_bits(spectrum, expected));
    }
    // A frame of several windows or groups is ungroup()'s.
    auto data = std::make_unique<detail::SfData>();
    data->max_sfb[0] = 3;
    std::vector<Real> scaled(offsets[3], Real(1));
    detail::AsfPsyInfo grouped = psy;
    grouped.num_window_groups = 2;
    CHECK_FALSE(detail::ungroup_in_place(ctx, grouped, *data, lengths, scaled, spectrum));
    grouped = psy;
    grouped.num_win_in_group[0] = 2;
    CHECK_FALSE(detail::ungroup_in_place(ctx, grouped, *data, lengths, scaled, spectrum));
    CHECK_FALSE(
        detail::ungroup_in_place(ctx, psy, *data, std::vector<int>{1024, 1024}, scaled, spectrum));
    scaled.push_back(Real(1));  // a line more than the bands cover
    CHECK_FALSE(detail::ungroup_in_place(ctx, psy, *data, lengths, scaled, spectrum));
}

TEST_CASE("the scale factor gains are the std::pow the reconstruction made for each band",
          "[ac4dec][exact]") {
    const iclforge::ac4::detail::ScaleFactorGains gains =
        iclforge::ac4::detail::scale_factor_gains();
    // At Fixed32 the table is empty: that tier forms each gain as a MantExp (pcm/asf_reconstruct.cpp).
    if (iclforge::ac4::detail::dsp::kFixed<Real>) {
        CHECK(gains == iclforge::ac4::detail::ScaleFactorGains{});
        return;
    }
    for (int sf = 0; sf < 256; ++sf) {
        CAPTURE(sf);
        const auto before = static_cast<Real>(std::pow(2.0, 0.25 * static_cast<double>(sf - 100)));
        CHECK(std::memcmp(&gains[static_cast<std::size_t>(sf)], &before, sizeof before) == 0);
    }
}
