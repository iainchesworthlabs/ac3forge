#pragma once

// The AC-4 half of the performance suite's inputs: the encoder configurations
// the AC-4 workloads share, the real-audio fixture cut into AC-4 frames, and
// the stream the decode workloads read. Shared by ac3perf, ac3bench and
// ac3membench so a decode number is always read against the configuration
// whose encode number sits beside it; each binary keeps its own timing and
// counting, as it does for AC-3.

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac4enc/encoder.hpp"
#include "real_audio.hpp"

namespace perf::ac4_bench {

// Part 1 Table 83: frame_rate_index 13 is the 2 048-sample frame at 48 kHz,
// the one rate that needs no sample-rate converter, so one encode() call of
// 2 048 samples per channel is one frame's real-time budget.
inline constexpr int kFrameRateIndex = 13;
inline constexpr std::size_t kSamplesPerFrame = 2048;

// Indices into the WAV's own channel order (FL FR FC LFE BL BR), which is
// already the order iclforge::ac4::Encoder takes (L R C LFE Ls Rs) - unlike
// perf::FrameSource, no permutation to A/52 Table 5.8 order.
inline constexpr std::array<std::size_t, 2> kStereoChannels = {0, 1};
inline constexpr std::array<std::size_t, 6> kFiveOneChannels = {0, 1, 2, 3, 4, 5};

inline ::iclforge::ac4::EncoderConfig stereo_config() {
    return {.channels = 2, .frame_rate_index = kFrameRateIndex, .bitrate_kbps = 192};
}

inline ::iclforge::ac4::EncoderConfig five_one_config() {
    return {.channels = 6, .frame_rate_index = kFrameRateIndex, .bitrate_kbps = 448};
}

// The fixture in AC-4 frames, wrapping at its end the way perf::FrameSource
// does, so every frame of a 200-frame run is real audio.
class FrameSource {
public:
    FrameSource(const iclforge::io::WavData& wav, std::span<const std::size_t> channels) {
        ordered_.reserve(channels.size());
        for (const std::size_t ch : channels) {
            ordered_.push_back(&wav.channels[ch % wav.channels.size()]);
        }
        views_.resize(channels.size());
        available_ = wav.frame_count() / kSamplesPerFrame;
    }

    // Views of frame `index`'s samples, one per channel. Valid until the next
    // call.
    [[nodiscard]] std::span<const std::span<const float>> frame(std::size_t index) {
        const std::size_t offset = (index % available_) * kSamplesPerFrame;
        for (std::size_t ch = 0; ch < views_.size(); ++ch) {
            views_[ch] = std::span<const float>{*ordered_[ch]}.subspan(offset, kSamplesPerFrame);
        }
        return views_;
    }

private:
    std::vector<const std::vector<float>*> ordered_;
    std::vector<std::span<const float>> views_;
    std::size_t available_ = 0;
};

// The raw_ac4_frame()s `frames` frames of input encode to, flushed so the
// encoder's delay holds none back. Empty if the encoder refuses.
inline std::vector<std::vector<std::byte>> encode_frames(FrameSource& source,
                                                         const ::iclforge::ac4::EncoderConfig& config,
                                                         int frames) {
    auto encoder = ::iclforge::ac4::Encoder::create(config);
    if (!encoder) {
        return {};
    }
    std::vector<std::vector<std::byte>> out;
    out.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        auto coded = encoder->encode(source.frame(static_cast<std::size_t>(i)));
        if (!coded) {
            return {};
        }
        for (auto& frame : *coded) {
            out.push_back(std::move(frame.raw_ac4_frame));
        }
    }
    auto tail = encoder->flush();
    if (!tail) {
        return {};
    }
    for (auto& frame : *tail) {
        out.push_back(std::move(frame.raw_ac4_frame));
    }
    return out;
}

}  // namespace perf::ac4_bench
