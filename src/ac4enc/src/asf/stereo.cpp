#include "asf/stereo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>

#include "tables/huffman_codes.hpp"

namespace ac4::detail {
namespace {

// The largest |alpha_q| written. a = +-1 predicts a source panned to one side,
// and up to +-3 one between the channels in opposite phase; past that M is so
// much weaker than S that left and right cost less. Two pairs' alpha_q then
// differ by at most 60, the largest delta Table A.1 sends.
constexpr int kMaxAlphaQ = 30;

// Perceptual entropy of a band, in bits: half a bit per line for each factor
// of two its energy stands above the noise it may carry.
[[nodiscard]] double entropy(double energy, double allowed, double lines) {
    if (energy <= allowed || allowed <= 0.0) {
        return 0.0;
    }
    return 0.5 * lines * std::log2(energy / allowed);
}

// Pseudocode 59's a, with the float the decoder's 0.1f makes of it.
[[nodiscard]] double alpha_of(int alpha_q) {
    return static_cast<double>(static_cast<float>(alpha_q) * 0.1f);
}

// What X0 may carry under the prediction, which reaches L with the gain 1 + a
// and R with 1 - a: the smaller of the two outputs' allowances over its gain.
[[nodiscard]] double allowed_x0(double allowed_l, double allowed_r, double a) {
    double out = std::numeric_limits<double>::max();
    const double gain_l = (1.0 + a) * (1.0 + a);
    const double gain_r = (1.0 - a) * (1.0 - a);
    if (gain_l > 0.0) {
        out = std::min(out, allowed_l / gain_l);
    }
    if (gain_r > 0.0) {
        out = std::min(out, allowed_r / gain_r);
    }
    return out;
}

[[nodiscard]] std::size_t sap_codeword_bits(int delta) {
    return tables::kAsfHcbScalefacCodes[static_cast<std::size_t>(delta + 60)].bits;
}

struct Band {
    std::size_t begin = 0;
    std::size_t end = 0;
    double e_l = 0.0;
    double e_r = 0.0;
    double e_m = 0.0;
    double e_s = 0.0;
    double c_ms = 0.0;  // the sum of M times S
    double lr = 0.0;    // the costs of left and right, and of M/S
    double ms = 0.0;
};

}  // namespace

StereoChoice choose_stereo(Grouped& left, Grouped& right, std::vector<std::vector<double>>& allowed_left,
                           std::vector<std::vector<double>>& allowed_right) {
    const std::size_t groups = left.offset.size();
    std::vector<std::vector<Band>> bands(groups);
    double cost_lr = 0.0;
    double cost_ms = 0.0;
    double cost_per_band = 0.0;
    for (std::size_t g = 0; g < groups; ++g) {
        const auto count = static_cast<std::size_t>(left.max_sfb[g]);
        bands[g].resize(count);
        for (std::size_t b = 0; b < count; ++b) {
            Band& band = bands[g][b];
            band.begin = left.offset[g][b];
            band.end = left.offset[g][b + 1];
            for (std::size_t k = band.begin; k < band.end; ++k) {
                const double l = left.lines[k];
                const double r = right.lines[k];
                const double m = 0.5 * (l + r);
                const double s = 0.5 * (l - r);
                band.e_l += l * l;
                band.e_r += r * r;
                band.e_m += m * m;
                band.e_s += s * s;
                band.c_ms += m * s;
            }
            const auto lines = static_cast<double>(band.end - band.begin);
            const double allowed_ms = std::min(allowed_left[g][b], allowed_right[g][b]);
            band.lr = entropy(band.e_l, allowed_left[g][b], lines) + entropy(band.e_r, allowed_right[g][b], lines);
            band.ms = entropy(band.e_m, allowed_ms, lines) + entropy(band.e_s, allowed_ms, lines);
            cost_lr += band.lr;
            cost_ms += band.ms;
            cost_per_band += std::min(band.lr, band.ms) + 1.0;  // and its ms_used
        }
    }

    // The prediction, pair by pair: a from the pair's M and S, then the pair
    // predicted where that costs less than left and right, its alpha_q sent
    // against the pair below (0 where that pair is not predicted).
    StereoChoice predicted;
    predicted.sap_mode = 3;
    predicted.sap_used.resize(groups);
    predicted.alpha_q.resize(groups);
    double cost_prediction = 1.0 + (groups != 1 ? 1.0 : 0.0);  // sap_coeff_all, delta_code_time
    double pair_flags = 0.0;
    bool all_used = true;
    for (std::size_t g = 0; g < groups; ++g) {
        const std::size_t count = bands[g].size();
        const std::size_t pairs = (count + 1) / 2;
        predicted.sap_used[g].assign(pairs, false);
        predicted.alpha_q[g].assign(pairs, 0);
        int below = 0;
        for (std::size_t p = 0; p < pairs; ++p) {
            const std::size_t last = std::min(2 * p + 2, count);
            double e_m = 0.0;
            double c_ms = 0.0;
            double lr = 0.0;
            for (std::size_t b = 2 * p; b < last; ++b) {
                e_m += bands[g][b].e_m;
                c_ms += bands[g][b].c_ms;
                lr += bands[g][b].lr;
            }
            const double best = e_m > 0.0 ? c_ms / e_m : 0.0;
            const int q = static_cast<int>(std::clamp(std::lround(best * 10.0), -static_cast<long>(kMaxAlphaQ),
                                                      static_cast<long>(kMaxAlphaQ)));
            const double a = alpha_of(q);
            double cost = static_cast<double>(sap_codeword_bits(q - below));
            for (std::size_t b = 2 * p; b < last; ++b) {
                const Band& band = bands[g][b];
                const auto lines = static_cast<double>(band.end - band.begin);
                const double residual = std::max(0.0, band.e_s - 2.0 * a * band.c_ms + a * a * band.e_m);
                cost += entropy(band.e_m, allowed_x0(allowed_left[g][b], allowed_right[g][b], a), lines) +
                        entropy(residual, std::min(allowed_left[g][b], allowed_right[g][b]), lines);
            }
            pair_flags += 1.0;
            if (cost < lr) {
                predicted.sap_used[g][p] = true;
                predicted.alpha_q[g][p] = q;
                cost_prediction += cost;
                below = q;
            } else {
                all_used = false;
                cost_prediction += lr;
                below = 0;
            }
        }
    }
    predicted.sap_coeff_all = all_used;
    if (!all_used) {
        cost_prediction += pair_flags;
    }

    StereoChoice choice;
    choice.ms_used.resize(groups);
    double cheapest = cost_lr;
    int mode = 0;
    for (const auto& [candidate, cost] : {std::pair{1, cost_per_band}, std::pair{2, cost_ms}, std::pair{3, cost_prediction}}) {
        if (cost < cheapest) {
            cheapest = cost;
            mode = candidate;
        }
    }
    if (mode == 3) {
        choice = std::move(predicted);
    }
    choice.sap_mode = mode;

    for (std::size_t g = 0; g < groups; ++g) {
        if (mode == 1) {
            choice.ms_used[g].assign(bands[g].size(), false);
        }
        for (std::size_t b = 0; b < bands[g].size(); ++b) {
            const Band& band = bands[g][b];
            const double smaller = std::min(allowed_left[g][b], allowed_right[g][b]);
            bool mid_side = mode == 2;
            if (mode == 1 && band.ms < band.lr) {
                choice.ms_used[g][b] = true;
                mid_side = true;
            }
            double a = 0.0;
            if (mode == 3 && choice.sap_used[g][b / 2]) {
                a = alpha_of(choice.alpha_q[g][b / 2]);
                mid_side = true;
            }
            if (!mid_side) {
                continue;
            }
            for (std::size_t k = band.begin; k < band.end; ++k) {
                const double m = 0.5 * (left.lines[k] + right.lines[k]);
                const double s = 0.5 * (left.lines[k] - right.lines[k]);
                left.lines[k] = m;
                right.lines[k] = s - a * m;
            }
            allowed_left[g][b] = mode == 3 ? allowed_x0(allowed_left[g][b], allowed_right[g][b], a) : smaller;
            allowed_right[g][b] = smaller;
        }
    }
    return choice;
}

void write_chparam_info(BitWriter& w, const StereoChoice& choice) {
    w.write(2, static_cast<std::uint64_t>(choice.sap_mode), "sap_mode");
    if (choice.sap_mode == 1) {
        for (const std::vector<bool>& group : choice.ms_used) {
            for (const bool used : group) {
                w.write(1, used ? 1U : 0U, "ms_used");
            }
        }
    }
    if (choice.sap_mode == 3) {
        // sap_data(), Table 48.
        w.write(1, choice.sap_coeff_all ? 1U : 0U, "sap_coeff_all");
        if (!choice.sap_coeff_all) {
            for (const std::vector<bool>& group : choice.sap_used) {
                for (const bool used : group) {
                    w.write(1, used ? 1U : 0U, "sap_coeff_used");
                }
            }
        }
        if (choice.sap_used.size() != 1) {
            w.write(1, 0, "delta_code_time");
        }
        for (std::size_t g = 0; g < choice.sap_used.size(); ++g) {
            int below = 0;
            for (std::size_t p = 0; p < choice.sap_used[g].size(); ++p) {
                if (!choice.sap_used[g][p]) {
                    below = 0;
                    continue;
                }
                const int alpha_q = choice.alpha_q[g][p];
                w.write_codeword(tables::kAsfHcbScalefacCodes, static_cast<std::size_t>(alpha_q - below + 60),
                                 "sap_hcw");
                below = alpha_q;
            }
        }
    }
}

std::size_t chparam_info_bits(const StereoChoice& choice) {
    BitWriter w;
    write_chparam_info(w, choice);
    return w.bit_position();
}

}  // namespace ac4::detail
