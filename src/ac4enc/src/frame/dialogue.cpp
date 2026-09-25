#include "frame/dialogue.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace ac4::detail {
namespace {

// Table 173: each band's first and last QMF subband.
constexpr std::array<std::pair<int, int>, kDeBands> kBandSubbands{
    {{0, 0}, {1, 1}, {2, 3}, {4, 6}, {7, 10}, {11, 16}, {17, 26}, {27, 40}}};

constexpr int kIndices = 32;

}  // namespace

double de_parameter_value(int index) noexcept {
    if (index <= 15) {
        return 0.1 * index;
    }
    if (index == 16) {
        return 1.75;
    }
    if (index == 17) {
        return 2.0;
    }
    return 2.5 + 0.5 * (index - 18);
}

int de_parameter_index(double p) noexcept {
    int best = 0;
    double distance = std::abs(p);
    for (int i = 1; i < kIndices; ++i) {
        const double d = std::abs(p - de_parameter_value(i));
        if (d < distance) {
            distance = d;
            best = i;
        }
    }
    return best;
}

std::array<int, kDeBands> de_parameters(std::span<const double> channel,
                                        std::span<const double> dialogue,
                                        int frame_length) noexcept {
    std::array<int, kDeBands> out{};
    const auto lines_per_subband = static_cast<std::size_t>(frame_length / 64);
    for (std::size_t b = 0; b < kDeBands; ++b) {
        const std::size_t first =
            static_cast<std::size_t>(kBandSubbands[b].first) * lines_per_subband;
        const std::size_t last =
            std::min(static_cast<std::size_t>(kBandSubbands[b].second + 1) * lines_per_subband,
                     std::min(channel.size(), dialogue.size()));
        double cross = 0.0;
        double power = 0.0;
        for (std::size_t k = first; k < last; ++k) {
            cross += dialogue[k] * channel[k];
            power += channel[k] * channel[k];
        }
        out[b] = power > 0.0 ? de_parameter_index(std::clamp(cross / power, 0.0, 9.0)) : 0;
    }
    return out;
}

}  // namespace ac4::detail
