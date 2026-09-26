#include "pcm/immersive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numbers>

#include "dsp/qmf.hpp"
#include "syntax/channel_elements.hpp"

namespace ac4::detail {
namespace {

using S = Speaker;

constexpr double kSqrt2 = std::numbers::sqrt2;

// Pseudocode 1: -1.5 dB, as printed.
constexpr double kPostProcessing = 0.841395;

// Table 23's coupled pairs: the channel holding D'', E'', F'' or G'' and the
// one holding H'', I'', J'' or K''.
constexpr std::array<std::array<S, 2>, 4> kCoupled = {{{S::kLeftSurround, S::kLeftBack},
                                                       {S::kRightSurround, S::kRightBack},
                                                       {S::kTopFrontLeft, S::kTopBackLeft},
                                                       {S::kTopFrontRight, S::kTopBackRight}}};

[[nodiscard]] std::vector<double>* channel(std::span<const Speaker> speakers,
                                           std::span<std::vector<double>> time,
                                           Speaker speaker) noexcept {
    for (std::size_t c = 0; c < speakers.size() && c < time.size(); ++c) {
        if (speakers[c] == speaker) {
            return &time[c];
        }
    }
    return nullptr;
}

void scale(std::vector<double>& samples, double gain) noexcept {
    for (double& x : samples) {
        x *= gain;
    }
}

}  // namespace

void apply_scpl(int codec_mode, DecodingMode decoding, std::span<const Speaker> speakers,
                std::span<std::vector<double>> time) {
    if (codec_mode != immersive_mode::kScpl && codec_mode != immersive_mode::kAspxScpl) {
        return;
    }
    const bool scpl = codec_mode == immersive_mode::kScpl;
    const double c_gain = scpl ? 2.0 : 1.0;
    if (decoding == DecodingMode::kCore) {
        for (std::size_t c = 0; c < speakers.size() && c < time.size(); ++c) {
            if (speakers[c] != S::kLfe) {
                scale(time[c], c_gain);
            }
        }
        return;
    }
    for (const Speaker front : {S::kLeft, S::kRight, S::kCentre}) {
        if (std::vector<double>* samples = channel(speakers, time, front)) {
            scale(*samples, c_gain);
        }
    }
    // m_gain x 2 x (1/2, 1/2; 1/2, -1/2).
    const double m_gain = scpl ? kSqrt2 : 1.0;
    for (const auto& [first, second] : kCoupled) {
        std::vector<double>* x = channel(speakers, time, first);
        std::vector<double>* y = channel(speakers, time, second);
        if (x == nullptr || y == nullptr) {
            continue;
        }
        const std::size_t n = std::min(x->size(), y->size());
        for (std::size_t i = 0; i < n; ++i) {
            const double sum = (*x)[i] + (*y)[i];
            const double difference = (*x)[i] - (*y)[i];
            (*x)[i] = m_gain * sum;
            (*y)[i] = m_gain * difference;
        }
    }
}

BandGains immersive_gains(int codec_mode, DecodingMode decoding, Speaker speaker) noexcept {
    if (speaker == S::kLfe) {
        return {};
    }
    const bool core = decoding == DecodingMode::kCore;
    switch (codec_mode) {
        case immersive_mode::kAspxScpl: {
            if (core) {
                const bool processed = speaker == S::kLeftSurround ||
                                       speaker == S::kRightSurround || speaker == S::kTopSideLeft ||
                                       speaker == S::kTopSideRight;
                return {.low = 2.0, .high = processed ? 2.0 * kPostProcessing : 2.0};
            }
            const bool front = speaker == S::kLeft || speaker == S::kRight || speaker == S::kCentre;
            const double g = front ? 2.0 : kSqrt2;
            return {.low = g, .high = g};
        }
        case immersive_mode::kAspxAcpl1:
        case immersive_mode::kAspxAcpl2:
            if (core) {
                return {.low = 2.0, .high = 2.0};
            }
            return {};
        default:
            return {};
    }
}

void apply_band_gains(std::span<QmfValue> matrix, int num_ts, int sbx, BandGains gains) noexcept {
    constexpr auto kSubbands = static_cast<std::size_t>(dsp::kQmfSubbands);
    const auto split = static_cast<std::size_t>(std::clamp(sbx, 0, dsp::kQmfSubbands));
    const std::size_t slots =
        std::min(static_cast<std::size_t>(std::max(num_ts, 0)), matrix.size() / kSubbands);
    for (std::size_t ts = 0; ts < slots; ++ts) {
        const std::span<QmfValue> slot = matrix.subspan(ts * kSubbands, kSubbands);
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            slot[sb] *= sb < split ? gains.low : gains.high;
        }
    }
}

}  // namespace ac4::detail
