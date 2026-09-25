#include "pcm/stereo.hpp"

#include <algorithm>
#include <cstddef>

namespace ac4::detail {
namespace {

constexpr std::array<double, 4> kIdentity = {1.0, 0.0, 0.0, 1.0};
constexpr std::array<double, 4> kMidSide = {1.0, 1.0, 1.0, -1.0};

// Pseudocode 59's inverse quantisation of alpha_q, with the float the text's
// 0.1f makes of it.
[[nodiscard]] std::array<double, 4> prediction(int alpha_q) noexcept {
    const auto sap_gain = static_cast<double>(static_cast<float>(alpha_q) * 0.1f);
    return {1.0 + sap_gain, 1.0, 1.0 - sap_gain, -1.0};
}

}  // namespace

StereoParameters stereo_parameters(const SubstreamContext& ctx, const SfInfo& info, const ChparamInfo& chparam) {
    StereoParameters out;
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
                    abcd = chparam.ms_used[gi][si] ? kMidSide : kIdentity;
                    break;
                case 2:
                    abcd = kMidSide;
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
                    abcd = prediction(value);
                    break;
                }
            }
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

}  // namespace ac4::detail
