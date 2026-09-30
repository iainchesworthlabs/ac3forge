#include "ajoc/downmix.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace iclforge::ac4::detail {

std::vector<int> downmix_groups(std::span<const std::array<double, 3>> positions, int signals) {
    const std::size_t n = positions.size();
    std::vector<double> azimuth(n);
    for (std::size_t o = 0; o < n; ++o) {
        // 0 straight ahead, positive to the right, +-pi straight behind.
        azimuth[o] = std::atan2(positions[o][0] - 0.5, 0.5 - positions[o][1]);
    }
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return azimuth[a] < azimuth[b]; });
    std::vector<int> out(n, 0);
    const auto groups = static_cast<std::size_t>(std::max(signals, 1));
    const std::size_t base = n / groups;
    const std::size_t extra = n % groups;
    std::size_t next = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        const std::size_t size = base + (g < extra ? 1 : 0);
        for (std::size_t k = 0; k < size && next < n; ++k) {
            out[order[next++]] = static_cast<int>(g);
        }
    }
    return out;
}

std::array<double, 5> static_downmix_gains(const std::array<double, 3>& position) noexcept {
    constexpr double kQuarter = std::numbers::pi / 2.0;
    const double x = std::clamp(position[0], 0.0, 1.0);
    const double y = std::clamp(position[1], 0.0, 1.0);
    const double front = std::cos(kQuarter * y);
    const double back = std::sin(kQuarter * y);
    std::array<double, 5> g{};  // L R C Ls Rs
    if (x <= 0.5) {
        const double t = x / 0.5;
        g[0] = front * std::cos(kQuarter * t);
        g[2] = front * std::sin(kQuarter * t);
    } else {
        const double t = (x - 0.5) / 0.5;
        g[2] = front * std::cos(kQuarter * t);
        g[1] = front * std::sin(kQuarter * t);
    }
    g[3] = back * std::cos(kQuarter * x);
    g[4] = back * std::sin(kQuarter * x);
    return g;
}

int ajoc_input_track(int i, int m) noexcept {
    if (m > 3) {
        const int offset = 2 + m % 2;
        return i < offset ? m - offset + i : i - offset;
    }
    return i;
}

}  // namespace iclforge::ac4::detail
