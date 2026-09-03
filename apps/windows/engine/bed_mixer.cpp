#include "bed_mixer.hpp"

#include <algorithm>
#include <cmath>

namespace ac3::windemo {

namespace {

constexpr float kMinus3dB = 0.70710678F;

}  // namespace

void BedMix::resize(std::size_t frames) {
    for (auto& slot : slots) {
        slot.assign(frames, 0.0F);
    }
}

void BedMix::clear() {
    for (auto& slot : slots) {
        std::ranges::fill(slot, 0.0F);
    }
}

void fold_to_mono(std::span<const float> interleaved, std::uint16_t channels,
                  std::span<float> out) {
    if (channels == 0) {
        std::ranges::fill(out, 0.0F);
        return;
    }
    const std::size_t frames = std::min(out.size(), interleaved.size() / channels);
    switch (channels) {
        case 1:
            std::copy_n(interleaved.begin(), frames, out.begin());
            break;
        case 2:
            for (std::size_t i = 0; i < frames; ++i) {
                out[i] = 0.5F * (interleaved[i * 2] + interleaved[i * 2 + 1]);
            }
            break;
        case 6: {
            // L R C LFE Ls Rs: fronts at 0 dB, centre and surrounds at -3 dB,
            // LFE dropped, normalised for the all-channels-driven case.
            constexpr float kNorm = 1.0F / (2.0F + 3.0F * kMinus3dB);
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 6];
                out[i] = kNorm * (f[0] + f[1] + kMinus3dB * (f[2] + f[4] + f[5]));
            }
            break;
        }
        case 8: {
            // L R C LFE Lss Rss Lrs Rrs.
            constexpr float kNorm = 1.0F / (2.0F + 5.0F * kMinus3dB);
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 8];
                out[i] = kNorm * (f[0] + f[1] + kMinus3dB * (f[2] + f[4] + f[5] + f[6] + f[7]));
            }
            break;
        }
        default: {
            const float norm = 1.0F / static_cast<float>(channels);
            for (std::size_t i = 0; i < frames; ++i) {
                float sum = 0.0F;
                for (std::uint16_t c = 0; c < channels; ++c) {
                    sum += interleaved[i * channels + c];
                }
                out[i] = norm * sum;
            }
            break;
        }
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(frames), out.end(), 0.0F);
}

void fold_to_pair(std::span<const float> interleaved, std::uint16_t channels,
                  std::span<float> left, std::span<float> right) {
    const std::size_t out_frames = std::min(left.size(), right.size());
    if (channels == 0) {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
        return;
    }
    const std::size_t frames = std::min(out_frames, interleaved.size() / channels);
    switch (channels) {
        case 2:
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = interleaved[i * 2];
                right[i] = interleaved[i * 2 + 1];
            }
            break;
        case 6: {
            // L R C LFE Ls Rs: each side is its front, the shared centre and
            // its surround, the latter two at -3 dB.
            constexpr float kNorm = 1.0F / (1.0F + 2.0F * kMinus3dB);
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 6];
                left[i] = kNorm * (f[0] + kMinus3dB * (f[2] + f[4]));
                right[i] = kNorm * (f[1] + kMinus3dB * (f[2] + f[5]));
            }
            break;
        }
        case 8: {
            // L R C LFE Lss Rss Lrs Rrs.
            constexpr float kNorm = 1.0F / (1.0F + 3.0F * kMinus3dB);
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 8];
                left[i] = kNorm * (f[0] + kMinus3dB * (f[2] + f[4] + f[6]));
                right[i] = kNorm * (f[1] + kMinus3dB * (f[2] + f[5] + f[7]));
            }
            break;
        }
        default: {
            fold_to_mono(interleaved, channels, left.subspan(0, frames));
            std::copy_n(left.begin(), frames, right.begin());
            break;
        }
    }
    std::fill(left.begin() + static_cast<std::ptrdiff_t>(frames), left.end(), 0.0F);
    std::fill(right.begin() + static_cast<std::ptrdiff_t>(frames), right.end(), 0.0F);
}

void add_to_bed(std::span<const float> interleaved, std::uint16_t channels, float gain,
                BedMix& bed) {
    if (channels == 0) {
        return;
    }
    auto& l = bed.slots[static_cast<std::size_t>(BedChannel::kL)];
    auto& r = bed.slots[static_cast<std::size_t>(BedChannel::kR)];
    auto& c = bed.slots[static_cast<std::size_t>(BedChannel::kC)];
    auto& ls = bed.slots[static_cast<std::size_t>(BedChannel::kLs)];
    auto& rs = bed.slots[static_cast<std::size_t>(BedChannel::kRs)];
    const std::size_t frames = std::min(l.size(), interleaved.size() / channels);

    switch (channels) {
        case 1:
            for (std::size_t i = 0; i < frames; ++i) {
                const float v = gain * kMinus3dB * interleaved[i];
                l[i] += v;
                r[i] += v;
            }
            break;
        case 2:
            for (std::size_t i = 0; i < frames; ++i) {
                l[i] += gain * interleaved[i * 2];
                r[i] += gain * interleaved[i * 2 + 1];
            }
            break;
        case 4:
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 4];
                l[i] += gain * f[0];
                r[i] += gain * f[1];
                ls[i] += gain * f[2];
                rs[i] += gain * f[3];
            }
            break;
        case 6:
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 6];
                l[i] += gain * f[0];
                r[i] += gain * f[1];
                c[i] += gain * f[2];
                ls[i] += gain * f[4];
                rs[i] += gain * f[5];
            }
            break;
        case 8:
            for (std::size_t i = 0; i < frames; ++i) {
                const float* f = &interleaved[i * 8];
                l[i] += gain * f[0];
                r[i] += gain * f[1];
                c[i] += gain * f[2];
                ls[i] += gain * kMinus3dB * (f[4] + f[6]);
                rs[i] += gain * kMinus3dB * (f[5] + f[7]);
            }
            break;
        default: {
            const float norm = gain * kMinus3dB / static_cast<float>(channels);
            for (std::size_t i = 0; i < frames; ++i) {
                float sum = 0.0F;
                for (std::uint16_t ch = 0; ch < channels; ++ch) {
                    sum += interleaved[i * channels + ch];
                }
                l[i] += norm * sum;
                r[i] += norm * sum;
            }
            break;
        }
    }
}

}  // namespace ac3::windemo
