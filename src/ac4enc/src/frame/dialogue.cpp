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

// Table 172.
constexpr std::array<double, 32> kMixCoefficients = {
    0.0,   6.32e-3, 1e-2,   1.79e-2, 3.16e-2, 5.65e-2, 7.87e-2, 0.111,   0.156,   0.218, 0.303,
    0.37,  0.448,   0.533,  0.577,   0.622,   0.7071,  0.783,   0.846,   0.894,   0.929, 0.953,
    0.976, 0.9877,  0.9938, 0.9969,  0.9984,  0.9995,  0.99984, 0.99995, 0.99998, 1.0};

// Table 210: -3.0 to 3.0 in steps of 0.1, indices -30 to 30.
constexpr int kCrossLimit = 30;

// Each band's lines: Table 173's subbands, frame_length / 64 lines each.
[[nodiscard]] std::pair<std::size_t, std::size_t> band_lines(std::size_t b,
                                                             int frame_length) noexcept {
    const auto lines_per_subband = static_cast<std::size_t>(frame_length / 64);
    return {static_cast<std::size_t>(kBandSubbands[b].first) * lines_per_subband,
            static_cast<std::size_t>(kBandSubbands[b].second + 1) * lines_per_subband};
}

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
    for (std::size_t b = 0; b < kDeBands; ++b) {
        const auto [first, end] = band_lines(b, frame_length);
        const std::size_t last = std::min(end, std::min(channel.size(), dialogue.size()));
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

int de_mix_index(double c) noexcept {
    int best = 0;
    for (int i = 1; i < static_cast<int>(kMixCoefficients.size()); ++i) {
        if (std::abs(c - kMixCoefficients[static_cast<std::size_t>(i)]) <
            std::abs(c - kMixCoefficients[static_cast<std::size_t>(best)])) {
            best = i;
        }
    }
    return best;
}

DeFrameParameters de_cross_parameters(std::span<const std::vector<double>> channels,
                                      std::span<const std::vector<double>> dialogue,
                                      int frame_length) {
    DeFrameParameters out;
    const std::size_t n = std::min<std::size_t>(channels.size(), 3);
    if (n < 2 || dialogue.size() < n) {
        return out;
    }
    std::size_t lines = band_lines(kDeBands - 1, frame_length).second;
    for (std::size_t c = 0; c < n; ++c) {
        lines = std::min({lines, channels[c].size(), dialogue[c].size()});
    }
    // The dialogue's panning, as Table 172 carries it: the first one or two
    // coefficients sent, the last what they leave of a unit's energy.
    std::array<double, 3> energy{};
    double total = 0.0;
    for (std::size_t c = 0; c < n; ++c) {
        for (std::size_t k = 0; k < lines; ++k) {
            energy[c] += dialogue[c][k] * dialogue[c][k];
        }
        total += energy[c];
    }
    std::array<double, 3> r{};
    double sent = 0.0;
    for (std::size_t c = 0; c + 1 < n; ++c) {
        const double share =
            total > 0.0 ? std::sqrt(energy[c] / total) : std::sqrt(1.0 / static_cast<double>(n));
        out.mix[c] = de_mix_index(share);
        r[c] = kMixCoefficients[static_cast<std::size_t>(out.mix[c])];
        sent += r[c] * r[c];
    }
    r[n - 1] = std::sqrt(std::max(0.0, 1.0 - sent));
    if (total <= 0.0) {
        return out;  // nothing to raise: every parameter 0
    }

    // Per band, p minimising the distance of p^T m from s = r^T d over the
    // band's lines: (sum m m^T) p = sum m s, solved by elimination.
    for (std::size_t b = 0; b < kDeBands; ++b) {
        const auto [first, end] = band_lines(b, frame_length);
        const std::size_t last = std::min(end, lines);
        std::array<std::array<double, 4>, 3> a{};  // [A | y]
        for (std::size_t k = first; k < last; ++k) {
            double s = 0.0;
            for (std::size_t c = 0; c < n; ++c) {
                s += r[c] * dialogue[c][k];
            }
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    a[i][j] += channels[i][k] * channels[j][k];
                }
                a[i][3] += channels[i][k] * s;
            }
        }
        double trace = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            trace += a[i][i];
        }
        if (trace <= 0.0) {
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            a[i][i] += 1e-9 * trace;
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t row = i + 1; row < n; ++row) {
                const double factor = a[row][i] / a[i][i];
                for (std::size_t col = i; col < 4; ++col) {
                    a[row][col] -= factor * a[i][col];
                }
            }
        }
        std::array<double, 3> p{};
        for (std::size_t i = n; i-- > 0;) {
            double sum = a[i][3];
            for (std::size_t j = i + 1; j < n; ++j) {
                sum -= a[i][j] * p[j];
            }
            p[i] = sum / a[i][i];
        }
        for (std::size_t c = 0; c < n; ++c) {
            out.par[c][b] =
                std::clamp(static_cast<int>(std::lround(p[c] * 10.0)), -kCrossLimit, kCrossLimit);
        }
    }
    return out;
}

}  // namespace ac4::detail