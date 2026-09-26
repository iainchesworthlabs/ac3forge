#include "pcm/stereo.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "tables/sfb_tables.hpp"

namespace ac4::detail {
namespace {

constexpr std::array<double, 4> kIdentity = {1.0, 0.0, 0.0, 1.0};
constexpr std::array<double, 4> kMidSide = {1.0, 1.0, 1.0, -1.0};

// Pseudocode 59's inverse quantisation of alpha_q, with the float the text's
// 0.1f makes of it.
[[nodiscard]] double sap_gain(int alpha_q) noexcept {
    return static_cast<double>(static_cast<float>(alpha_q) * 0.1f);
}

[[nodiscard]] std::array<double, 4> prediction(int alpha_q) noexcept {
    const double gain = sap_gain(alpha_q);
    return {1.0 + gain, 1.0, 1.0 - gain, -1.0};
}

}  // namespace

StereoParameters stereo_parameters(const SubstreamContext& ctx, const SfInfo& info,
                                   const ChparamInfo& chparam, StereoUse use) {
    StereoParameters out;
    const bool pair = use == StereoUse::kPair;
    const AsfPsyInfo& psy = info.psy;
    // alpha_q of a band sap_data() sent no coefficient for is never read by a
    // well-formed stream; it is 0 here rather than whatever it last held.
    std::array<std::array<int, kMaxSfb>, kMaxWindows> alpha_q{};
    int max_sfb_prev = std::min(get_max_sfb(ctx, psy, 0, false), kMaxSfb);
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const int max_sfb_g = std::min(get_max_sfb(ctx, psy, g, false), kMaxSfb);
        for (int sfb = 0; sfb < max_sfb_g; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            std::array<double, 4>& abcd = out.abcd[gi][si];
            switch (chparam.sap_mode) {
                case 0:
                    abcd = kIdentity;
                    break;
                case 1:
                    abcd = pair && chparam.ms_used[gi][si] ? kMidSide : kIdentity;
                    break;
                case 2:
                    abcd = pair ? kMidSide : kIdentity;
                    break;
                default: {  // sap_mode 3
                    if (!chparam.sap_coeff_used[gi][si]) {
                        abcd = kIdentity;
                        break;
                    }
                    int& value = alpha_q[gi][si];
                    if (sfb % 2 != 0) {
                        value = alpha_q[gi][si - 1];
                    } else {
                        const int delta = chparam.dpcm_alpha_q[gi][si] - 60;
                        const bool code_delta = g != 0 && max_sfb_g == max_sfb_prev && chparam.delta_code_time;
                        if (code_delta) {
                            value = alpha_q[gi - 1][si] + delta;
                        } else if (sfb == 0) {
                            value = delta;
                        } else {
                            value = alpha_q[gi][si - 2] + delta;
                        }
                    }
                    abcd = pair ? prediction(value)
                                : std::array<double, 4>{1.0, 0.0, sap_gain(value), 1.0};
                    break;
                }
            }
        }
        for (int sfb = std::max(max_sfb_g, 0); sfb < kMaxSfb; ++sfb) {
            out.abcd[gi][static_cast<std::size_t>(sfb)] = kIdentity;
        }
        max_sfb_prev = max_sfb_g;
    }
    return out;
}

void apply_stereo(const SfInfo& info, const SfData& layout, const StereoParameters& parameters,
                  std::span<double> track0, std::span<double> track1) {
    for (int g = 0; g < info.psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < layout.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const auto [a, b, c, d] = parameters.abcd[gi][si];
            const std::size_t begin = layout.sect_sfb_offset[gi][si];
            const std::size_t end = layout.sect_sfb_offset[gi][si + 1];
            for (std::size_t k = begin; k < end; ++k) {
                const double i0 = track0[k];
                const double i1 = track1[k];
                track0[k] = a * i0 + b * i1;
                track1[k] = c * i0 + d * i1;
            }
        }
    }
}

void align_tracks(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& first,
                  const SfData& second, std::vector<double>& track0, std::vector<double>& track1,
                  SfData& common) {
    std::vector<double> out0;
    std::vector<double> out1;
    std::size_t k0 = 0;
    std::size_t k1 = 0;
    std::size_t line = 0;
    // A band's lines from a track that sends it, or zeros.
    const auto take = [](const std::vector<double>& from, std::size_t& k, bool sent,
                         std::size_t lines, std::vector<double>& to) {
        for (std::size_t i = 0; i < lines; ++i) {
            to.push_back(sent && k < from.size() ? from[k++] : 0.0);
        }
    };
    common.max_sfb = {};
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const std::span<const std::uint16_t> offsets =
            tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, psy, g)));
        const std::size_t windows = psy.num_win_in_group[gi];
        // Each track's max_sfb was checked against its transform's bands when
        // it was read.
        const int bands = std::min(std::max(first.max_sfb[gi], second.max_sfb[gi]),
                                   static_cast<int>(offsets.size()) - 1);
        common.max_sfb[gi] = std::max(bands, 0);
        for (int sfb = 0; sfb < bands; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            common.sect_sfb_offset[gi][si] = static_cast<std::uint16_t>(line);
            const std::size_t lines =
                static_cast<std::size_t>(offsets[si + 1] - offsets[si]) * windows;
            take(track0, k0, sfb < first.max_sfb[gi], lines, out0);
            take(track1, k1, sfb < second.max_sfb[gi], lines, out1);
            line += lines;
        }
        common.sect_sfb_offset[gi][static_cast<std::size_t>(common.max_sfb[gi])] =
            static_cast<std::uint16_t>(line);
    }
    track0 = std::move(out0);
    track1 = std::move(out1);
}

}  // namespace ac4::detail
