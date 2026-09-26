#include "pcm/de.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ac4::detail {
namespace {

constexpr int kSubbands = 64;

// Table 173: the first QMF subband of each parameter band, and one past the
// last band's.
constexpr std::array<int, kDeNrBands + 1> kBandStart = {0, 1, 2, 4, 7, 11, 17, 27, 41};

// Table 172.
constexpr std::array<double, 32> kMixCoefficients = {
    0.0,   6.32e-3, 1e-2,   1.79e-2, 3.16e-2, 5.65e-2, 7.87e-2, 0.111,   0.156,   0.218, 0.303,
    0.37,  0.448,   0.533,  0.577,   0.622,   0.7071,  0.783,   0.846,   0.894,   0.929, 0.953,
    0.976, 0.9877,  0.9938, 0.9969,  0.9984,  0.9995,  0.99984, 0.99995, 0.99998, 1.0,
};

}  // namespace

double de_parameter(int index, bool cross_channel) noexcept {
    if (cross_channel) {
        // Table 210: -3.0 to 3.0 in steps of 0.1.
        return 0.1 * static_cast<double>(std::clamp(index, -30, 30));
    }
    // Table 209: 0 to 1.5 in steps of 0.1, then 1.75, 2.0, and 2.5 to 9.0 in
    // steps of 0.5.
    const int i = std::clamp(index, 0, 31);
    if (i <= 15) {
        return 0.1 * static_cast<double>(i);
    }
    if (i == 16) {
        return 1.75;
    }
    if (i == 17) {
        return 2.0;
    }
    return 2.5 + 0.5 * static_cast<double>(i - 18);
}

double de_mix_coefficient(int index) noexcept {
    return kMixCoefficients[static_cast<std::size_t>(std::clamp(index, 0, 31))];
}

std::array<double, kDeFront> de_rendering(int nr_channels, double coef1, double coef2) noexcept {
    switch (nr_channels) {
        case 2:
            return {coef1, std::sqrt(std::max(0.0, 1.0 - coef1 * coef1)), 0.0};
        case 3:
            return {coef1, coef2, std::sqrt(std::max(0.0, 1.0 - coef1 * coef1 - coef2 * coef2))};
        default:
            return {1.0, 0.0, 0.0};
    }
}

DeFrameValues de_frame_values(const DialogEnhancement& de) {
    DeFrameValues values;
    if (!de.b_de_data_present || de.de_nr_channels <= 0) {
        return values;
    }
    values.active = true;
    values.method = de.config.de_method;
    values.max_gain_db = 3.0 * static_cast<double>(de.config.de_max_gain + 1);
    const int config = de.config.de_channel_config;
    values.processed = {(config & 4) != 0, (config & 2) != 0, (config & 1) != 0};
    const bool cross = values.method == 1 || values.method == 3;
    const DeData& data = de.data;
    values.ms = data.de_ms_proc_flag;
    for (int i = 0; i < de.de_nr_channels && i < kDeFront; ++i) {
        for (int band = 0; band < kDeNrBands; ++band) {
            values.p[static_cast<std::size_t>(i)][static_cast<std::size_t>(band)] = de_parameter(
                data.de_par[static_cast<std::size_t>(i)][static_cast<std::size_t>(band)], cross);
        }
    }
    values.r = de_rendering(de.de_nr_channels, de_mix_coefficient(data.de_mix_coef1_idx),
                            de_mix_coefficient(data.de_mix_coef2_idx));
    values.alpha_c = static_cast<double>(data.de_signal_contribution) / 31.0;
    return values;
}

void DeStage::configure(int slots, std::span<const Speaker> speakers) {
    slots_ = slots;
    constexpr std::array<Speaker, kDeFront> kFront = {Speaker::kLeft, Speaker::kRight,
                                                      Speaker::kCentre};
    for (std::size_t f = 0; f < kFront.size(); ++f) {
        const auto it = std::ranges::find(speakers, kFront[f]);
        channel_[f] = it == speakers.end() ? -1 : static_cast<int>(it - speakers.begin());
    }
    reset();
}

void DeStage::reset() noexcept {
    previous_.fill(identity());
    previous_identity_ = true;
}

DeStage::Matrix DeStage::identity() noexcept {
    Matrix m{};
    for (std::size_t i = 0; i < kDeFront; ++i) {
        m[i][i] = 1.0;
    }
    return m;
}

std::array<DeStage::Matrix, kDeNrBands> DeStage::frame_matrices(double gain_db,
                                                                const DeFrameValues& values) const {
    std::array<Matrix, kDeNrBands> out;
    out.fill(identity());
    if (!values.active || gain_db <= 0.0 || values.max_gain_db <= 0.0) {
        return out;
    }
    // Clause 5.7.8.7: g = 10^(G/20) - 1, with G capped at G_max.
    const double g = std::pow(10.0, std::min(gain_db, values.max_gain_db) / 20.0) - 1.0;
    // The processed channels in L, R, C order; p[i] is the i-th's.
    std::array<std::size_t, kDeFront> front{};
    std::size_t count = 0;
    for (std::size_t c = 0; c < kDeFront; ++c) {
        if (values.processed[c]) {
            front[count++] = c;
        }
    }
    const bool cross = values.method == 1 || values.method == 3;
    for (std::size_t band = 0; band < kDeNrBands; ++band) {
        Matrix& h = out[band];
        if (cross) {
            // Clause 5.7.8.8: Y = (I + g r p^T) m.
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t j = 0; j < count; ++j) {
                    h[front[i]][front[j]] += g * values.r[i] * values.p[j][band];
                }
            }
        } else if (values.ms && count == 2) {
            // Clause 5.7.8.7 with de_ms_proc_flag: the Mid alone,
            // 1/2 [1 1; 1 -1] diag(1 + g p0, 1) [1 1; 1 -1].
            const double half = 0.5 * g * values.p[0][band];
            const std::size_t a = front[0];
            const std::size_t b = front[1];
            h[a][a] = 1.0 + half;
            h[a][b] = half;
            h[b][a] = half;
            h[b][b] = 1.0 + half;
        } else {
            // Clause 5.7.8.7: Y_i = m_i + g p_i m_i.
            for (std::size_t i = 0; i < count; ++i) {
                h[front[i]][front[i]] = 1.0 + g * values.p[i][band];
            }
        }
    }
    return out;
}

bool DeStage::active(double gain_db, const DeFrameValues& values) const noexcept {
    return !previous_identity_ || (values.active && gain_db > 0.0 && values.max_gain_db > 0.0);
}

void DeStage::process(double gain_db, const DeFrameValues& values,
                      std::span<std::vector<QmfValue>* const> matrices) {
    const std::array<Matrix, kDeNrBands> current = frame_matrices(gain_db, values);
    const Matrix unit = identity();
    const bool current_identity =
        std::ranges::all_of(current, [&unit](const Matrix& m) { return m == unit; });
    if (current_identity && previous_identity_) {
        return;  // the tool bypassed, exactly
    }
    const auto at = [&](std::size_t c, int slot, int k) -> QmfValue* {
        const int channel = channel_[c];
        if (channel < 0 || static_cast<std::size_t>(channel) >= matrices.size()) {
            return nullptr;
        }
        return matrices[static_cast<std::size_t>(channel)]->data() +
               static_cast<std::size_t>(slot * kSubbands + k);
    };
    for (int n = 0; n < slots_; ++n) {
        // Clause 5.7.8.6: from the previous frame's matrix to this one's.
        const double w = (static_cast<double>(n) + 0.5) / static_cast<double>(slots_);
        for (std::size_t band = 0; band < kDeNrBands; ++band) {
            Matrix h{};
            for (std::size_t i = 0; i < kDeFront; ++i) {
                for (std::size_t j = 0; j < kDeFront; ++j) {
                    h[i][j] = (1.0 - w) * previous_[band][i][j] + w * current[band][i][j];
                }
            }
            for (int k = kBandStart[band]; k < kBandStart[band + 1]; ++k) {
                std::array<QmfValue, kDeFront> m{};
                std::array<QmfValue*, kDeFront> where{};
                for (std::size_t c = 0; c < kDeFront; ++c) {
                    where[c] = at(c, n, k);
                    m[c] = where[c] != nullptr ? *where[c] : QmfValue{};
                }
                for (std::size_t i = 0; i < kDeFront; ++i) {
                    if (where[i] == nullptr) {
                        continue;
                    }
                    QmfValue y{};
                    for (std::size_t j = 0; j < kDeFront; ++j) {
                        y += h[i][j] * m[j];
                    }
                    *where[i] = y;
                }
            }
        }
    }
    previous_ = current;
    previous_identity_ = current_identity;
}

}  // namespace ac4::detail
