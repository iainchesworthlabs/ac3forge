#include "asf/psycho.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

#include "dsp/kbd.hpp"
#include "dsp/mdct.hpp"

namespace ac4::detail {
namespace {

constexpr double kSlopeUp = 15.0;            // dB per Bark, towards higher frequencies
constexpr double kSlopeDown = 30.0;          // dB per Bark, towards lower frequencies
constexpr double kNoiseOffset = 5.5;         // dB below a noise masker
constexpr double kToneOffsetBase = 14.5;     // dB below a tone masker, plus its Bark
// The floor, dB below a full-scale sine's energy in its line: about what 16-bit
// PCM's own quantisation noise puts in one line of the longest transform.
constexpr double kFloorDb = 126.0;

// The Bark frequency of `hz` (E. Zwicker and E. Terhardt, JASA 68(5), 1980).
[[nodiscard]] double bark(double hz) {
    return 13.0 * std::atan(0.00076 * hz) + 3.5 * std::atan((hz / 7500.0) * (hz / 7500.0));
}

}  // namespace

Psychoacoustics::Psychoacoustics(int sample_rate_hz, int frame_length)
    : sample_rate_(sample_rate_hz), frame_length_(frame_length) {
    // What a full-scale sine puts in its strongest line, per transform length,
    // through the same windowed transform and scale as Analysis: a sine centred
    // on a line near 1 kHz, measured rather than derived.
    for (std::size_t k = 0; k < full_scale_.size(); ++k) {
        const int n = frame_length >> k;
        const auto size = static_cast<std::size_t>(n);
        const double alpha = dsp::kbd_alpha(n, 1);
        if (alpha == 0.0) {
            continue;
        }
        const std::vector<double> window = dsp::kbd_left(n, alpha);
        dsp::Mdct<double> mdct(size);
        if (!mdct.valid()) {
            continue;
        }
        const double line = std::round(1000.0 * 2.0 * static_cast<double>(n) / sample_rate_hz);
        const double hz = (line + 0.5) * static_cast<double>(sample_rate_hz) / (2.0 * static_cast<double>(n));
        std::vector<double> block(2 * size);
        for (std::size_t i = 0; i < 2 * size; ++i) {
            const double w = i < size ? window[i] : window[2 * size - 1 - i];
            block[i] = w * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) /
                                    static_cast<double>(sample_rate_hz));
        }
        std::vector<double> lines(size);
        mdct.forward(block, lines);
        double peak = 0.0;
        for (const double x : lines) {
            peak = std::max(peak, 65536.0 * 65536.0 * x * x);
        }
        full_scale_[k] = peak;
    }
}

double Psychoacoustics::full_scale_line_energy(int transform_length) const noexcept {
    for (std::size_t k = 0; k < full_scale_.size(); ++k) {
        if ((frame_length_ >> k) == transform_length) {
            return full_scale_[k];
        }
    }
    return full_scale_[0];
}

std::vector<std::vector<double>> Psychoacoustics::thresholds(const Grouped& grouped,
                                                             const FrameLayout& layout) const {
    std::vector<std::vector<double>> out(grouped.offset.size());
    for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
        const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
        const int length = layout.group_length[g];
        const std::span<const std::uint16_t> offsets = band_offsets(length);
        const double line_hz = static_cast<double>(sample_rate_) / (2.0 * static_cast<double>(length));
        const double full_scale = full_scale_line_energy(length);

        std::vector<double> masking(bands, 0.0);
        std::vector<double> z(bands, 0.0);
        const double line_floor = full_scale * std::pow(10.0, -kFloorDb / 10.0);
        for (std::size_t b = 0; b < bands; ++b) {
            const std::size_t begin = grouped.offset[g][b];
            const std::size_t end = grouped.offset[g][b + 1];
            double energy = 0.0;
            double log_sum = 0.0;
            for (std::size_t k = begin; k < end; ++k) {
                const double e = grouped.lines[k] * grouped.lines[k];
                energy += e;
                log_sum += std::log(e + 1e-9);
            }
            const auto count = static_cast<double>(end - begin);
            const double centre_hz = 0.5 * (offsets[b] + offsets[b + 1]) * line_hz;
            z[b] = bark(centre_hz);
            // Spectral flatness, dB: 0 for a flat band, far below for a tone.
            const double mean = energy / count;
            const double flatness_db =
                mean > 0.0 ? 10.0 * (log_sum / count - std::log(mean + 1e-9)) / std::numbers::ln10 : 0.0;
            const double tonality = std::clamp(flatness_db / -60.0, 0.0, 1.0);
            const double offset_db = tonality * (kToneOffsetBase + z[b]) + (1.0 - tonality) * kNoiseOffset;
            masking[b] = energy * std::pow(10.0, -offset_db / 10.0);
        }
        // Spreading: the largest of a band's own threshold and its neighbours',
        // each lowered by the slope over the Bark distance, weighted by width.
        std::vector<double> spread = masking;
        for (std::size_t b = 1; b < bands; ++b) {
            const double width_ratio = static_cast<double>(offsets[b + 1] - offsets[b]) /
                                       static_cast<double>(offsets[b] - offsets[b - 1]);
            const double drop = std::pow(10.0, -kSlopeUp * (z[b] - z[b - 1]) / 10.0);
            spread[b] = std::max(spread[b], spread[b - 1] * drop * width_ratio);
        }
        for (std::size_t b = bands; b-- > 1;) {
            const double width_ratio = static_cast<double>(offsets[b] - offsets[b - 1]) /
                                       static_cast<double>(offsets[b + 1] - offsets[b]);
            const double drop = std::pow(10.0, -kSlopeDown * (z[b] - z[b - 1]) / 10.0);
            spread[b - 1] = std::max(spread[b - 1], spread[b] * drop * width_ratio);
        }
        out[g].resize(bands);
        for (std::size_t b = 0; b < bands; ++b) {
            const auto lines = static_cast<double>(grouped.offset[g][b + 1] - grouped.offset[g][b]);
            out[g][b] = std::max(spread[b], line_floor * lines);
        }
    }
    return out;
}

std::vector<std::vector<int>> scale_factors_for(const Grouped& grouped,
                                               const std::vector<std::vector<double>>& allowed) {
    std::vector<std::vector<int>> sf(grouped.offset.size());
    for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
        const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
        sf[g].assign(bands, 255);
        for (std::size_t b = 0; b < bands; ++b) {
            double root_sum = 0.0;
            for (std::size_t k = grouped.offset[g][b]; k < grouped.offset[g][b + 1]; ++k) {
                root_sum += std::sqrt(std::abs(grouped.lines[k]));
            }
            if (root_sum <= 0.0 || allowed[g][b] <= 0.0) {
                continue;
            }
            const double gain = std::pow(allowed[g][b] * 27.0 / 4.0 / root_sum, 2.0 / 3.0);
            const double value = 100.0 + 4.0 * std::log2(gain);
            sf[g][b] = static_cast<int>(std::clamp(std::floor(value), 0.0, 255.0));
        }
    }
    return sf;
}

}  // namespace ac4::detail
