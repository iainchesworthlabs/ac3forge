#include "dsp/kbd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace ac4::detail::dsp {
namespace {

struct AlphaRow {
    std::array<int, 3> lengths;  // at 44.1 or 48 kHz; 44.1 kHz uses the first column only
    double alpha;
};

// Table 186, the 44.1/48 kHz columns. The 96 and 192 kHz columns are these
// lengths times 2 and 4.
constexpr std::array<AlphaRow, 5> kAlphas{{
    {{2048, 1920, 1536}, 3.0},
    {{1024, 960, 768}, 4.0},
    {{512, 480, 384}, 4.5},
    {{256, 240, 192}, 5.0},
    {{128, 120, 96}, 6.0},
}};

}  // namespace

double kbd_alpha(int length, int rate_multiplier) noexcept {
    if (rate_multiplier != 1 && rate_multiplier != 2 && rate_multiplier != 4) {
        return 0.0;
    }
    if (length <= 0 || length % rate_multiplier != 0) {
        return 0.0;
    }
    const int base = length / rate_multiplier;
    for (const AlphaRow& row : kAlphas) {
        for (const int candidate : row.lengths) {
            if (candidate == base) {
                return row.alpha;
            }
        }
    }
    return 0.0;
}

double bessel_i0(double x) noexcept {
    // Terms grow while (x/2)/k > 1, then fall faster than geometrically; stop
    // once one no longer changes the sum.
    const double half = x / 2.0;
    double term = 1.0;  // ((x/2)^k / k!), squared when added
    double sum = 1.0;
    for (int k = 1; k < 500; ++k) {
        term *= half / static_cast<double>(k);
        const double add = term * term;
        const double next = sum + add;
        if (next == sum) {
            break;
        }
        sum = next;
    }
    return sum;
}

std::vector<double> kbd_left(int length, double alpha) {
    std::vector<double> window;
    if (length <= 0) {
        return window;
    }
    const auto n_count = static_cast<std::size_t>(length);
    const double pi_alpha = std::numbers::pi * alpha;
    const double norm = bessel_i0(pi_alpha);
    // cumulative[p] = sum_{q=0..p} W(N, q, alpha), for p = 0 .. N.
    std::vector<double> cumulative(n_count + 1);
    double running = 0.0;
    for (std::size_t p = 0; p <= n_count; ++p) {
        const double t = 2.0 * static_cast<double>(p) / static_cast<double>(length) - 1.0;
        const double radicand = std::max(0.0, 1.0 - t * t);
        running += bessel_i0(pi_alpha * std::sqrt(radicand)) / norm;
        cumulative[p] = running;
    }
    const double total = cumulative[n_count];
    window.resize(n_count);
    for (std::size_t n = 0; n < n_count; ++n) {
        window[n] = std::sqrt(cumulative[n] / total);
    }
    return window;
}

}  // namespace ac4::detail::dsp
